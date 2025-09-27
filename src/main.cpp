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
#include "config.h"

// NTP сервера
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;     // МСК = UTC+3
const int daylightOffset_sec = 0;

// Настройки буфера (уменьшены для экономии памяти)
const size_t MAX_BUFFER_SIZE = 200;
const unsigned long STANDBY_TIMEOUT = 8000; // 8 секунд для более быстрого определения окончания
const unsigned long CONNECTION_CHECK_INTERVAL = 5000;

BLEAddress treadmillAddress(TREADMILL_MAC);
bool connected = false;

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
const unsigned long WORKOUT_COOLDOWN = 30000; // 30 секунд покоя после тренировки

// Глобальные переменные для расчета дистанции
float totalDistance = 0.0;
unsigned long lastTimeUpdate = 0;
unsigned long sessionStartTime = 0;

// FORWARD DECLARATIONS
void sendWorkoutToSupabaseFromTask(WorkoutData* data);
String createOptimizedWorkoutJson(const std::vector<WorkoutRecord>& buffer, time_t startTime, time_t endTime);
String getISOTimestamp(time_t timeValue);
String getReadableTime(time_t timeValue);
void sendWorkoutToSupabase();
void updateWorkoutState(const WorkoutRecord& record);
void addToBuffer(const WorkoutRecord& record);

// Функция проверки валидности времени
bool isTimeValid(time_t timeValue) {
  // Время должно быть больше 1 января 2020 и меньше 1 января 2030
  const time_t MIN_VALID_TIME = 1577836800; // 1 января 2020
  const time_t MAX_VALID_TIME = 1893456000; // 1 января 2030
  
  return (timeValue >= MIN_VALID_TIME && timeValue <= MAX_VALID_TIME);
}
// Функция получения текущего времени в формате ISO 8601
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

// Оптимизированная функция создания JSON (меньше использует память)
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
  
  // Формируем JSON компактно - экономим память
  String json;
  json.reserve(400); // Резервируем память заранее
  
  json = "{\"workout_start\":\"" + getISOTimestamp(startTime) + 
         "\",\"workout_end\":\"" + getISOTimestamp(endTime) + 
         "\",\"duration_seconds\":" + String(duration) + 
         ",\"total_distance\":" + String(finalRecord.distance) + 
         ",\"max_speed\":" + String(maxSpeed, 1) + 
         ",\"avg_speed\":" + String(avgSpeed, 1) + 
         ",\"records_count\":" + String(buffer.size()) + 
         ",\"device_name\":\"ESP32_Treadmill_Logger\"}";
  
  return json;
}

