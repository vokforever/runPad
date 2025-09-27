#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <vector>
#include <Adafruit_NeoPixel.h>
#include "config.h"
#include <ESPAsyncWebServer.h>

bool RAW = false;

// Variables for workout start delay
unsigned long workoutStartDelay = 5000; // 5 секунд задержка после старта
unsigned long actualWorkoutStartTime = 0;

// NeoPixel настройки
#define NEOPIXEL_PIN 48     // GPIO48 для ESP32-S3
#define NEOPIXEL_COUNT 1    // Количество светодиодов
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// NeoPixel цвета для состояний
#define COLOR_OFF         pixels.Color(0, 0, 0)
#define COLOR_STANDBY     pixels.Color(0, 0, 50)      // Синий - ожидание
#define COLOR_ACTIVE      pixels.Color(0, 255, 0)     // Зелёный - активная тренировка
#define COLOR_SENDING     pixels.Color(255, 165, 0)   // Оранжевый - отправка данных
#define COLOR_SUCCESS     pixels.Color(0, 255, 255)   // Голубой - успешная отправка
#define COLOR_ERROR       pixels.Color(255, 0, 0)     // Красный - ошибка
#define COLOR_CONNECTING  pixels.Color(128, 0, 128)   // Фиолетовый - подключение
#define COLOR_WIFI_ERROR  pixels.Color(255, 255, 0)   // Жёлтый - проблемы с WiFi

// NTP сервера
const char* ntpServer1 = "ntp2.vniiftri.ru";
const char* ntpServer2 = "ntp.ix.ru";
const char* ntpServer3 = "ntp.msk-ix.ru";
const long gmtOffset_sec = 3 * 3600;     // МСК = UTC+3
const int daylightOffset_sec = 0;

// Настройки буфера
const size_t MAX_BUFFER_SIZE = 200;
const unsigned long STANDBY_TIMEOUT = 10000;
const unsigned long CONNECTION_CHECK_INTERVAL = 5000;

// Пороги активности
const float MIN_ACTIVITY_SPEED = 0.8;    // 0.8 км/ч для расчета дистанции
const float MIN_WORKOUT_SPEED = 0.5;     // 0.5 км/ч для начала тренировки

BLEAddress treadmillAddress(TREADMILL_MAC);
bool connected = false;
bool wifiConnected = false;

AsyncWebServer webServer(80);
float webCurrentSpeed = 0.0;
uint32_t webCurrentDistance = 0;
uint16_t webCurrentTime = 0;
String webCurrentState = "STANDBY";
time_t webSessionDuration = 0;
unsigned long lastWebUpdate = 0;
const unsigned long WEB_UPDATE_INTERVAL = 2000;

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pTreadmillData = nullptr;

// HTTP Task components
TaskHandle_t httpTaskHandle = nullptr;
QueueHandle_t workoutQueue;

enum WorkoutState {
  STANDBY,
  ACTIVE,
  WORKOUT_ENDED
};

WorkoutState currentState = STANDBY;
WorkoutState previousState = STANDBY;

enum LEDState {
  LED_STANDBY,
  LED_ACTIVE,
  LED_SENDING,
  LED_SUCCESS,
  LED_ERROR,
  LED_CONNECTING,
  LED_WIFI_ERROR,
  LED_BLINK
};

LEDState currentLEDState = LED_STANDBY;
unsigned long lastLEDUpdate = 0;
bool blinkState = false;

struct WorkoutRecord {
  time_t timestamp;
  float speed;
  uint32_t distance;
  uint16_t time;
  bool isActive;
};

struct WorkoutData {
  std::vector<WorkoutRecord> buffer;
  time_t startTime;
  time_t endTime;
};

std::vector<WorkoutRecord> workoutBuffer;
time_t workoutStartTime = 0;
time_t workoutEndTime = 0;
unsigned long lastActiveTime = 0;
unsigned long lastConnectionCheck = 0;
unsigned long workoutEndTime_millis = 0;
const unsigned long WORKOUT_COOLDOWN = 10000;

// Глобальные переменные для расчета дистанции
float totalDistance = 0.0;
unsigned long lastTimeUpdate = 0;
unsigned long sessionStartTime = 0;

// Конфиг для расчета калорий
const int USER_HEIGHT = 193;
const int USER_WEIGHT = 110;
const bool USER_MALE = true;

// FORWARD DECLARATIONS
void sendWorkoutToSupabaseFromTask(WorkoutData* data);
String createOptimizedWorkoutJson(const std::vector<WorkoutRecord>& buffer, time_t startTime, time_t endTime);
String getISOTimestamp(time_t timeValue);
String getReadableTime(time_t timeValue);
void sendWorkoutToSupabase();
void updateWorkoutState(const WorkoutRecord& record);
void updateNeoPixel();
void setLEDState(LEDState newState);

