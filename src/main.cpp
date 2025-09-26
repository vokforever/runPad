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
const unsigned long STANDBY_TIMEOUT = 15000;
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
  if (data->buffer.empty() || WiFi.status() != WL_CONNECTED) {
    Serial0.println("No workout data to send or no WiFi");
    return;
  }
  
  Serial0.printf("Sending workout: %s - %s\n", 
                getReadableTime(data->startTime).c_str(),
                getReadableTime(data->endTime).c_str());
  
  // Создаем HTTP клиент
  HTTPClient http;
  
  // ИСПРАВЛЕНО: Полный URL с путем к таблице
  String fullUrl = String(SUPABASE_URL) + "/rest/v1/workouts"; // Замените "workouts" на имя вашей таблицы
  http.begin(fullUrl);
  http.setTimeout(10000); // 10 секунд таймаут
  
  // Все необходимые заголовки
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal"); // Минимальный ответ для экономии памяти
  
  // Создаем JSON
  String jsonPayload = createOptimizedWorkoutJson(data->buffer, data->startTime, data->endTime);
  
  Serial0.println("Sending to: " + fullUrl);
  Serial0.println("JSON size: " + String(jsonPayload.length()) + " bytes");
  Serial0.println("JSON: " + jsonPayload);
  
  // Отправляем запрос
  int httpResponse = http.POST(jsonPayload);
  
  // Получаем ответ
  String response = "";
  if (http.getSize() > 0 && http.getSize() < 1000) { // Ограничиваем размер ответа
    response = http.getString();
  }
  
  Serial0.printf("Supabase response code: %d\n", httpResponse);
  
  if (httpResponse == 200 || httpResponse == 201) {
    Serial0.println("✓ Workout sent successfully!");
  } else {
    Serial0.printf("✗ Failed to send workout. Code: %d\n", httpResponse);
    if (response.length() > 0 && response.length() < 200) {
      Serial0.println("Error details: " + response);
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
  
  // Проверяем свободную память
  if (ESP.getFreeHeap() < 10000) {
    Serial0.println("Low memory - skipping workout send");
    return;
  }
  
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
  
  if (isCurrentlyActive) {
    lastActiveTime = millis();
    if (currentState == STANDBY) {
      currentState = ACTIVE;
      workoutStartTime = time(nullptr);
      
      // Сбрасываем счетчики при начале новой тренировки
      totalDistance = 0.0;
      sessionStartTime = millis();
      lastTimeUpdate = millis();
      
      Serial0.printf(">>> WORKOUT STARTED at %s!\n", 
                     getReadableTime(workoutStartTime).c_str());
    }
  } else {
    unsigned long inactiveTime = millis() - lastActiveTime;
    
    if (currentState == ACTIVE && (inactiveTime > STANDBY_TIMEOUT || 
                                  record.speed <= 0.1)) {
      currentState = WORKOUT_ENDED;
      workoutEndTime = time(nullptr);
      Serial0.printf(">>> WORKOUT ENDED at %s!\n", 
                     getReadableTime(workoutEndTime).c_str());
    }
  }
  
  if (previousState == ACTIVE && currentState == WORKOUT_ENDED) {
    sendWorkoutToSupabase(); // Теперь безопасно через очередь
    currentState = STANDBY;
  }
}

void addToBuffer(const WorkoutRecord& record) {
  static WorkoutRecord lastRecord = {0};
  
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
  
  if (currentDataVec != lastData) {
    Serial0.print("RAW DATA: ");
    for (size_t i = 0; i < length; i++) {
      Serial0.printf("%02X ", pData[i]);
    }
    Serial0.println();
    
    // Детальный анализ пакета
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
  
  // Парсинг скорости - всегда в байтах 2-3
  if (length >= 4) {
    uint16_t speedRaw = pData[2] | (pData[3] << 8);
    newRecord.speed = speedRaw / 100.0;  // Конвертируем в км/ч
    
    Serial0.printf("SPEED: Raw=0x%04X (%d) -> %.1f km/h\n", 
                  speedRaw, speedRaw, newRecord.speed);
  }
  
  // Парсинг дистанции из байтов 4-6 (если есть)
  uint32_t packetDistance = 0;
  if (length >= 7) {
    uint32_t distanceRaw = pData[4] | (pData[5] << 8) | (pData[6] << 16);
    packetDistance = distanceRaw;
    
    Serial0.printf("PACKET DISTANCE: Raw=0x%06X (%d) meters\n", 
                  distanceRaw, distanceRaw);
  }
  
  // Парсинг времени из байтов 16-17
  if (length >= 18) {
    uint16_t timeRaw = pData[16] | (pData[17] << 8);
    newRecord.time = timeRaw;
    
    Serial0.printf("TIME: Raw=0x%04X (%d) seconds\n", 
                  timeRaw, timeRaw);
  }
  
  // Вычисление общей дистанции на основе скорости и времени
  if (newRecord.speed > 0.1 && lastTimeUpdate > 0) {
    unsigned long timeInterval = millis() - lastTimeUpdate;
    if (timeInterval > 100 && timeInterval < 10000) { // От 0.1 до 10 секунд
      float distanceInterval = (newRecord.speed / 3.6) * (timeInterval / 1000.0); // м/с * секунды
      totalDistance += distanceInterval;
      
      Serial0.printf("CALCULATED: Interval=%.1fs, Distance+=%.2fm, Total=%.1fm\n",
                    timeInterval / 1000.0, distanceInterval, totalDistance);
    }
  }
  
  if (newRecord.speed > 0.1) {
    lastTimeUpdate = millis();
  }
  
  // Используем вычисленную общую дистанцию
  newRecord.distance = (uint32_t)totalDistance;
  
  // Проверка валидности данных
  if (newRecord.speed > 25.0) {
    newRecord.speed = 0.0;
  }
  
  newRecord.isActive = (newRecord.speed > 0.1 && newRecord.time > 0);
  
  Serial0.printf("FINAL: Speed=%.1f km/h, Calculated Distance=%d m, Time=%d s, Active=%s\n",
                newRecord.speed, newRecord.distance, newRecord.time, 
                newRecord.isActive ? "YES" : "NO");
  
  updateWorkoutState(newRecord);
  addToBuffer(newRecord);
  
  static WorkoutRecord lastDisplayed = {0};
  if (newRecord.speed != lastDisplayed.speed || 
      newRecord.distance != lastDisplayed.distance ||
      currentState != previousState) {
    
    Serial0.printf("STATE: %s, Speed: %.1f km/h, Total Distance: %d m, Time: %d s\n",
                  currentState == STANDBY ? "STANDBY" : 
                  currentState == ACTIVE ? "ACTIVE" : "ENDED",
                  newRecord.speed, newRecord.distance, newRecord.time);
    
    lastDisplayed = newRecord;
  }
}

void setup() {
  Serial0.begin(115200);
  delay(3000);
  
  Serial0.println("ESP32-S3 Treadmill Logger v2.4 - Stack Safe");
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
    12288,              // Размер стека (12KB) - увеличен для HTTP
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
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial0.println("Failed to obtain time");
  } else {
    Serial0.printf("Current time: %s\n", getReadableTime(time(nullptr)).c_str());
  }
  
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
      
      // Проверяем память
      if (ESP.getFreeHeap() < 8000) {
        Serial0.printf("WARNING: Low memory! Free heap: %d bytes\n", ESP.getFreeHeap());
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