void sendWorkoutToSupabaseFromTask(WorkoutData* data) {
  if (data->buffer.empty()) {
    Serial0.println("No workout data to send");
    return;
  }

  // ПРОВЕРЯЕМ ВАЛИДНОСТЬ ВРЕМЕННЫХ МЕТОК
  if (!isTimeValid(data->startTime) || !isTimeValid(data->endTime)) {
    Serial0.printf("Invalid timestamps - Start: %ld, End: %ld\n", data->startTime, data->endTime);
    return;
  }

  // ПРОВЕРЯЕМ РАЗУМНОСТЬ ДЛИТЕЛЬНОСТИ
  long duration = data->endTime - data->startTime;
  if (duration <= 0 || duration > 86400) { // От 0 до 24 часов
    Serial0.printf("Invalid workout duration: %ld seconds\n", duration);
    return;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial0.println("No WiFi connection - reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(1000);
      Serial0.print(".");
      attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial0.println("\nFailed to reconnect WiFi");
      return;
    }
    Serial0.println("\nWiFi reconnected");
  }
  
  Serial0.printf("Sending workout: %s - %s (Duration: %ld sec)\n",
                getReadableTime(data->startTime).c_str(),
                getReadableTime(data->endTime).c_str(),
                duration);
  
  // Создаем HTTP клиент с SSL настройками
  HTTPClient http;

  // ИСПРАВЛЕНО: Полный URL с путем к таблице
  String fullUrl = String(SUPABASE_URL) + "/rest/v1/workouts";
  http.begin(fullUrl);

  // Увеличиваем таймауты для стабильности
  http.setTimeout(15000); // 15 секунд таймаут
  http.setConnectTimeout(8000); // 8 секунд на подключение

  // Добавляем SSL настройки для стабильности
  http.setReuse(false); // Отключаем переиспользование соединения
  
  // Все необходимые заголовки
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal"); // Минимальный ответ для экономии памяти
  
  // Создаем JSON
  String jsonPayload = createOptimizedWorkoutJson(data->buffer, data->startTime, data->endTime);

  Serial0.println("Sending to: " + fullUrl);
  Serial0.println("JSON size: " + String(jsonPayload.length()) + " bytes");

  int httpResponse = -1;
  int attempts = 0;
  const int maxAttempts = 3;

  // Повторные попытки отправки
  while (httpResponse <= 0 && attempts < maxAttempts) {
    attempts++;
    Serial0.printf("Attempt %d/%d...\n", attempts, maxAttempts);
    
    // Проверяем память перед каждой попыткой
    Serial0.printf("Free heap before attempt: %d bytes\n", ESP.getFreeHeap());
    
    httpResponse = http.POST(jsonPayload);
    
    if (httpResponse <= 0) {
      Serial0.printf("HTTP error on attempt %d: %d\n", attempts, httpResponse);
      if (attempts < maxAttempts) {
        delay(2000 * attempts); // Увеличиваем задержку с каждой попыткой
      }
    }
  }
  
  // Получаем ответ
  String response = "";
  if (httpResponse > 0) {
    if (http.getSize() > 0 && http.getSize() < 1000) {
      response = http.getString();
    }
    Serial0.printf("Supabase response code: %d\n", httpResponse);
    
    if (httpResponse == 200 || httpResponse == 201) {
      Serial0.println("✓ Workout sent successfully!");
    } else {
      Serial0.printf("✗ HTTP error. Code: %d\n", httpResponse);
      if (response.length() > 0 && response.length() < 200) {
        Serial0.println("Response: " + response);
      }
    }
  } else {
    Serial0.printf("✗ Connection failed. Error code: %d\n", httpResponse);
    
    // Расшифровка основных SSL ошибок
    switch (httpResponse) {
      case -1:
        Serial0.println("Error: Connection failed or timeout");
        break;
      case -11:
        Serial0.println("Error: Connection refused");
        break;
      case -29312:
        Serial0.println("Error: SSL connection ended unexpectedly (EOF)");
        break;
      default:
        Serial0.printf("Error: Unknown connection error (%d)\n", httpResponse);
    }
  }

  http.end();
}