// Функция управления NeoPixel
void updateNeoPixel() {
  unsigned long currentTime = millis();
  
  // Обновляем мигание каждые 500мс
  if (currentTime - lastLEDUpdate > 500) {
    blinkState = !blinkState;
    lastLEDUpdate = currentTime;
  }
  
  uint32_t color = COLOR_OFF;
  
  switch (currentLEDState) {
    case LED_STANDBY:
      color = COLOR_STANDBY;
      break;
      
    case LED_ACTIVE:
      color = COLOR_ACTIVE;
      break;
      
    case LED_SENDING:
      color = COLOR_SENDING;
      break;
      
    case LED_SUCCESS:
      color = COLOR_SUCCESS;
      break;
      
    case LED_ERROR:
      color = COLOR_ERROR;
      break;
      
    case LED_WIFI_ERROR:
      color = COLOR_WIFI_ERROR;
      break;
      
    case LED_CONNECTING:
      color = blinkState ? COLOR_CONNECTING : COLOR_OFF;
      break;
      
    case LED_BLINK:
      color = blinkState ? COLOR_STANDBY : COLOR_OFF;
      break;
  }
  
  pixels.setPixelColor(0, color);
  pixels.show();
}

// Установка состояния LED
void setLEDState(LEDState newState) {
  if (currentLEDState != newState) {
    currentLEDState = newState;
    Serial0.printf("LED State: %s\n", 
      newState == LED_STANDBY ? "STANDBY" :
      newState == LED_ACTIVE ? "ACTIVE" :
      newState == LED_SENDING ? "SENDING" :
      newState == LED_SUCCESS ? "SUCCESS" :
      newState == LED_ERROR ? "ERROR" :
      newState == LED_WIFI_ERROR ? "WIFI_ERROR" :
      newState == LED_CONNECTING ? "CONNECTING" : "BLINK");
  }
}

