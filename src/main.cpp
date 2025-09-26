#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "config.h"

// Настройки буфера (в коде)
const size_t MAX_BUFFER_SIZE = 1000;
const float BUFFER_SEND_THRESHOLD = 0.9;

BLEAddress treadmillAddress(TREADMILL_MAC);
bool connected = false;

BLEClient* pClient = nullptr;
BLERemoteCharacteristic* pTreadmillData = nullptr;

// Состояния тренировки
enum WorkoutState {
  STANDBY,        // Дорожка включена но не используется
  ACTIVE,         // Идет тренировка
  WORKOUT_ENDED   // Тренировка только что закончилась
};

WorkoutState currentState = STANDBY;
WorkoutState previousState = STANDBY;

struct WorkoutRecord {
  unsigned long timestamp;
  float speed;
  uint32_t distance;
  uint16_t time;
  uint16_t calories;
  uint16_t heartRate;
  bool isActive;
};

std::vector<WorkoutRecord> workoutBuffer;
unsigned long workoutStartTime = 0;
unsigned long lastActiveTime = 0;
const unsigned long STANDBY_TIMEOUT = 30000; // 30 секунд без активности = standby

void sendWorkoutToSupabase() {
  if (workoutBuffer.empty() || WiFi.status() != WL_CONNECTED) {
    Serial0.println("No workout data to send or no WiFi");
    return;
  }
  
  Serial0.printf("Sending completed workout with %d records...\n", workoutBuffer.size());
  
  HTTPClient http;
  http.begin(SUPABASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  
  // Создаем одну запись с суммарными данными тренировки (ИСПРАВЛЕНО)
  JsonDocument jsonDoc;
  JsonObject workout = jsonDoc.to<JsonObject>();
  
  // Берем данные из последней записи (финальные показатели)
  WorkoutRecord finalRecord = workoutBuffer.back();
  WorkoutRecord firstRecord = workoutBuffer.front();
  
  workout["workout_start"] = workoutStartTime;
  workout["workout_end"] = millis();
  workout["duration_seconds"] = finalRecord.time;
  workout["total_distance"] = finalRecord.distance;
  workout["max_speed"] = 0.0;
  workout["avg_speed"] = 0.0;
  workout["calories"] = finalRecord.calories;
  
  // Вычисляем максимальную и среднюю скорость
  float maxSpeed = 0.0;
  float totalSpeed = 0.0;
  int activeRecords = 0;
  
  for (const auto& record : workoutBuffer) {
    if (record.speed > maxSpeed) maxSpeed = record.speed;
    if (record.speed > 0.1) {
      totalSpeed += record.speed;
      activeRecords++;
    }
  }
  
  workout["max_speed"] = maxSpeed;
  workout["avg_speed"] = activeRecords > 0 ? totalSpeed / activeRecords : 0.0;
  workout["records_count"] = workoutBuffer.size();
  
  String jsonString;
  serializeJson(jsonDoc, jsonString);
  
  Serial0.println("Sending workout summary:");
  Serial0.printf("Duration: %d sec, Distance: %d m, Max Speed: %.1f km/h\n", 
                finalRecord.time, finalRecord.distance, maxSpeed);
  
  int httpResponse = http.POST(jsonString);
  Serial0.printf("Supabase response: %d\n", httpResponse);
  
  if (httpResponse == 201) {
    workoutBuffer.clear();
    Serial0.println("Workout sent successfully!");
  } else {
    Serial0.printf("Failed to send workout: %d\n", httpResponse);
  }
  
  http.end();
}

void updateWorkoutState(const WorkoutRecord& record) {
  previousState = currentState;
  
  // Определяем активность
  bool isCurrentlyActive = (record.distance > 0 || record.speed > 0.1);
  
  if (isCurrentlyActive) {
    lastActiveTime = millis();
    if (currentState == STANDBY) {
      currentState = ACTIVE;
      workoutStartTime = millis();
      Serial0.println(">>> WORKOUT STARTED!");
    }
  } else {
    // Проверяем, прошло ли достаточно времени без активности
    if (currentState == ACTIVE && (millis() - lastActiveTime > STANDBY_TIMEOUT)) {
      currentState = WORKOUT_ENDED;
      Serial0.println(">>> WORKOUT ENDED (timeout)!");
    }
  }
  
  // Если состояние изменилось с ACTIVE на WORKOUT_ENDED
  if (previousState == ACTIVE && currentState == WORKOUT_ENDED) {
    Serial0.println(">>> SENDING COMPLETED WORKOUT TO SUPABASE!");
    sendWorkoutToSupabase();
    currentState = STANDBY; // Возвращаемся в режим ожидания
  }
}

void addToBuffer(const WorkoutRecord& record) {
  // Добавляем в буфер только активные записи или изменения
  static WorkoutRecord lastRecord = {0};
  
  bool shouldAdd = (record.distance != lastRecord.distance || 
                   record.speed != lastRecord.speed ||
                   record.time != lastRecord.time ||
                   currentState == ACTIVE);
  
  if (shouldAdd) {
    if (workoutBuffer.size() >= MAX_BUFFER_SIZE) {
      Serial0.println("Buffer overflow - removing oldest record");
      workoutBuffer.erase(workoutBuffer.begin());
    }
    
    workoutBuffer.push_back(record);
    lastRecord = record;
    
    Serial0.printf("Buffer: %d records, State: %s\n", 
                  workoutBuffer.size(), 
                  currentState == STANDBY ? "STANDBY" : 
                  currentState == ACTIVE ? "ACTIVE" : "ENDED");
  }
}

void treadmillDataCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (length < 8) return;
  
  // Показываем сырые данные только при изменениях
  static std::vector<uint8_t> lastData;
  std::vector<uint8_t> currentDataVec(pData, pData + length);
  
  if (currentDataVec != lastData) {
    Serial0.print("TREADMILL DATA: ");
    for (size_t i = 0; i < length; i++) {
      Serial0.printf("%02X ", pData[i]);
    }
    Serial0.println();
    lastData = currentDataVec;
  }
  
  uint16_t flags = pData[0] | (pData[1] << 8);
  
  WorkoutRecord newRecord;
  newRecord.timestamp = millis();
  
  int offset = 2;
  
  // Декодирование данных
  if (flags & 0x01) {
    uint16_t speedRaw = pData[offset] | (pData[offset+1] << 8);
    newRecord.speed = speedRaw / 100.0;
    offset += 2;
  }
  
  if (flags & 0x02) {
    offset += 2; // Пропускаем наклон
  }
  
  if (flags & 0x04) {
    newRecord.distance = pData[offset] | (pData[offset+1] << 8) | (pData[offset+2] << 16);
    offset += 3;
  }
  
  if ((flags & 0x0C00) && length >= 18) {
    newRecord.time = pData[16] | (pData[17] << 8);
  }
  
  newRecord.isActive = (newRecord.distance > 0 || newRecord.speed > 0.1);
  
  // Обновляем состояние и логику
  updateWorkoutState(newRecord);
  
  // Добавляем в буфер только если тренировка активна
  if (currentState == ACTIVE) {
    addToBuffer(newRecord);
  }
  
  // Показываем статус только при изменениях
  static WorkoutRecord lastDisplayed = {0};
  if (newRecord.distance != lastDisplayed.distance || 
      newRecord.speed != lastDisplayed.speed ||
      currentState != previousState) {
    
    Serial0.printf("State: %s, Speed: %.1f km/h, Distance: %d m, Time: %d sec\n",
                  currentState == STANDBY ? "STANDBY" : 
                  currentState == ACTIVE ? "ACTIVE" : "ENDED",
                  newRecord.speed, newRecord.distance, newRecord.time);
    
    lastDisplayed = newRecord;
  }
}

