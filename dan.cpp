#include <WiFi.h>
#include <FirebaseESP32.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <time.h>
#include <LittleFS.h>

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // Múi giờ Việt Nam (GMT+7)
const int   daylightOffset_sec = 0;

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00:00";
  char timeStringBuff[20];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// ================= CẤU HÌNH HỆ THỐNG =================

SemaphoreHandle_t dataMutex;

//XỬ LÝ KHÓA RƠ-LE
volatile bool isOverloadTripped = false;
unsigned long ignoreWebUntil = 0;
// 1. WiFi & Firebase
#define WIFI_SSID "Mainz"
#define WIFI_PASSWORD "07042004new"
#define FIREBASE_HOST "enerymonitor-37ab9-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "SH4zasBdOwb1fpcXCe4bq3rBTURaVSE2rQbfcsOu"

// 2. Chân kết nối
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17
#define PZEM_SERIAL Serial2

#define TFT_CS    5
#define TFT_RST   22
#define TFT_DC    21
// Chân SPI: SCK=18, MOSI=23

#define RELAY_PIN 4
#define BUZZER_PIN 15  // Còi báo động

// 3. Màu màn hình (RGB565)
#define COLOR_BG      0x0000
#define COLOR_HEADER  0x02B5
#define COLOR_LABEL   0x07FF
#define COLOR_VALUE   0xFFE0
#define COLOR_BAR_BG  0x4208
#define COLOR_ON      0x07E0
#define COLOR_OFF     0xF800

// ================= KHỞI TẠO ĐỐI TƯỢNG =================
PZEM004Tv30 pzem(PZEM_SERIAL, PZEM_RX_PIN, PZEM_TX_PIN);
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
FirebaseData firebaseData;
FirebaseAuth auth;
FirebaseConfig config;

// ================= CẤU TRÚC DỮ LIỆU =================
struct PowerData {
  float voltage = 0.0;
  float current = 0.0;
  float power = 0.0;
  float energy = 0.0;
  bool relayState = true;
};

// Cấu trúc lưu ngưỡng bảo vệ tải từ Firebase
struct SystemSettings {
  float maxPower = 1500.0;
  float maxCurrent = 10.0;
  float maxVoltage = 250.0;
  float minVoltage = 180.0;
};

//Cấu trúc lưu Thống kê
struct EnergyStats {
  float daily = 0; float weekly = 0; float monthly = 0; float yearly = 0;
  float last_e = -1;
  int saved_yday = -1; int saved_wday = -1; int saved_mon = -1; int saved_year = -1;
};
EnergyStats currentStats;
bool statsLoaded = false;

PowerData sharedData;
SystemSettings currentSettings;