const char* webPageHTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Тренировочный монитор ESP32-S3</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 20px;
            background-color: #f0f0f0;
        }
        .container {
            max-width: 600px;
            margin: 0 auto;
            background: white;
            border-radius: 10px;
            padding: 20px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        .header {
            text-align: center;
            color: #333;
            margin-bottom: 30px;
        }
        .metric {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 15px;
            margin: 10px 0;
            background: #f8f9fa;
            border-radius: 8px;
            border-left: 4px solid #007bff;
        }
        .metric.active {
            border-left-color: #28a745;
            background: #d4edda;
        }
        .metric-label {
            font-weight: bold;
            color: #495057;
        }
        .metric-value {
            font-size: 24px;
            font-weight: bold;
            color: #212529;
        }
        .status {
            text-align: center;
            padding: 10px;
            border-radius: 5px;
            margin: 20px 0;
            font-weight: bold;
        }
        .status.standby { background: #cce7ff; color: #0066cc; }
        .status.active { background: #ccffcc; color: #006600; }
        .status.ended { background: #ffffcc; color: #cc6600; }
        .update-time {
            text-align: center;
            color: #666;
            font-size: 14px;
            margin-top: 20px;
        }
        .progress {
            width: 100%;
            height: 10px;
            background: #e0e0e0;
            border-radius: 5px;
            overflow: hidden;
            margin: 10px 0;
        }
        .progress-bar {
            height: 100%;
            background: linear-gradient(90deg, #007bff, #28a745);
            width: 0%;
            transition: width 0.3s ease;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1 class="header">🏃 Тренировочный Монитор</h1>
        
        <div id="status" class="status standby">ОЖИДАНИЕ</div>
        
        <div id="speed-metric" class="metric">
            <span class="metric-label">Скорость:</span>
            <span class="metric-value"><span id="speed">0.0</span> км/ч</span>
        </div>
        
        <div id="distance-metric" class="metric">
            <span class="metric-label">Дистанция:</span>
            <span class="metric-value"><span id="distance">0</span> м</span>
        </div>
        
        <div id="time-metric" class="metric">
            <span class="metric-label">Время тренировки:</span>
            <span class="metric-value"><span id="duration">00:00</span></span>
        </div>
        
        <div class="progress">
            <div id="progress-bar" class="progress-bar"></div>
        </div>
        
        <div class="update-time">
            Последнее обновление: <span id="lastUpdate">-</span>
        </div>
    </div>

    <script>
        function updateData() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('speed').textContent = data.speed;
                    document.getElementById('distance').textContent = data.distance;
                    document.getElementById('duration').textContent = formatTime(data.duration);
                    
                    const statusEl = document.getElementById('status');
                    statusEl.textContent = data.state;
                    statusEl.className = 'status ' + data.state.toLowerCase();
                    
                    const speedMetric = document.getElementById('speed-metric');
                    const distanceMetric = document.getElementById('distance-metric');
                    const timeMetric = document.getElementById('time-metric');
                    
                    if (data.state === 'ACTIVE') {
                        speedMetric.classList.add('active');
                        distanceMetric.classList.add('active');
                        timeMetric.classList.add('active');
                        
                        const progress = Math.min((data.speed / 15) * 100, 100);
                        document.getElementById('progress-bar').style.width = progress + '%';
                    } else {
                        speedMetric.classList.remove('active');
                        distanceMetric.classList.remove('active');
                        timeMetric.classList.remove('active');
                        document.getElementById('progress-bar').style.width = '0%';
                    }
                    
                    document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString('ru-RU');
                })
                .catch(error => {
                    console.error('Ошибка получения данных:', error);
                });
        }
        
        function formatTime(seconds) {
            const mins = Math.floor(seconds / 60);
            const secs = seconds % 60;
            return mins.toString().padStart(2, '0') + ':' + secs.toString().padStart(2, '0');
        }
        
        setInterval(updateData, 3000); // Обновляем каждые 3 секунды вместо 2
        updateData();
    </script>
</body>
</html>
)rawliteral";

// Функция проверки валидности времени
bool isTimeValid(time_t timeValue) {
  const time_t MIN_VALID_TIME = 1577836800; // 1 января 2020
  const time_t MAX_VALID_TIME = 1893456000; // 1 января 2030
  
  return (timeValue >= MIN_VALID_TIME && timeValue <= MAX_VALID_TIME);
}

// Попытка переподключения WiFi
void reconnectWiFi() {
  Serial0.println("Attempting WiFi reconnection...");
  setLEDState(LED_CONNECTING);
  
  WiFi.disconnect();
  delay(1000);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(1000);
    Serial0.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial0.println("\nWiFi reconnected!");
    wifiConnected = true;
    setLEDState(LED_SUCCESS);
    delay(1000);
    
    // Пересинхронизируем время после подключения WiFi
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
  } else {
    Serial0.println("\nWiFi reconnection failed");
    wifiConnected = false;
    setLEDState(LED_WIFI_ERROR);
  }
}

// Тестирование подключения к Supabase с правильной структурой
void testSupabaseConnection() {
  if (!wifiConnected) {
    Serial0.println("No WiFi for connection test");
    setLEDState(LED_WIFI_ERROR);
    return;
  }
  
  Serial0.println("Testing Supabase connection...");
  setLEDState(LED_CONNECTING);
  
  HTTPClient http;
  // Тестируем структуру таблицы
  String testUrl = String(SUPABASE_URL) + "/rest/v1/workouts?select=workout_start,workout_end,duration_seconds,total_distance,max_speed,avg_speed,records_count,device_name&limit=1";
  
  http.begin(testUrl);
  http.setTimeout(10000);
  
http.addHeader("Content-Type", "application/json");
http.addHeader("apikey", SUPABASE_KEY);
http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  
  int responseCode = http.GET();
  String response = "";
  
  if (responseCode > 0 && http.getSize() > 0 && http.getSize() < 1000) {
    response = http.getString();
  }
  
  Serial0.printf("Test response code: %d\n", responseCode);
  
  if (responseCode == 200) {
    Serial0.println("✓ Supabase connection OK!");
    Serial0.println("✓ Table structure accessible");
    setLEDState(LED_SUCCESS);
    delay(2000);
    setLEDState(LED_STANDBY);
  } else {
    Serial0.printf("✗ Supabase connection failed: %d\n", responseCode);
    setLEDState(LED_ERROR);
    if (response.length() > 0 && response.length() < 200) {
      Serial0.println("Error response: " + response);
    }
    
    if (responseCode == 401) {
      Serial0.println("401 Error: API key invalid or insufficient permissions");
      Serial0.println("Make sure you're using service_role key for write operations");
    } else if (responseCode == 404) {
      Serial0.println("404 Error: Table 'workouts' not found or inaccessible");
    }
    
    delay(3000);
    setLEDState(LED_STANDBY);
  }
  
  http.end();
}

// Расчет калорий на основе MET значений
float calculateCalories(float avgSpeed, int durationSeconds) {
  if (durationSeconds <= 0) return 0.0;
  
  float hours = durationSeconds / 3600.0;
  float met = 1.0;
  
  // MET значения для разных скоростей (км/ч)
  if (avgSpeed < 1.0) {
    met = 2.0; // очень медленная ходьба
  } else if (avgSpeed < 4.0) {
    met = 3.5; // медленная ходьба
  } else if (avgSpeed < 6.0) {
    met = 4.5; // обычная ходьба
  } else if (avgSpeed < 8.0) {
    met = 6.0; // быстрая ходьба
  } else if (avgSpeed < 10.0) {
    met = 8.0; // легкий бег
  } else if (avgSpeed < 12.0) {
    met = 10.0; // умеренный бег
  } else {
    met = 11.5; // быстрый бег
  }
  
  float calories = met * USER_WEIGHT * hours;
  return calories;
}

// Функция получения времени в формате ISO 8601
String getISOTimestamp(time_t timeValue) {
  struct tm timeinfo;
  localtime_r(&timeValue, &timeinfo);
  
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+03:00", &timeinfo);
  return String(buffer);
}

// Функция получения читаемого времени
String getReadableTime(time_t timeValue) {
  struct tm timeinfo;
  localtime_r(&timeValue, &timeinfo);
  
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%d.%m.%y в %H:%M", &timeinfo);
  return String(buffer);
}

// Создание JSON с правильной структурой таблицы workouts
String createOptimizedWorkoutJson(const std::vector<WorkoutRecord>& buffer, time_t startTime, time_t endTime) {
  if (buffer.empty()) return "{}";
  
  WorkoutRecord finalRecord = buffer.back();
  
  float maxSpeed = 0.0;
  float totalSpeed = 0.0;
  int activeRecords = 0;
  
  for (const auto& record : buffer) {
    if (record.speed > maxSpeed) maxSpeed = record.speed;
    if (record.speed > 0.1) {
      totalSpeed += record.speed;
      activeRecords++;
    }
  }
  
  float avgSpeed = activeRecords > 0 ? totalSpeed / activeRecords : 0.0;
  long duration = endTime - startTime;
  
  // JSON с полной структурой как в таблице (без id и created_at - они автогенерируются)
  String json;
  json.reserve(400);
  
  json = "{\"workout_start\":\"" + getISOTimestamp(startTime) + 
         "\",\"workout_end\":\"" + getISOTimestamp(endTime) + 
         "\",\"duration_seconds\":" + String(duration) + 
         ",\"total_distance\":" + String(finalRecord.distance) + 
         ",\"max_speed\":" + String(maxSpeed, 1) + 
         ",\"avg_speed\":" + String(avgSpeed, 1) + 
         ",\"records_count\":" + String(buffer.size()) + 
         ",\"device_name\":\"ESP32_S3_Treadmill_Logger\"}";
  
  return json;
}

void sendWorkoutToSupabaseFromTask(WorkoutData* data) {
  if (data->buffer.empty()) {
    Serial0.println("No workout data to send");
    setLEDState(LED_ERROR);
    return;
  }

  if (!wifiConnected) {
    Serial0.println("No WiFi - attempting reconnection for workout upload");
    reconnectWiFi();
    if (!wifiConnected) {
      Serial0.println("Still no WiFi - skipping workout upload");
      setLEDState(LED_WIFI_ERROR);
      return;
    }
  }

  if (!isTimeValid(data->startTime) || !isTimeValid(data->endTime)) {
    Serial0.printf("Invalid timestamps - Start: %ld, End: %ld\n", data->startTime, data->endTime);
    setLEDState(LED_ERROR);
    return;
  }

  long duration = data->endTime - data->startTime;
  if (duration < 30 || duration > 86400) {
    Serial0.printf("Invalid workout duration: %ld seconds\n", duration);
    setLEDState(LED_ERROR);
    return;
  }
  
  setLEDState(LED_SENDING);
  
  Serial0.printf("Sending workout: %s - %s (Duration: %ld sec)\n",
                getReadableTime(data->startTime).c_str(),
                getReadableTime(data->endTime).c_str(),
                duration);
  Serial0.printf("Buffer size: %d records\n", data->buffer.size());
  
  HTTPClient http;
  String fullUrl = String(SUPABASE_URL) + "/rest/v1/workouts";
  http.begin(fullUrl);

  http.setTimeout(10000);
  http.setConnectTimeout(5000);
  http.setReuse(false);
  delay(10);
  
  // Правильные заголовки для Supabase REST API
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);  // apikey заголовок НУЖЕН
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");
  
  // Создаем JSON с правильной структурой
  String jsonPayload = createOptimizedWorkoutJson(data->buffer, data->startTime, data->endTime);
  
  Serial0.println("=== SUPABASE REQUEST DEBUG ===");
  Serial0.println("URL: " + fullUrl);
  Serial0.printf("Using API key: %.30s...\n", SUPABASE_KEY);
  Serial0.printf("JSON size: %d bytes\n", jsonPayload.length());
  Serial0.println("JSON payload: " + jsonPayload);
  Serial0.println("Expected fields: workout_start, workout_end, duration_seconds, total_distance, max_speed, avg_speed, records_count, device_name");
  Serial0.println("===============================");

  int httpResponse = -1;
  int attempts = 0;
  const int maxAttempts = 3;

  while (httpResponse <= 0 && attempts < maxAttempts) {
    attempts++;
    Serial0.printf("Attempt %d/%d...\n", attempts, maxAttempts);
    
    httpResponse = http.POST(jsonPayload);
    
    if (httpResponse <= 0) {
      Serial0.printf("HTTP error on attempt %d: %d\n", attempts, httpResponse);
      if (attempts < maxAttempts) {
        delay(2000 * attempts);
      }
    }
  }
  
  String response = "";
  if (httpResponse > 0) {
    if (http.getSize() > 0 && http.getSize() < 2000) {
      response = http.getString();
    }
    
    Serial0.printf("=== SUPABASE RESPONSE ===\n");
    Serial0.printf("Response code: %d\n", httpResponse);
    
    if (response.length() > 0) {
      Serial0.println("Response body: " + response);
    }
    Serial0.println("========================\n");
    
    if (httpResponse == 200 || httpResponse == 201) {
      Serial0.println("✓ Workout sent successfully!");
      setLEDState(LED_SUCCESS);
      delay(1000);
      setLEDState(LED_STANDBY);
    } else {
      Serial0.printf("✗ HTTP error. Code: %d\n", httpResponse);
      setLEDState(LED_ERROR);
      
      // Детальная диагностика для 400 ошибки
      if (httpResponse == 400) {
        Serial0.println("400 Bad Request analysis:");
        Serial0.println("- Checking JSON structure matches table schema");
        Serial0.println("- Required fields: workout_start, workout_end, duration_seconds, total_distance, max_speed, avg_speed, records_count, device_name");
        Serial0.println("- Auto fields (excluded): id, created_at");
        
        if (response.indexOf("duplicate") >= 0) {
          Serial0.println("- Possible duplicate key violation");
        }
        if (response.indexOf("constraint") >= 0) {
          Serial0.println("- Database constraint violation");
        }
        if (response.indexOf("permission") >= 0 || response.indexOf("policy") >= 0) {
          Serial0.println("- Permission/RLS policy issue - check service_role key");
        }
      } else if (httpResponse == 401) {
        Serial0.println("401 Unauthorized - Check API key permissions");
      } else if (httpResponse == 403) {
        Serial0.println("403 Forbidden - Check RLS policies for INSERT operation");
      }
      
      delay(2000);
      setLEDState(LED_STANDBY);
    }
  } else {
    Serial0.printf("✗ Connection failed. Error code: %d\n", httpResponse);
    setLEDState(LED_ERROR);
    
    // Диагностика сетевых ошибок
    switch (httpResponse) {
      case HTTPC_ERROR_CONNECTION_REFUSED:
        Serial0.println("Error: Connection refused - check URL");
        break;
      case HTTPC_ERROR_SEND_HEADER_FAILED:
        Serial0.println("Error: Failed to send headers");
        break;
      case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
        Serial0.println("Error: Failed to send JSON payload");
        break;
      case HTTPC_ERROR_NOT_CONNECTED:
        Serial0.println("Error: Not connected to WiFi");
        break;
      case HTTPC_ERROR_CONNECTION_LOST:
        Serial0.println("Error: WiFi connection lost during request");
        break;
      case HTTPC_ERROR_READ_TIMEOUT:
        Serial0.println("Error: Server response timeout");
        break;
      default:
        Serial0.printf("Error: Network error code %d\n", httpResponse);
    }
    
    delay(2000);
    setLEDState(LED_STANDBY);
  }

  http.end();
}