void setup() {
  Serial0.begin(115200);
  delay(3000);
  
  Serial0.println("=================================");
  Serial0.println("ESP32-S3 Smart Treadmill Logger");
  Serial0.println("=================================");
  
  // Подключение к WiFi
  Serial0.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial0.print(".");
  }
  Serial0.println("\nWiFi connected!");
  
  // Подключение к беговой дорожке
  Serial0.println("Connecting to treadmill...");
  BLEDevice::init("");
  pClient = BLEDevice::createClient();
  
  if (pClient->connect(treadmillAddress)) {
    Serial0.println("Connected to treadmill!");
    
    BLERemoteService* pService = pClient->getService("00001826-0000-1000-8000-00805f9b34fb");
    if (pService) {
      pTreadmillData = pService->getCharacteristic("00002acd-0000-1000-8000-00805f9b34fb");
      if (pTreadmillData && pTreadmillData->canNotify()) {
        pTreadmillData->registerForNotify(treadmillDataCallback);
        Serial0.println("Smart workout tracking enabled!");
        Serial0.println("- STANDBY: Waiting for workout");
        Serial0.println("- ACTIVE: Recording workout data");
        Serial0.println("- Will send to Supabase when workout ends");
        connected = true;
      }
    }
  }
}

void loop() {
  if (connected && pClient->isConnected()) {
    // Показываем статус каждые 60 секунд в режиме STANDBY
    static unsigned long lastStatus = 0;
    if (currentState == STANDBY && (millis() - lastStatus > 60000)) {
      Serial0.printf("Status: STANDBY (waiting for workout)\n");
      lastStatus = millis();
    }
    delay(1000);
  } else {
    Serial0.println("Connection lost, reconnecting...");
    delay(5000);
  }
}