// HTTP задача - работает в отдельном потоке с достаточным стеком
void httpTask(void* parameter) {
  WorkoutData* data = nullptr;
  
  Serial0.println("HTTP Task started");
  
  while (true) {
    if (xQueueReceive(workoutQueue, &data, portMAX_DELAY)) {
      Serial0.println("Processing workout data...");
      
      // Проверяем память перед отправкой
      Serial0.printf("Free heap before HTTP: %d bytes\n", ESP.getFreeHeap());
      
      sendWorkoutToSupabaseFromTask(data);
      
      // Освобождаем память
      delete data;
      data = nullptr;
      
      Serial0.printf("Free heap after HTTP: %d bytes\n", ESP.getFreeHeap());
    }
    
    // Небольшая задержка для других задач
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Безопасная отправка в Supabase через очередь
void sendWorkoutToSupabase() {
  if (workoutBuffer.empty()) {
    Serial0.println("No workout data to send");
    return;
  }
  
  // Проверяем свободную память более строго
  const int MIN_MEMORY_FOR_HTTP = 15000; // Увеличено для SSL
  if (ESP.getFreeHeap() < MIN_MEMORY_FOR_HTTP) {
    Serial0.printf("Low memory for HTTP - skipping. Free: %d, Required: %d\n",
                  ESP.getFreeHeap(), MIN_MEMORY_FOR_HTTP);
    return;
  }

  Serial0.printf("Memory check OK: %d bytes free\n", ESP.getFreeHeap());
  
  Serial0.println(">>> PREPARING TO SEND TO SUPABASE!");
  
  // Создаем копию данных для передачи в HTTP задачу
  WorkoutData* data = new WorkoutData();
  data->buffer = workoutBuffer; // Копируем буфер
  data->startTime = workoutStartTime;
  data->endTime = workoutEndTime;
  
  // Отправляем в очередь HTTP задачи
  if (xQueueSend(workoutQueue, &data, pdMS_TO_TICKS(1000)) == pdTRUE) {
    workoutBuffer.clear(); // Очищаем исходный буфер
    Serial0.println(">>> Workout queued for sending");
  } else {
    delete data; // Если не удалось отправить - освобождаем память
    Serial0.println(">>> Failed to queue workout - queue full");
  }
}

void updateWorkoutState(const WorkoutRecord& record) {
  previousState = currentState;
  
  bool isCurrentlyActive = (record.speed > 0.1 && record.time > 0);

// Игнорируем очень низкие скорости как шум
if (record.speed < 0.5) {
  isCurrentlyActive = false;
}
  
  if (isCurrentlyActive) {
    lastActiveTime = millis();
    if (currentState == STANDBY && (millis() - workoutEndTime_millis > WORKOUT_COOLDOWN)) {
      // Требуем стабильную активность минимум 3 секунды перед началом записи
      static unsigned long firstActiveDetection = 0;
      static float stableSpeedSum = 0.0;
      static int stableSpeedCount = 0;
      
      if (firstActiveDetection == 0) {
        firstActiveDetection = millis();
        stableSpeedSum = record.speed;
        stableSpeedCount = 1;
      } else {
        stableSpeedSum += record.speed;
        stableSpeedCount++;
        
        // Проверяем стабильность активности в течение 3 секунд
        if (millis() - firstActiveDetection >= 3000) {
          float avgStableSpeed = stableSpeedSum / stableSpeedCount;
          
          if (avgStableSpeed >= 1.0) { // Средняя скорость должна быть минимум 1 км/ч
            currentState = ACTIVE;
            
            time_t currentTime = time(nullptr);
            
            if (isTimeValid(currentTime)) {
              workoutStartTime = currentTime;
              Serial0.printf(">>> WORKOUT STARTED at %s! (Avg speed during detection: %.1f km/h)\n",
                             getReadableTime(workoutStartTime).c_str(), avgStableSpeed);
            } else {
              Serial0.printf(">>> WARNING: Invalid time detected (%ld), waiting for NTP sync...\n", currentTime);
              currentState = STANDBY;
              firstActiveDetection = 0;
              return;
            }
            
            // Сбрасываем счетчики при начале новой тренировки
            totalDistance = 0.0;
            sessionStartTime = millis();
            lastTimeUpdate = millis();
          }
          
          // Сбрасываем детектор в любом случае
          firstActiveDetection = 0;
          stableSpeedSum = 0.0;
          stableSpeedCount = 0;
        }
      }
    } else {
      // Сбрасываем детектор активности если движение прекратилось
      static unsigned long firstActiveDetection = 0;
      firstActiveDetection = 0;
    }
  } else {
    unsigned long inactiveTime = millis() - lastActiveTime;
    
    if (currentState == ACTIVE && inactiveTime > STANDBY_TIMEOUT) {
      // Дополнительная проверка: если скорость ниже 0.5 км/ч более 10 секунд - завершаем
      static unsigned long lowSpeedStartTime = 0;
      
      if (record.speed < 0.5) {
        if (lowSpeedStartTime == 0) {
          lowSpeedStartTime = millis();
        } else if (millis() - lowSpeedStartTime > 10000) { // 10 секунд низкой скорости
          Serial0.printf(">>> Ending workout due to prolonged low speed (%.1f km/h)\n", record.speed);
          lowSpeedStartTime = 0; // Сброс для следующего раза
        } else {
          return; // Продолжаем ждать
        }
      } else {
        lowSpeedStartTime = 0; // Сброс если скорость поднялась
        return; // Не завершаем тренировку если скорость нормальная
      }
      time_t currentTime = time(nullptr);
      
      // ПРОВЕРЯЕМ ВАЛИДНОСТЬ ВРЕМЕНИ ПЕРЕД ОКОНЧАНИЕМ
      if (isTimeValid(currentTime) && isTimeValid(workoutStartTime)) {
        workoutEndTime = currentTime;
        
        // ДОПОЛНИТЕЛЬНАЯ ПРОВЕРКА: длительность не должна быть больше 24 часов
        long duration = workoutEndTime - workoutStartTime;
        if (duration > 86400) { // 24 часа в секундах
          Serial0.printf(">>> WARNING: Invalid workout duration (%ld sec), skipping save\n", duration);
          currentState = STANDBY;
          workoutBuffer.clear();
          totalDistance = 0.0;
          return;
        }
        
        currentState = WORKOUT_ENDED;
        workoutEndTime_millis = millis(); // Сохраняем время окончания в миллисекундах
        Serial0.printf(">>> WORKOUT ENDED at %s! Duration: %ld seconds\n",
                       getReadableTime(workoutEndTime).c_str(), duration);
      } else {
        Serial0.printf(">>> WARNING: Invalid time for workout end, discarding workout\n");
        currentState = STANDBY;
        workoutBuffer.clear();
        totalDistance = 0.0;
        return;
      }
    }
  }
  
  if (previousState == ACTIVE && currentState == WORKOUT_ENDED) {
    sendWorkoutToSupabase(); // Теперь безопасно через очередь
    currentState = STANDBY;
  }
}

void addToBuffer(const WorkoutRecord& record) {
  static WorkoutRecord lastRecord = {0};
  
  // ПРОВЕРЯЕМ ВАЛИДНОСТЬ ВРЕМЕНИ В ЗАПИСИ
  if (!isTimeValid(record.timestamp)) {
    Serial0.printf("Skipping record with invalid timestamp: %ld\n", record.timestamp);
    return;
  }
  
  bool shouldAdd = (record.distance != lastRecord.distance ||
                   record.speed != lastRecord.speed ||
                   record.time != lastRecord.time);
  
  if (shouldAdd && currentState == ACTIVE) {
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
  if (currentDataVec != lastData) {
    Serial0.print("RAW DATA: ");
    for (size_t i = 0; i < length; i++) {
      Serial0.printf("%02X ", pData[i]);
    }
    Serial0.println();
    
    Serial0.printf("Analysis: Flags=0x%04X, Bytes[2-3]=0x%04X (%d), Bytes[4-6]=0x%06X, Bytes[16-17]=0x%04X (%d)\n",
                  pData[0] | (pData[1] << 8),
                  pData[2] | (pData[3] << 8), pData[2] | (pData[3] << 8),
                  length >= 7 ? (pData[4] | (pData[5] << 8) | (pData[6] << 16)) : 0,
                  length >= 18 ? (pData[16] | (pData[17] << 8)) : 0,
                  length >= 18 ? (pData[16] | (pData[17] << 8)) : 0);
    
    lastData = currentDataVec;
  }
  
  uint16_t flags = pData[0] | (pData[1] << 8);
  WorkoutRecord newRecord;
  newRecord.timestamp = time(nullptr);
  newRecord.speed = 0.0;
  newRecord.distance = 0;
  newRecord.time = 0;
  
  // Парсинг скорости
  if (length >= 4) {
    uint16_t speedRaw = pData[2] | (pData[3] << 8);
    newRecord.speed = speedRaw / 100.0;
    
    // Фильтр шума - скорости менее 0.3 км/ч считаем нулевыми
    if (newRecord.speed < 0.3) {
      newRecord.speed = 0.0;
    }
  }
  
  // Парсинг дистанции
  uint32_t packetDistance = 0;
  if (length >= 7) {
    uint32_t distanceRaw = pData[4] | (pData[5] << 8) | (pData[6] << 16);
    packetDistance = distanceRaw;
  }
  
  // Парсинг времени
  if (length >= 18) {
    uint16_t timeRaw = pData[16] | (pData[17] << 8);
    newRecord.time = timeRaw;
  }
  
  // Вычисление общей дистанции на основе скорости и времени
  if (newRecord.speed > 0.1 && lastTimeUpdate > 0) {
    unsigned long timeInterval = millis() - lastTimeUpdate;
    if (timeInterval > 100 && timeInterval < 10000) {
      float distanceInterval = (newRecord.speed / 3.6) * (timeInterval / 1000.0);
      totalDistance += distanceInterval;
      
      // Выводим расчеты только когда есть движение
      Serial0.printf("CALCULATED: Interval=%.1fs, Distance+=%.2fm, Total=%.1fm\n",
                    timeInterval / 1000.0, distanceInterval, totalDistance);
    }
  }
  
  if (newRecord.speed > 0.1) {
    lastTimeUpdate = millis();
  }
  
  newRecord.distance = (uint32_t)totalDistance;
  
  if (newRecord.speed > 25.0) {
    newRecord.speed = 0.0;
  }
  
  newRecord.isActive = (newRecord.speed > 0.1 && newRecord.time > 0);
  
  updateWorkoutState(newRecord);
  addToBuffer(newRecord);
  
  // Выводим основную информацию только при изменениях или активности
  static WorkoutRecord lastDisplayed = {0};
  static bool wasActive = false;
  bool isActive = (newRecord.speed > 0.1);
  
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
  
  Serial0.println("ESP32-S3 Treadmill Logger v2.5 - Anti-Multiple Sessions");
  workoutEndTime_millis = millis(); // Инициализируем время окончания
  Serial0.printf("Free heap at start: %d bytes\n", ESP.getFreeHeap());
  
  // СОЗДАЕМ HTTP ЗАДАЧУ И ОЧЕРЕДЬ ПЕРВЫМИ
  Serial0.println("Creating HTTP task and queue...");
  workoutQueue = xQueueCreate(3, sizeof(WorkoutData*)); // Очередь на 3 элемента
  
  if (workoutQueue == nullptr) {
    Serial0.println("Failed to create workout queue!");
    return;
  }
  
  BaseType_t result = xTaskCreatePinnedToCore(
    httpTask,           // Функция задачи
    "HTTP_Task",        // Имя задачи
    16384,              // Размер стека (16KB) - увеличено для SSL
    nullptr,            // Параметр
    2,                  // Приоритет (выше чем у loop)
    &httpTaskHandle,    // Хендл задачи
    0                   // Ядро 0 (BLE обычно на ядре 1)
  );
  
  if (result != pdPASS) {
    Serial0.println("Failed to create HTTP task!");
    return;
  }
  
  Serial0.println("HTTP task created successfully");
  
  Serial0.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial0.print(".");
  }
  Serial0.println("\nWiFi OK!");
  
  // Настройка NTP для получения точного времени
  Serial0.println("Getting time from NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // ЖДЕМ СИНХРОНИЗАЦИИ ВРЕМЕНИ
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
    
    // Проверяем валидность полученного времени
    if (!isTimeValid(currentTime)) {
      Serial0.println("WARNING: Received invalid time from NTP!");
    }
  }

  // ИНИЦИАЛИЗИРУЕМ ВРЕМЕННЫЕ ПЕРЕМЕННЫЕ НУЛЕВЫМИ ЗНАЧЕНИЯМИ
  workoutStartTime = 0;
  workoutEndTime = 0;
  workoutEndTime_millis = millis();
  
  Serial0.println("Connecting to treadmill...");
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
      }
    }
  }
  
  Serial0.printf("Setup complete. Free heap: %d bytes\n", ESP.getFreeHeap());
}