// HTTP задача
void httpTask(void* parameter) {
  WorkoutData* data = nullptr;
  
  Serial0.println("HTTP Task started");
  
  while (true) {
    if (xQueueReceive(workoutQueue, &data, pdMS_TO_TICKS(30000))) {
      Serial0.println("Processing workout data...");
      
      Serial0.printf("Free heap before HTTP: %d bytes\n", ESP.getFreeHeap());
      
      sendWorkoutToSupabaseFromTask(data);
      
      delete data;
      data = nullptr;
      
      Serial0.printf("Free heap after HTTP: %d bytes\n", ESP.getFreeHeap());
    } else {
      // Queue timeout - check if LED is stuck in SENDING state
      if (currentLEDState == LED_SENDING) {
        Serial0.println(">>> Queue timeout detected - resetting LED to STANDBY");
        setLEDState(LED_STANDBY);
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Безопасная отправка в Supabase через очередь
void sendWorkoutToSupabase() {
  if (workoutBuffer.empty()) {
    Serial0.println("No workout data to send");
    return;
  }
  
  const int MIN_MEMORY_FOR_HTTP = 20000;
  if (ESP.getFreeHeap() < MIN_MEMORY_FOR_HTTP) {
    Serial0.printf("Low memory for HTTP - skipping. Free: %d, Required: %d\n",
                  ESP.getFreeHeap(), MIN_MEMORY_FOR_HTTP);
    setLEDState(LED_ERROR);
    delay(3000);
    setLEDState(LED_STANDBY);
    return;
  }

  Serial0.printf("Memory check OK: %d bytes free\n", ESP.getFreeHeap());
  Serial0.println(">>> PREPARING TO SEND TO SUPABASE!");
  setLEDState(LED_SENDING);
  
  WorkoutData* data = new WorkoutData();
  data->buffer = workoutBuffer;
  data->startTime = workoutStartTime;
  data->endTime = workoutEndTime;
  
  if (xQueueSend(workoutQueue, &data, pdMS_TO_TICKS(100)) == pdTRUE) {
    workoutBuffer.clear();
    Serial0.println(">>> Workout queued for sending");
  } else {
    delete data;
    Serial0.println(">>> Failed to queue workout - queue full");
    setLEDState(LED_ERROR);
    delay(2000);
    setLEDState(LED_STANDBY);
  }
}

void updateWorkoutState(const WorkoutRecord& record) {
  previousState = currentState;
  
  // Понижены пороги активности для обнаружения медленной ходьбы
  bool isCurrentlyActive = (record.speed >= MIN_WORKOUT_SPEED && record.time > 0);

  if (isCurrentlyActive) {
    lastActiveTime = millis();
    if (currentState == STANDBY && (millis() - workoutEndTime_millis > WORKOUT_COOLDOWN)) {
      currentState = ACTIVE;
      setLEDState(LED_ACTIVE);
      
      time_t currentTime = time(nullptr);
      
      if (isTimeValid(currentTime)) {
        workoutStartTime = currentTime;
        actualWorkoutStartTime = millis();
        Serial0.println(">>> WORKOUT START DELAY: 5 seconds before counting");
        Serial0.printf(">>> WORKOUT STARTED at %s! Speed: %.1f km/h\n",
                       getReadableTime(workoutStartTime).c_str(), record.speed);
      } else {
        Serial0.printf(">>> WARNING: Invalid time detected (%ld), waiting for sync...\n", currentTime);
        currentState = STANDBY;
        setLEDState(LED_STANDBY);
        return;
      }
      
      totalDistance = 0.0;
      sessionStartTime = millis();
      lastTimeUpdate = millis();
    }
  } else {
    unsigned long inactiveTime = millis() - lastActiveTime;
    
    if (currentState == ACTIVE && record.speed < 0.1 && inactiveTime > 15000) {
      Serial0.printf(">>> Ending workout due to zero speed for 15+ seconds\n");
      
      time_t currentTime = time(nullptr);
      
      if (isTimeValid(currentTime) && isTimeValid(workoutStartTime)) {
        workoutEndTime = currentTime;
        
        long duration = workoutEndTime - workoutStartTime;
        if (duration < 30 || duration > 86400) {
          Serial0.printf(">>> WARNING: Invalid workout duration (%ld sec), skipping save\n", duration);
          currentState = STANDBY;
          setLEDState(LED_STANDBY);
          workoutBuffer.clear();
          totalDistance = 0.0;
          return;
        }
        
        currentState = WORKOUT_ENDED;
        workoutEndTime_millis = millis();
        Serial0.printf(">>> WORKOUT ENDED at %s! Duration: %ld seconds\n",
                       getReadableTime(workoutEndTime).c_str(), duration);
      } else {
        Serial0.printf(">>> WARNING: Invalid time for workout end, discarding workout\n");
        currentState = STANDBY;
        setLEDState(LED_STANDBY);
        workoutBuffer.clear();
        totalDistance = 0.0;
        return;
      }
    }
  }
  
  if (previousState == ACTIVE && currentState == WORKOUT_ENDED) {
    Serial0.println(">>> Starting workout upload process...");
    setLEDState(LED_SENDING);
    Serial0.println(">>> LED set to SENDING state");
    sendWorkoutToSupabase();
    Serial0.println(">>> sendWorkoutToSupabase() completed");
    currentState = STANDBY;
    Serial0.println(">>> State changed to STANDBY");
  }
}

void addToBuffer(const WorkoutRecord& record) {
  static WorkoutRecord lastRecord = {0};
  
  if (!isTimeValid(record.timestamp)) {
    Serial0.printf("Skipping record with invalid timestamp: %ld\n", record.timestamp);
    return;
  }
  
  bool shouldAdd = (record.distance != lastRecord.distance ||
                   record.speed != lastRecord.speed ||
                   record.time != lastRecord.time);
  
  if (shouldAdd && currentState == ACTIVE && (millis() - actualWorkoutStartTime > workoutStartDelay)) {
    if (workoutBuffer.size() >= MAX_BUFFER_SIZE) {
      workoutBuffer.erase(workoutBuffer.begin());
    }
    
    workoutBuffer.push_back(record);
    lastRecord = record;
    
    Serial0.printf("Buffer: %d, State: %s, Free RAM: %d\n", 
                  workoutBuffer.size(), 
                  currentState == STANDBY ? "STANDBY" : 
                  currentState == ACTIVE ? "ACTIVE" : "ENDED",
                  ESP.getFreeHeap());
  }
}

void treadmillDataCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (length < 8) return;
  
  static std::vector<uint8_t> lastData;
  std::vector<uint8_t> currentDataVec(pData, pData + length);
  
  // Выводим RAW DATA только при изменении данных
  if (RAW && currentDataVec != lastData) {
    Serial0.print("RAW DATA: ");
    for (size_t i = 0; i < length; i++) {
      Serial0.printf("%02X ", pData[i]);
    }
    Serial0.println();
    
    Serial0.printf("Analysis: Flags=0x%04X, Speed=0x%04X (%d), Distance=0x%06X, Time=0x%04X (%d)\n",
                  pData[0] | (pData[1] << 8),
                  pData[2] | (pData[3] << 8), pData[2] | (pData[3] << 8),
                  length >= 7 ? (pData[4] | (pData[5] << 8) | (pData[6] << 16)) : 0,
                  length >= 18 ? (pData[16] | (pData[17] << 8)) : 0,
                  length >= 18 ? (pData[16] | (pData[17] << 8)) : 0);
  }
  
  lastData = currentDataVec;
  
  WorkoutRecord newRecord;
  newRecord.timestamp = time(nullptr);
  newRecord.speed = 0.0;
  newRecord.distance = 0;
  newRecord.time = 0;
  
  // Парсинг скорости
  if (length >= 4) {
    uint16_t speedRaw = pData[2] | (pData[3] << 8);
    newRecord.speed = speedRaw / 100.0;
    
    // Убрали фильтр низких скоростей для поддержки медленной ходьбы
  }
  
  // Парсинг времени
  if (length >= 18) {
    uint16_t timeRaw = pData[16] | (pData[17] << 8);
    newRecord.time = timeRaw;
  }
  
  // Вычисление дистанции на основе скорости и времени
  if (newRecord.speed >= MIN_ACTIVITY_SPEED && lastTimeUpdate > 0 && currentState == ACTIVE && (millis() - actualWorkoutStartTime > workoutStartDelay)) {
    unsigned long timeInterval = millis() - lastTimeUpdate;
    if (timeInterval > 100 && timeInterval < 10000) {
      float distanceInterval = (newRecord.speed / 3.6) * (timeInterval / 1000.0);
      totalDistance += distanceInterval;
      
      Serial0.printf("CALCULATED: Interval=%.1fs, Distance+=%.2fm, Total=%.1fm\n",
                    timeInterval / 1000.0, distanceInterval, totalDistance);
    }
  }
  
  if (newRecord.speed >= MIN_ACTIVITY_SPEED && currentState == ACTIVE && (millis() - actualWorkoutStartTime > workoutStartDelay)) {
    lastTimeUpdate = millis();
  }
  
  newRecord.distance = (uint32_t)totalDistance;
  
  if (newRecord.speed > 25.0) {
    newRecord.speed = 0.0;
  }
  
  newRecord.isActive = (newRecord.speed >= MIN_ACTIVITY_SPEED && newRecord.time > 0);

  // Обновляем данные для веб-интерфейса только раз в 2 секунды
  unsigned long currentTimeMs = millis();
  if (currentTimeMs - lastWebUpdate > WEB_UPDATE_INTERVAL) {
    webCurrentSpeed = newRecord.speed;
    webCurrentDistance = newRecord.distance;
    webCurrentTime = newRecord.time;
    webCurrentState = (currentState == STANDBY ? "STANDBY" :
                      (currentState == ACTIVE ? "ACTIVE" : "ENDED"));
    
    if (currentState == ACTIVE && workoutStartTime > 0) {
      webSessionDuration = time(nullptr) - workoutStartTime;
    } else {
      webSessionDuration = 0;
    }
    
    lastWebUpdate = currentTimeMs;
  }
  
  updateWorkoutState(newRecord);
  addToBuffer(newRecord);
  
  // Выводим основную информацию
  static WorkoutRecord lastDisplayed = {0};
  static bool wasActive = false;
  bool isActive = (newRecord.speed >= MIN_ACTIVITY_SPEED);
  
  if ((isActive != wasActive) ||
      (isActive && (newRecord.speed != lastDisplayed.speed ||
                    newRecord.distance != lastDisplayed.distance)) ||
      currentState != previousState) {
    
    Serial0.printf("STATE: %s, Speed: %.1f km/h, Total Distance: %d m, Time: %d s\n",
                  currentState == STANDBY ? "STANDBY" :
                  currentState == ACTIVE ? "ACTIVE" : "ENDED",
                  newRecord.speed, newRecord.distance, newRecord.time);
    
    lastDisplayed = newRecord;
    wasActive = isActive;
  }
}

void setup() {
  Serial0.begin(115200);
  delay(3000);
  
  // Инициализация NeoPixel
  pixels.begin();
  pixels.clear();
  pixels.show();
  setLEDState(LED_CONNECTING);
  
  Serial0.println("ESP32-S3 Treadmill Logger v3.2 - Fixed Supabase Structure");
  RAW = false; // для включения RAW данных
  Serial0.printf("Activity thresholds: MIN_WORKOUT=%.1f km/h, MIN_ACTIVITY=%.1f km/h\n", 
                 MIN_WORKOUT_SPEED, MIN_ACTIVITY_SPEED);
  workoutEndTime_millis = millis();
  Serial0.printf("Free heap at start: %d bytes\n", ESP.getFreeHeap());
  
  // Создание HTTP задачи и очереди
  Serial0.println("Creating HTTP task and queue...");
  workoutQueue = xQueueCreate(3, sizeof(WorkoutData*));
  
  if (workoutQueue == nullptr) {
    Serial0.println("Failed to create workout queue!");
    setLEDState(LED_ERROR);
    return;
  }
  
  BaseType_t result = xTaskCreatePinnedToCore(
    httpTask,
    "HTTP_Task",
    20480,
    nullptr,
    2,
    &httpTaskHandle,
    0
  );
  
  if (result != pdPASS) {
    Serial0.println("Failed to create HTTP task!");
    setLEDState(LED_ERROR);
    return;
  }

  // Понижаем приоритет веб-сервера чтобы не мешал BLE
  vTaskPrioritySet(NULL, 1); // Понижаем приоритет main task с веб-сервером
  
  Serial0.println("HTTP task created successfully");
  
  // Подключение к WiFi с несколькими попытками
  Serial0.println("Connecting to WiFi...");
  reconnectWiFi();
  
  // Настройка NTP
  Serial0.println("Getting time from NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);

  int ntpAttempts = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo) && ntpAttempts < 20) {
    delay(1000);
    Serial0.print(".");
    ntpAttempts++;
  }

  if (ntpAttempts >= 20) {
    Serial0.println("\nFailed to obtain time from NTP");
  } else {
    time_t currentTime = time(nullptr);
    Serial0.printf("\nCurrent time: %s (timestamp: %ld)\n",
                  getReadableTime(currentTime).c_str(), currentTime);
    
    if (!isTimeValid(currentTime)) {
      Serial0.println("WARNING: Received invalid time from NTP!");
    }
  }

  workoutStartTime = 0;
  workoutEndTime = 0;
  workoutEndTime_millis = millis();
  
  // Тестирование Supabase только если есть WiFi
  if (wifiConnected) {
    testSupabaseConnection();
  } else {
    Serial0.println("Skipping Supabase test - no WiFi");
  }

  Serial0.println("Starting web server...");

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", webPageHTML);
  });

  webServer.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    // Статический буфер для JSON
    static char jsonBuffer[200];
    String stateCopy = webCurrentState;
    snprintf(jsonBuffer, sizeof(jsonBuffer),
      "{\"speed\":%.1f,\"distance\":%u,\"time\":%u,\"duration\":%ld,\"state\":\"%s\",\"free_heap\":%u}",
      webCurrentSpeed, webCurrentDistance, webCurrentTime, webSessionDuration,
      stateCopy.c_str(), ESP.getFreeHeap());
      
    request->send(200, "application/json", jsonBuffer);
  });

  // Настройка для минимального влияния на производительность
  webServer.onNotFound([](AsyncWebServerRequest *request){
    request->send(404, "text/plain", "Not found");
  });

  webServer.begin();
  Serial0.println("Web server started!");
  if (wifiConnected) {
    Serial0.printf("Open http://%s in your browser\n", WiFi.localIP().toString().c_str());
  }
  
  // Подключение к беговой дорожке
  Serial0.println("Connecting to treadmill...");
  setLEDState(LED_CONNECTING);
  
  BLEDevice::init("");
  pClient = BLEDevice::createClient();
  
  if (pClient->connect(treadmillAddress)) {
    Serial0.println("Treadmill connected!");
    
    BLERemoteService* pService = pClient->getService("00001826-0000-1000-8000-00805f9b34fb");
    if (pService) {
      pTreadmillData = pService->getCharacteristic("00002acd-0000-1000-8000-00805f9b34fb");
      if (pTreadmillData && pTreadmillData->canNotify()) {
        pTreadmillData->registerForNotify(treadmillDataCallback);
        Serial0.println("Ready to log workouts!");
        connected = true;
        setLEDState(LED_SUCCESS);
        delay(2000);
        setLEDState(LED_STANDBY);
      }
    }
  } else {
    Serial0.println("Failed to connect to treadmill!");
    setLEDState(LED_ERROR);
  }
  
  Serial0.printf("Setup complete. Free heap: %d bytes\n", ESP.getFreeHeap());
}