// ================= KHAI BÁO HÀM =================
void drawStaticUI();
void TaskReadPZEM(void *pvParameters);
void TaskDisplay(void *pvParameters);
void TaskRelayControl(void *pvParameters);
void TaskFirebase(void *pvParameters);

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  // 1. Khởi tạo Relay (Active-Low) & Còi
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Bật mặc định
  sharedData.relayState = true;

  pinMode(BUZZER_PIN, OUTPUT);     
  digitalWrite(BUZZER_PIN, LOW);

  // 2. Khởi tạo Mutex (Dùng trong FreeRTOS)
  dataMutex = xSemaphoreCreateMutex();

  // 3. Màn hình TFT
  tft.initR(INITR_BLACKTAB); 
  tft.setRotation(1); 
  drawStaticUI(); 

  // 4. Kết nối WiFi
  tft.fillRect(0, 118, 160, 10, COLOR_BG);
  tft.setCursor(5, 118); tft.setTextColor(ST77XX_YELLOW);
  tft.print("Dang ket noi WiFi...");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 20) {
    delay(500); Serial.print("."); retryCount++;
  }

  tft.fillRect(0, 118, 160, 10, COLOR_BG);
  tft.setCursor(5, 118);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("Da ket noi WiFi"); 

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // Khởi tạo Firebase
    config.host = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.print("Offline Mode");
  }

  if (!LittleFS.begin(true)) {
    Serial.println("Lỗi khởi tạo hệ thống File LittleFS!");
  }

  // 5. Khởi chạy Đa nhiệm
  if (dataMutex != NULL) {
    xTaskCreatePinnedToCore(TaskReadPZEM, "PZEM", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(TaskDisplay, "TFT", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(TaskRelayControl, "RelayCtrl", 2048, NULL, 1, NULL, 1);

    xTaskCreatePinnedToCore(TaskFirebase, "Firebase", 8192, NULL, 1, NULL, 0);
  }
}

void loop() { vTaskDelete(NULL); }

// ================= CÁC TASK =================

void drawStaticUI() {
  tft.fillScreen(COLOR_BG);
  tft.fillRect(0, 0, 160, 18, COLOR_HEADER);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(40, 5); tft.print("POWER MONITOR");

  tft.setTextColor(COLOR_LABEL);
  tft.setCursor(4, 26);  tft.print("Dien ap:");
  tft.setCursor(4, 44);  tft.print("Dong:");
  tft.setCursor(4, 62);  tft.print("Cong suat:");
  tft.drawRect(4, 76, 150, 8, ST77XX_WHITE);
  tft.setCursor(4, 92);  tft.print("Dien nang:");
  tft.setCursor(4, 108); tft.print("Relay:");
}

void TaskReadPZEM(void *pvParameters) {
  for (;;) {
    float v = pzem.voltage(); float i = pzem.current();
    float p = pzem.power();   float e = pzem.energy();

    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      sharedData.voltage = isnan(v) ? 0.0 : v;
      sharedData.current = isnan(i) ? 0.0 : i;
      sharedData.power   = isnan(p) ? 0.0 : p;
      sharedData.energy  = isnan(e) ? 0.0 : e;
      xSemaphoreGive(dataMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
  }
}

void TaskDisplay(void *pvParameters) {
  PowerData localData;
  float maxPower = 2000.0;
  for (;;) {
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
      localData = sharedData;
      xSemaphoreGive(dataMutex);
    }

    tft.setTextColor(COLOR_VALUE, COLOR_BG); 
    
    // Đưa toàn bộ cột tọa độ X từ 109 lùi về 95 
    // Tại vị trí 95, kể cả chuỗi "kWh" dài nhất cộng lại (95 + 54 = 149) vẫn nằm gọn trong 160 pixel
    tft.setCursor(95, 26); tft.printf("%5.1f V", localData.voltage);
    tft.setCursor(95, 44); tft.printf("%5.2f A", localData.current);
    tft.setCursor(95, 62); tft.printf("%5.1f W", localData.power);
    tft.setCursor(95, 92); tft.printf("%5.3f kWh", localData.energy);

    // Lùi thanh dung lượng từ 6 về 5
    tft.fillRect(5, 77, 148, 6, COLOR_BAR_BG); 
    int barWidth = (localData.power / maxPower) * 148;
    if(barWidth > 148) barWidth = 148; if(barWidth < 0) barWidth = 0;
    uint16_t barColor = (localData.power > 1500) ?
      COLOR_OFF : ((localData.power > 1000) ? ST77XX_ORANGE : COLOR_ON);
    tft.fillRect(5, 77, barWidth, 6, barColor); 

    // Lùi nút trạng thái Relay từ 85 về 84
    if (localData.relayState == false) {
      tft.fillRoundRect(84, 105, 40, 12, 3, COLOR_OFF);
      tft.setTextColor(ST77XX_WHITE); tft.setCursor(91, 107); tft.print("OFF"); 
    } else {
      tft.fillRoundRect(84, 105, 40, 12, 3, COLOR_ON);
      tft.setTextColor(ST77XX_BLACK); tft.setCursor(94, 107); tft.print("ON");
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// TASK XỬ LÝ BẢO VỆ (Kiểm tra đa ngưỡng)
void TaskRelayControl(void *pvParameters) {
  for (;;) {
    PowerData localData;
    SystemSettings localSettings;
    
    // Đọc dữ liệu từ biến dùng chung an toàn qua Mutex
    if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) { 
      localData = sharedData; 
      localSettings = currentSettings;
      xSemaphoreGive(dataMutex);
    }
    
    // CHUẨN LOGIC: Chỉ cảnh báo khi Relay ĐANG BẬT và CÓ ĐIỆN LƯỚI (> 50V)
    if (localData.relayState == true && localData.voltage > 50.0) {
      bool isWarning = false;
      
      // KIỂM TRA 4 NGƯỠNG CÀI ĐẶT
      if (localData.power > localSettings.maxPower) isWarning = true;
      if (localData.current > localSettings.maxCurrent) isWarning = true;
      if (localData.voltage > localSettings.maxVoltage) isWarning = true;
      if (localData.voltage < localSettings.minVoltage) isWarning = true;

      if (isWarning) { 
        // 1. Kéo chân tín hiệu lên HIGH để TẮT rơ le, cách ly tải
        digitalWrite(RELAY_PIN, HIGH); 
        
        // 2. Cập nhật trạng thái mới vào biến dùng chung
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) { 
          sharedData.relayState = false; // false = Đã tắt (OFF)
          xSemaphoreGive(dataMutex);
        }
        
        // 3. Bật cờ sự kiện để Task Firebase gửi trạng thái lên Web
        isOverloadTripped = true; 
        
        // 4. Hú còi 5 tiếng cảnh báo bằng hàm phát xung tone()
        for(int i = 0; i < 5; i++) {
          tone(BUZZER_PIN, 2500); // Kêu ở tần số 2500Hz
          vTaskDelay(pdMS_TO_TICKS(300));
          noTone(BUZZER_PIN);     // Ngắt âm thanh
          vTaskDelay(pdMS_TO_TICKS(300));
        }
        // Tạm nghỉ 2 giây sau chuỗi cảnh báo
        vTaskDelay(pdMS_TO_TICKS(2000)); 
      } else {
        // Đảm bảo còi tắt nếu các thông số bình thường
        noTone(BUZZER_PIN); 
      }
    } else {
      // Đảm bảo còi tắt nếu rơ-le đang ngắt hoặc không có điện lưới
      noTone(BUZZER_PIN); 
    }
    
    // Chu kỳ quét bảo vệ (0.5 giây/lần)
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// TASK FIREBASE (Tích hợp Offline Logging)
void TaskFirebase(void *pvParameters) {
  unsigned long lastLogTime = 0;
  unsigned long lastSensorTime = 0; 
  const unsigned long logInterval = 6000; 

  for (;;) {
    bool isConnected = (WiFi.status() == WL_CONNECTED);

    // ==========================================
    // 0. ĐỒNG BỘ DỮ LIỆU OFFLINE LÊN FIREBASE
    // ==========================================
    if (isConnected && Firebase.ready()) {
      File file = LittleFS.open("/offline_logs.txt", FILE_READ);
      if (file && file.size() > 0) {
        Serial.println("Phat hien du lieu Offline. Dang day len Firebase...");
        while (file.available()) {
          String line = file.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            FirebaseJson offlineJson;
            offlineJson.setJsonData(line);
            Firebase.pushJSON(firebaseData, "/logs", offlineJson);
            vTaskDelay(pdMS_TO_TICKS(100)); // Nghỉ 0.1s giữa mỗi dòng để tránh nghẽn
          }
        }
        file.close();
        LittleFS.remove("/offline_logs.txt"); // Đẩy xong xóa file đi
        Serial.println("Da dong bo Offline xong!");
      }
    }

    // ==========================================
    // CÁC CHỨC NĂNG ONLINE (Điều khiển & Cấu hình)
    // ==========================================
    if (isConnected && Firebase.ready()) {
      PowerData localData;
      if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        localData = sharedData; xSemaphoreGive(dataMutex);
      }

      // ƯU TIÊN 1: BÁO QUÁ TẢI LÊN WEB & ĐỌC LỆNH
      if (isOverloadTripped) {
        if (Firebase.setInt(firebaseData, "/control", 1)) {
          isOverloadTripped = false;
          ignoreWebUntil = millis() + 6000; 
        }
      } 
      else if (millis() > ignoreWebUntil) {
        if (Firebase.getInt(firebaseData, "/control")) {
          bool isTurnedOn = (firebaseData.intData() == 0); 
          if (isTurnedOn != localData.relayState) {
            digitalWrite(RELAY_PIN, isTurnedOn ? LOW : HIGH);
            if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
              sharedData.relayState = isTurnedOn; 
              xSemaphoreGive(dataMutex);
            }
          }
        }
      }

      // ƯU TIÊN 2: CẬP NHẬT THÔNG SỐ VÀ TÍNH TOÁN TIỀN ĐIỆN (2 giây/lần)
      if (millis() - lastSensorTime >= 2000) {
        lastSensorTime = millis();
        Firebase.setFloat(firebaseData, "/voltage", localData.voltage);
        Firebase.setFloat(firebaseData, "/current", localData.current);
        Firebase.setFloat(firebaseData, "/power", localData.power);
        Firebase.setFloat(firebaseData, "/energy", localData.energy);

        if (Firebase.getJSON(firebaseData, "/settings")) {
            FirebaseJson &json = firebaseData.jsonObject();
            FirebaseJsonData result;
            if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
                json.get(result, "max_power");   if(result.success) currentSettings.maxPower = result.to<float>();
                json.get(result, "max_current"); if(result.success) currentSettings.maxCurrent = result.to<float>();
                json.get(result, "max_voltage"); if(result.success) currentSettings.maxVoltage = result.to<float>();
                json.get(result, "min_voltage"); if(result.success) currentSettings.minVoltage = result.to<float>();
                xSemaphoreGive(dataMutex);
            }
        }

        // --- BỔ SUNG LOGIC THỐNG KÊ ĐIỆN NĂNG ---
        // 1. Lần đầu mở máy, tải lại số cũ từ Firebase xuống để không bị mất dữ liệu
        if (!statsLoaded) {
            if (Firebase.getJSON(firebaseData, "/stats")) {
                FirebaseJsonData result;
                FirebaseJson &json = firebaseData.jsonObject();
                json.get(result, "daily");   if(result.success) currentStats.daily = result.to<float>();
                json.get(result, "weekly");  if(result.success) currentStats.weekly = result.to<float>();
                json.get(result, "monthly"); if(result.success) currentStats.monthly = result.to<float>();
                json.get(result, "yearly");  if(result.success) currentStats.yearly = result.to<float>();
                json.get(result, "last_e");  if(result.success) currentStats.last_e = result.to<float>();
                json.get(result, "yday");    if(result.success) currentStats.saved_yday = result.to<int>();
                json.get(result, "wday");    if(result.success) currentStats.saved_wday = result.to<int>();
                json.get(result, "mon");     if(result.success) currentStats.saved_mon = result.to<int>();
                json.get(result, "year");    if(result.success) currentStats.saved_year = result.to<int>();
            }
            statsLoaded = true;
        }

        struct tm timeinfo;
        if (statsLoaded && getLocalTime(&timeinfo)) {
            // Lần đầu chưa có mốc thời gian -> Lưu mốc hiện tại
            if (currentStats.saved_year == -1) {
                currentStats.saved_yday = timeinfo.tm_yday;
                currentStats.saved_wday = timeinfo.tm_wday;
                currentStats.saved_mon = timeinfo.tm_mon;
                currentStats.saved_year = timeinfo.tm_year;
            }

            // RESET NẾU QUA NGÀY/TUẦN/THÁNG/NĂM MỚI
            if (timeinfo.tm_yday != currentStats.saved_yday) {
                currentStats.daily = 0;
                // tm_wday = 1 là Thứ Hai, nếu qua ngày mà là Thứ 2 thì reset tuần
                if (timeinfo.tm_wday == 1) currentStats.weekly = 0; 
            }
            if (timeinfo.tm_mon != currentStats.saved_mon) currentStats.monthly = 0;
            if (timeinfo.tm_year != currentStats.saved_year) currentStats.yearly = 0;

            // CỘNG DỒN ĐIỆN NĂNG (Dùng phương pháp Delta để chống lỗi khi PZEM bị Reset về 0)
            if (currentStats.last_e == -1 || localData.energy < currentStats.last_e) {
                currentStats.last_e = localData.energy;
            }
            float delta_e = localData.energy - currentStats.last_e;
            
            if (delta_e > 0) {
                currentStats.daily += delta_e;
                currentStats.weekly += delta_e;
                currentStats.monthly += delta_e;
                currentStats.yearly += delta_e;
                currentStats.last_e = localData.energy;
            }

            // Cập nhật lại mốc thời gian
            currentStats.saved_yday = timeinfo.tm_yday;
            currentStats.saved_wday = timeinfo.tm_wday;
            currentStats.saved_mon = timeinfo.tm_mon;
            currentStats.saved_year = timeinfo.tm_year;

            // ĐẨY DỮ LIỆU LÊN FIREBASE
            FirebaseJson statsJson;
            statsJson.add("daily", currentStats.daily);
            statsJson.add("weekly", currentStats.weekly);
            statsJson.add("monthly", currentStats.monthly);
            statsJson.add("yearly", currentStats.yearly);
            statsJson.add("last_e", currentStats.last_e);
            statsJson.add("yday", currentStats.saved_yday);
            statsJson.add("wday", currentStats.saved_wday);
            statsJson.add("mon", currentStats.saved_mon);
            statsJson.add("year", currentStats.saved_year);
            Firebase.updateNode(firebaseData, "/stats", statsJson);
        }
      }
    }

    // ==========================================
    // 3. LƯU LOG (Chạy bất chấp Online hay Offline)
    // ==========================================
    if (millis() - lastLogTime >= logInterval) {
      lastLogTime = millis();
      String currentTime = getTimeString();
      
      // Chống lỗi thời gian 1970 nếu vừa cúp điện vừa mất WiFi
      if (currentTime != "00:00:00") { 
        PowerData logData;
        if (xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
           logData = sharedData; xSemaphoreGive(dataMutex);
        }

        FirebaseJson json;
        json.add("0_time", currentTime); 
        json.add("1_v", logData.voltage);
        json.add("2_i", logData.current);
        json.add("3_p", logData.power);
        json.add("4_e", logData.energy);
        json.add("5_r", logData.relayState ? 0 : 1); 

        if (isConnected && Firebase.ready()) {
          // CÓ MẠNG -> Đẩy lên Web ngay
          Firebase.pushJSON(firebaseData, "/logs", json);
        } else {
          // MẤT MẠNG -> Lưu tạm vào File Text trong mạch
          String jsonStr;
          json.toString(jsonStr, false);
          File file = LittleFS.open("/offline_logs.txt", FILE_APPEND);
          if (file) {
            file.println(jsonStr);
            file.close();
            Serial.println("Da luu 1 log vao che do Offline");
          }
        }
      }
    } 

    // CHỜ THÔNG MINH
    for (int i = 0; i < 20; i++) {
      if (isOverloadTripped && isConnected) break; 
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  } 
}