void loop() {
  if (connected && pClient->isConnected()) {
    static unsigned long lastStatus = 0;
    
    // Проверка соединения каждые 5 секунд
    if (millis() - lastConnectionCheck > CONNECTION_CHECK_INTERVAL) {
      lastConnectionCheck = millis();
      
      // ПРОВЕРЯЕМ АКТУАЛЬНОСТЬ СИСТЕМНОГО ВРЕМЕНИ
      time_t currentTime = time(nullptr);
      if (!isTimeValid(currentTime)) {
        Serial0.printf("WARNING: System time is invalid: %ld\n", currentTime);
        
        // Пытаемся пересинхронизировать время
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      }
      
      // Проверяем память
      if (ESP.getFreeHeap() < 8000) {
        Serial0.printf("WARNING: Low memory! Free heap: %d bytes\n", ESP.getFreeHeap());
      }

      // Проверяем качество WiFi соединения
      int rssi = WiFi.RSSI();
      if (rssi < -80) {
        Serial0.printf("Weak WiFi signal: %d dBm\n", rssi);
      }

      if (WiFi.status() != WL_CONNECTED) {
        Serial0.println("WiFi disconnected - attempting reconnection");
        WiFi.reconnect();
      }
      
      if (currentState == STANDBY && (millis() - workoutEndTime_millis < WORKOUT_COOLDOWN)) {
        unsigned long remaining = (WORKOUT_COOLDOWN - (millis() - workoutEndTime_millis)) / 1000;
        Serial0.printf("COOLDOWN: %lu seconds remaining\n", remaining);
      }
      
      // Если долго в активном состоянии без данных - сбрасываем
      if (currentState == ACTIVE && (millis() - lastActiveTime > STANDBY_TIMEOUT * 2)) {
        Serial0.println("Force reset to STANDBY - no activity detected");
        currentState = STANDBY;
        workoutBuffer.clear();
        totalDistance = 0.0;
      }
    }
    
    if (currentState == STANDBY && (millis() - lastStatus > 60000)) {
      Serial0.printf("STANDBY (waiting) - %s, Free RAM: %d\n", 
                     getReadableTime(time(nullptr)).c_str(),
                     ESP.getFreeHeap());
      lastStatus = millis();
    }
    
    delay(1000);
  } else {
    Serial0.println("Reconnecting...");
    connected = false;
    delay(5000);
  }
}