void loop() {
  // Отключаем веб-сервер при критически низкой памяти
  static bool webServerRunning = true;
  if (ESP.getFreeHeap() < 15000 && webServerRunning) {
    Serial0.println("Low memory - temporarily disabling web server");
    webServer.end();
    webServerRunning = false;
  } else if (ESP.getFreeHeap() > 25000 && !webServerRunning) {
    Serial0.println("Memory recovered - restarting web server");
    webServer.begin();
    webServerRunning = true;
  }

  // Обновляем NeoPixel в каждом цикле
  updateNeoPixel();
  
  if (connected && pClient->isConnected()) {
    static unsigned long lastStatus = 0;
    
    // Проверка соединений каждые 5 секунд
    if (millis() - lastConnectionCheck > CONNECTION_CHECK_INTERVAL) {
      lastConnectionCheck = millis();
      
      // Проверяем WiFi и пытаемся переподключиться при необходимости
      if (WiFi.status() != WL_CONNECTED && wifiConnected) {
        Serial0.println("WiFi lost - attempting reconnection");
        wifiConnected = false;
        setLEDState(LED_WIFI_ERROR);
      } else if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
        Serial0.println("WiFi restored");
        wifiConnected = true;
        if (currentLEDState == LED_WIFI_ERROR) {
          setLEDState(LED_STANDBY);
        }
      }
      
      // Проверяем системное время
      time_t currentTime = time(nullptr);
      if (!isTimeValid(currentTime)) {
        Serial0.printf("WARNING: System time is invalid: %ld\n", currentTime);
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);
      }
      
      // Проверяем память
      if (ESP.getFreeHeap() < 10000) {
        Serial0.printf("WARNING: Low memory! Free heap: %d bytes\n", ESP.getFreeHeap());
      }
      
      if (currentState == STANDBY && (millis() - workoutEndTime_millis < WORKOUT_COOLDOWN)) {
        unsigned long remaining = (WORKOUT_COOLDOWN - (millis() - workoutEndTime_millis)) / 1000;
        Serial0.printf("COOLDOWN: %lu seconds remaining\n", remaining);
      }
      
      // Принудительный сброс при долгой неактивности
      if (currentState == ACTIVE && (millis() - lastActiveTime > STANDBY_TIMEOUT * 2)) {
        Serial0.println("Force reset to STANDBY - no activity detected");
        currentState = STANDBY;
        setLEDState(LED_STANDBY);
        workoutBuffer.clear();
        totalDistance = 0.0;
      }
      
      // Попытка переподключения WiFi каждые 2 минуты если его нет
      static unsigned long lastWiFiAttempt = 0;
      if (!wifiConnected && (millis() - lastWiFiAttempt > 120000)) {
        reconnectWiFi();
        lastWiFiAttempt = millis();
      }
    }
    
    if (currentState == STANDBY && (millis() - lastStatus > 60000)) {
      Serial0.printf("STANDBY (waiting) - %s, WiFi: %s, Free RAM: %d\n", 
                     getReadableTime(time(nullptr)).c_str(),
                     wifiConnected ? "OK" : "NO",
                     ESP.getFreeHeap());
      lastStatus = millis();
    }
    
    delay(100);
  } else {
    Serial0.println("Reconnecting to treadmill...");
    setLEDState(LED_ERROR);
    connected = false;
    delay(5000);
  }
}
