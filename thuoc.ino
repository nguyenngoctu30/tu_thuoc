/*
 * Hiển thị 4 hộp thuốc + Đồng hồ RTC + MQTT TLS (port 8883)
 * Thư viện: Adafruit GFX + Adafruit ILI9341 + RTClib + WiFi + PubSubClient
 *
 * Pin Cảm biến : 4, 5, 6, 7
 * Pin TFT      : MOSI=18  SCK=19  CS=3  DC=2  RST=20
 * Pin RTC I2C  : SDA=8    SCL=10
 *
 * MQTT topic   : esp/h1 .. esp/h4   (1 = còn, 0 = hết)
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>
#include <Preferences.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "677 5G";
const char* WIFI_PASS = "10101010";

const char* NTP_SERVER1 = "pool.ntp.org";
const char* NTP_SERVER2 = "time.google.com";
const long  GMT_OFFSET  = 7 * 3600;
const int   DST_OFFSET  = 0;

// ── MQTT TLS ──────────────────────────────────────────────
const char* MQTT_SERVER = "broker.hivemq.com";
const int   MQTT_PORT   = 8883;
const char* MQTT_CLIENT = "esp32_hopThuoc";
const char* MQTT_USER   = "";
const char* MQTT_PASS_B = "";

const char* mqttTopics[4] = { "esp/h1", "esp/h2", "esp/h3", "esp/h4" };

// ── Pin TFT ──────────────────────────────────────────────
#define TFT_CS   3
#define TFT_DC   2
#define TFT_RST  20
#define TFT_MOSI 18
#define TFT_SCK  19

// ── Pin cảm biến ─────────────────────────────────────────
#define NUM_SENSORS 4
const int sensorPins[NUM_SENSORS] = {4, 5, 6, 7};

// ── Pin buzzer ───────────────────────────────────────────
#define BUZZER_PIN 21

// ── Web server URL ───────────────────────────────────────
// const char* REMINDER_URL = "http://your-web-server.com/reminders.json"; // Removed, using MQTT

// ─────────────────────────────────────────────────────────
//  BẢNG MÀU – Giao diện xám nhạt, tối giản, gắn kết
// ─────────────────────────────────────────────────────────

// Nền chính: xám nhạt ấm  #CECECE → RGB565
#define CLR_BG          0xCE59   // xám nhạt chủ đạo

// Header bar: trắng nhẹ hơn nền 1 chút
#define CLR_HEADER_BG   0xF79E   // trắng ngà

// Đường phân cách mảnh
#define CLR_DIVIDER     0xB5B6   // xám trung

// Chữ tiêu đề đậm tối
#define CLR_TITLE       0x2124   // gần đen

// Vùng đồng hồ: nền cùng màu nền chính
#define CLR_CLOCK_BG    0xCE59   // = CLR_BG
#define CLR_CLOCK_FG    0x10A2   // xanh tối đậm (giờ)
#define CLR_DATE_FG     0x738E   // xám xanh (ngày)
#define CLR_UTC_FG      0x9CD3   // xám nhạt (nhãn UTC)

// Card CÒN THUỐC: nền trắng sữa, viền xanh lá
#define CLR_CARD_HAS_BG 0xF7FE   // trắng ngà ấm
#define CLR_CARD_HAS_BD 0x2DC5   // xanh lá chuẩn
#define CLR_HAS_TEXT    0x1603   // xanh lá đậm
#define CLR_HAS_PILL_A  0x67E4   // xanh lá sáng
#define CLR_HAS_PILL_B  0x2DC5   // xanh lá đậm

// Card HẾT THUỐC: nền hồng nhạt, viền đỏ
#define CLR_CARD_EMP_BG 0xFEF3   // hồng rất nhạt
#define CLR_CARD_EMP_BD 0xF204   // đỏ chuẩn
#define CLR_EMP_TEXT    0xA000   // đỏ đậm
#define CLR_EMP_PILL_A  0xFB8C   // cam đỏ nhạt
#define CLR_EMP_PILL_B  0xF204   // đỏ

// Dot MQTT
#define CLR_MQTT_OK     0x2DC5   // xanh lá
#define CLR_MQTT_ERR    0xF204   // đỏ
#define CLR_MQTT_RIM    0xB5B6   // xám viền

// Reminder box
#define CLR_REMINDER_BG 0xE6F1   // xanh nhạt

// Label số (HOP 1 .. HOP 4)
#define CLR_LABEL       0x4208   // xám đậm

const char* boxLabels[NUM_SENSORS] = { "HOP 1", "HOP 2", "HOP 3", "HOP 4" };
const char* dayNames[] = { "CN","T2","T3","T4","T5","T6","T7" };

// ─────────────────────────────────────────────────────────

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
RTC_DS3231 rtc;
WiFiClientSecure wifiClient;
PubSubClient     mqtt(wifiClient);

bool prevState[NUM_SENSORS];
char prevTimeStr[9]  = "";
char prevDateStr[20] = "";
bool mqttWasConnected = false;

// ── Reminders ────────────────────────────────────────────
#define MAX_REMINDERS 5
#define MAX_SCHEDULES 7

struct Schedule {
  String date;
  String times[MAX_REMINDERS];
  int numTimes;
};

Schedule schedules[MAX_SCHEDULES];
int numSchedules = 0;
bool reminderTriggered = false;
Preferences preferences;

// ── Next reminder display ─────────────────────────────────
char nextReminderStr[30] = "Chua co lich uong thuoc";
unsigned long lastNextUpdate = 0;

// ─────────────────────────────────────────────────────────
//  Check và trigger reminder
// ─────────────────────────────────────────────────────────
void checkReminder() {
  DateTime now = rtc.now();
  char currentTime[6];
  sprintf(currentTime, "%02d:%02d", now.hour(), now.minute());

  char today[11];
  sprintf(today, "%04d-%02d-%02d", now.year(), now.month(), now.day());

  // Find schedule for today
  int todayIdx = -1;
  for (int i = 0; i < numSchedules; i++) {
    if (schedules[i].date == String(today)) {
      todayIdx = i;
      break;
    }
  }

  if (todayIdx == -1) return; // No schedule for today

  for (int i = 0; i < schedules[todayIdx].numTimes; i++) {
    if (schedules[todayIdx].times[i] == String(currentTime) && !reminderTriggered) {
      // Trigger reminder
      Serial.printf("[REMINDER] Time to take medicine! %s\n", currentTime);

      // Buzzer beep
      digitalWrite(BUZZER_PIN, HIGH);
      delay(1000);
      digitalWrite(BUZZER_PIN, LOW);

      // Display message on screen
      tft.fillRect(0, 40, 240, 40, CLR_BG);
      tft.setTextColor(CLR_TITLE);
      tft.setTextSize(2);
      tft.setCursor(20, 50);
      tft.print("UONG THUOC!");
      delay(5000); // Show for 5 seconds
      drawClock(true); // Redraw clock

      reminderTriggered = true;
      break;
    }
  }

  // Reset trigger at next minute
  static int lastMinute = -1;
  if (now.minute() != lastMinute) {
    reminderTriggered = false;
    lastMinute = now.minute();
  }
}
void loadReminders() {
  preferences.begin("reminders", false);
  numSchedules = preferences.getInt("numSch", 0);
  for (int s = 0; s < numSchedules; s++) {
    String schKey = "sch" + String(s);
    schedules[s].date = preferences.getString((schKey + "_date").c_str(), "");
    schedules[s].numTimes = preferences.getInt((schKey + "_num").c_str(), 0);
    for (int i = 0; i < schedules[s].numTimes; i++) {
      String timeKey = schKey + "_t" + String(i);
      schedules[s].times[i] = preferences.getString(timeKey.c_str(), "");
    }
  }
  preferences.end();
  Serial.printf("[REMINDER] Loaded %d schedules\n", numSchedules);
}

// ─────────────────────────────────────────────────────────
//  Save reminders vào Preferences
// ─────────────────────────────────────────────────────────
void saveReminders() {
  preferences.begin("reminders", false);
  preferences.putInt("numSch", numSchedules);
  for (int s = 0; s < numSchedules; s++) {
    String schKey = "sch" + String(s);
    preferences.putString((schKey + "_date").c_str(), schedules[s].date);
    preferences.putInt((schKey + "_num").c_str(), schedules[s].numTimes);
    for (int i = 0; i < schedules[s].numTimes; i++) {
      String timeKey = schKey + "_t" + String(i);
      preferences.putString(timeKey.c_str(), schedules[s].times[i]);
    }
  }
  preferences.end();
  Serial.printf("[REMINDER] Saved %d schedules\n", numSchedules);
}

// ─────────────────────────────────────────────────────────
//  MQTT Callback
// ─────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.printf("[MQTT] Received %s: %s\n", topic, msg.c_str());

  if (strcmp(topic, "esp/schedule") == 0) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, msg);
    if (error) {
      Serial.println("[MQTT] JSON parse failed");
      return;
    }

    int times = doc["times"];
    JsonArray slots = doc["slots"];

    const char* dateValue = doc["date"];
    String newDate = "";
    if (dateValue != nullptr && strlen(dateValue) == 10 && dateValue[4] == '-' && dateValue[7] == '-') {
      newDate = String(dateValue);
    } else {
      Serial.println("[MQTT] Invalid date");
      return;
    }

    // Find if date already exists
    int existingIdx = -1;
    for (int i = 0; i < numSchedules; i++) {
      if (schedules[i].date == newDate) {
        existingIdx = i;
        break;
      }
    }

    if (existingIdx != -1) {
      // Update existing
      schedules[existingIdx].numTimes = 0;
      for (String slot : slots) {
        if (schedules[existingIdx].numTimes < MAX_REMINDERS) {
          schedules[existingIdx].times[schedules[existingIdx].numTimes++] = slot;
        }
      }
    } else {
      // Add new
      if (numSchedules < MAX_SCHEDULES) {
        schedules[numSchedules].date = newDate;
        schedules[numSchedules].numTimes = 0;
        for (String slot : slots) {
          if (schedules[numSchedules].numTimes < MAX_REMINDERS) {
            schedules[numSchedules].times[schedules[numSchedules].numTimes++] = slot;
          }
        }
        numSchedules++;
      } else {
        Serial.println("[MQTT] Max schedules reached");
        return;
      }
    }

    saveReminders();
    updateNextReminder();
    drawNextReminder(true);

    // Publish confirmation
    String ackMsg = "{\"status\":\"received\",\"date\":\"" + newDate + "\",\"times\":" + String(times) + "}";
    mqtt.publish("esp/schedule/ack", ackMsg.c_str(), true);
    Serial.printf("[MQTT] Sent ACK: %s\n", ackMsg.c_str());
  }
}

// ─────────────────────────────────────────────────────────
//  Update next reminder string
// ─────────────────────────────────────────────────────────
void updateNextReminder() {
  DateTime now = rtc.now();
  char today[11];
  sprintf(today, "%04d-%02d-%02d", now.year(), now.month(), now.day());

  // Find schedule for today
  int todayIdx = -1;
  for (int i = 0; i < numSchedules; i++) {
    if (schedules[i].date == String(today)) {
      todayIdx = i;
      break;
    }
  }

  if (todayIdx == -1) {
    strcpy(nextReminderStr, "Chua co lich uong thuoc");
    return;
  }

  int currentMinutes = now.hour() * 60 + now.minute();

  int nextIdx = -1;
  int nextMinutes = 24 * 60;

  for (int i = 0; i < schedules[todayIdx].numTimes; i++) {
    int h, m;
    sscanf(schedules[todayIdx].times[i].c_str(), "%d:%d", &h, &m);
    int remMinutes = h * 60 + m;
    if (remMinutes > currentMinutes && remMinutes < nextMinutes) {
      nextMinutes = remMinutes;
      nextIdx = i;
    }
  }

  if (nextIdx != -1) {
    sprintf(nextReminderStr, "Lich uong tiep theo: %s", schedules[todayIdx].times[nextIdx].c_str());
  } else {
    strcpy(nextReminderStr, "Lich da qua");
  }
}
void drawMqttDot(bool connected) {
  uint16_t fill = connected ? CLR_MQTT_OK : CLR_MQTT_ERR;
  // Vẽ nền xung quanh trước để xóa dot cũ gọn
  tft.fillRect(218, 10, 18, 18, CLR_HEADER_BG);
  tft.fillCircle(226, 19, 5, fill);
  tft.drawCircle(226, 19, 6, CLR_MQTT_RIM);
}

// ─────────────────────────────────────────────────────────
//  Boot message – vùng đồng hồ
// ─────────────────────────────────────────────────────────
void showBootMsg(const char* line1, const char* line2 = "") {
  tft.fillRect(0, 40, 240, 40, CLR_CLOCK_BG);
  tft.setTextColor(CLR_CLOCK_FG);
  tft.setTextSize(1);
  tft.setCursor(8, 46);
  tft.print(line1);
  if (strlen(line2) > 0) {
    tft.setCursor(8, 58);
    tft.setTextColor(CLR_DATE_FG);
    tft.print(line2);
  }
}

// ─────────────────────────────────────────────────────────
//  Kết nối WiFi + NTP → ghi RTC
// ─────────────────────────────────────────────────────────
bool syncTimeFromNTP() {
  Serial.print("[WiFi] Ket noi: ");
  Serial.println(WIFI_SSID);
  showBootMsg("Ket noi WiFi...", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] THAT BAI");
    showBootMsg("WiFi that bai!", "Dung gio RTC cu...");
    delay(1500);
    return false;
  }

  Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
  showBootMsg("WiFi OK! Lay gio NTP...", WiFi.localIP().toString().c_str());

  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER1, NTP_SERVER2);

  struct tm timeinfo;
  uint8_t ntpRetry = 0;
  Serial.print("[NTP] Dong bo");
  while (!getLocalTime(&timeinfo) && ntpRetry < 20) {
    delay(500); Serial.print("."); ntpRetry++;
  }
  Serial.println();

  if (ntpRetry >= 20) {
    Serial.println("[NTP] THAT BAI");
    showBootMsg("NTP that bai!", "Dung gio RTC cu...");
    delay(1500);
    return false;
  }

  rtc.adjust(DateTime(
    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec
  ));

  char buf[40];
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d",
    timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  Serial.print("[NTP] OK: "); Serial.println(buf);
  showBootMsg("NTP OK!", buf);
  delay(1000);
  return true;
}

// ─────────────────────────────────────────────────────────
//  Kết nối / reconnect MQTT
// ─────────────────────────────────────────────────────────
void mqttReconnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  Serial.print("[MQTT] Ket noi broker (TLS 8883)...");
  bool ok;
  if (strlen(MQTT_USER) > 0)
    ok = mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS_B);
  else
    ok = mqtt.connect(MQTT_CLIENT);

  if (ok) {
    Serial.println(" OK");
    mqtt.subscribe("esp/schedule");
    for (int i = 0; i < NUM_SENSORS; i++)
      mqtt.publish(mqttTopics[i], prevState[i] ? "1" : "0", true);
    drawMqttDot(true);
    mqttWasConnected = true;
  } else {
    Serial.printf(" THAT BAI (rc=%d)\n", mqtt.state());
    drawMqttDot(false);
  }
}

// ─────────────────────────────────────────────────────────
//  Gửi MQTT
// ─────────────────────────────────────────────────────────
void mqttPublish(uint8_t idx, bool hasMed) {
  const char* payload = hasMed ? "1" : "0";
  if (mqtt.connected()) {
    bool ok = mqtt.publish(mqttTopics[idx], payload, true);
    Serial.printf("[MQTT] %s -> %s  (%s)\n", mqttTopics[idx], payload, ok ? "OK" : "FAIL");
  } else {
    Serial.printf("[MQTT] %s -> %s  (OFFLINE)\n", mqttTopics[idx], payload);
  }
}

// ─────────────────────────────────────────────────────────
//  Icon viên thuốc – đơn giản, 2 màu rõ ràng
// ─────────────────────────────────────────────────────────
void drawPillIcon(int16_t cx, int16_t cy, bool hasMed) {
  uint16_t colL = hasMed ? CLR_HAS_PILL_A : CLR_EMP_PILL_A;
  uint16_t colR = hasMed ? CLR_HAS_PILL_B : CLR_EMP_PILL_B;
  const int16_t r = 9, hw = 18;

  // Nền xóa vùng icon
  uint16_t cardBg = hasMed ? CLR_CARD_HAS_BG : CLR_CARD_EMP_BG;
  tft.fillRect(cx - hw - r - 1, cy - r - 1, (hw + r) * 2 + 2, r * 2 + 2, cardBg);

  // Viên thuốc: nửa trái
  tft.fillCircle(cx - hw, cy, r, colL);
  tft.fillRect(cx - hw, cy - r, hw, r * 2, colL);
  // Nửa phải
  tft.fillCircle(cx + hw, cy, r, colR);
  tft.fillRect(cx, cy - r, hw, r * 2, colR);
  // Đường giữa trắng mảnh
  tft.drawFastVLine(cx, cy - r + 1, r * 2 - 2, 0xFFFF);
  // Viền ngoài
  tft.drawRoundRect(cx - hw - r, cy - r, (hw + r) * 2, r * 2, r, CLR_DIVIDER);
}

// ─────────────────────────────────────────────────────────
//  Card hộp thuốc – bo tròn mềm, nền sáng
// ─────────────────────────────────────────────────────────
void drawCard(uint8_t idx, bool hasMed) {
  // Layout 2×2, màn 240×320
  // Header: 38px, Clock: 42px, Next Reminder: 20px → cards bắt đầu từ y=100
  const int16_t cardW = 110, cardH = 113;
  const int16_t gapX  = 8,  gapY  = 6;
  const int16_t startX = 6, startY = 100;

  uint8_t  col = idx % 2;
  uint8_t  row = idx / 2;
  int16_t  x = startX + col * (cardW + gapX);
  int16_t  y = startY + row * (cardH + gapY);

  uint16_t bgCard  = hasMed ? CLR_CARD_HAS_BG : CLR_CARD_EMP_BG;
  uint16_t bdCard  = hasMed ? CLR_CARD_HAS_BD : CLR_CARD_EMP_BD;
  uint16_t txtClr  = hasMed ? CLR_HAS_TEXT     : CLR_EMP_TEXT;
  const char* statusTxt = hasMed ? "CON" : "HET";

  // ── Bóng mờ nhẹ ──
  tft.fillRoundRect(x + 2, y + 2, cardW, cardH, 10, CLR_DIVIDER);

  // ── Thân card ──
  tft.fillRoundRect(x, y, cardW, cardH, 10, bgCard);

  // ── Viền accent (2px) ──
  tft.drawRoundRect(x,     y,     cardW,     cardH,     10, bdCard);
  tft.drawRoundRect(x + 1, y + 1, cardW - 2, cardH - 2, 9,  bdCard);

  // ── Thanh màu trên đầu card (accent strip) ──
  tft.fillRect(x + 10, y, cardW - 20, 4, bdCard);

  // ── Label "HOP N" ──
  tft.setTextColor(CLR_LABEL);
  tft.setTextSize(1);
  tft.setCursor(x + 8, y + 10);
  tft.print(boxLabels[idx]);

  // ── Icon viên thuốc ở giữa ──
  drawPillIcon(x + cardW / 2, y + 55, hasMed);

  // ── Trạng thái chữ to ──
  tft.setTextSize(2);
  tft.setTextColor(txtClr);
  // Căn giữa: "CON" / "HET" đều 3 ký tự × 12px = 36px
  tft.setCursor(x + (cardW - 36) / 2, y + 84);
  tft.print(statusTxt);

  // ── Chấm trạng thái góc trên phải card ──
  tft.fillCircle(x + cardW - 12, y + 12, 5, bdCard);
}

// ─────────────────────────────────────────────────────────
//  Đồng hồ – chữ tối, nền nền chính
// ─────────────────────────────────────────────────────────
void drawClock(bool forceRedraw) {
  DateTime now = rtc.now();

  char timeStr[9], dateStr[20];
  sprintf(timeStr, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  sprintf(dateStr, "%s, %02d/%02d/%04d",
          dayNames[now.dayOfTheWeek()], now.day(), now.month(), now.year());

  bool timeChanged = (strcmp(timeStr, prevTimeStr) != 0);
  bool dateChanged = (strcmp(dateStr, prevDateStr) != 0);

  if (!forceRedraw && !timeChanged && !dateChanged) return;

  if (timeChanged)
    Serial.printf("[CLOCK] %s  %s\n", dateStr, timeStr);

  if (forceRedraw) {
    // Vùng clock: y=38 .. y=80 (42px)
    tft.fillRect(0, 38, 240, 43, CLR_CLOCK_BG);
    // Đường phân cách mảnh dưới clock
    tft.drawFastHLine(0, 80, 240, CLR_DIVIDER);
  }

  if (forceRedraw || timeChanged) {
    // Xóa vùng giờ
    tft.fillRect(6, 40, 160, 22, CLR_CLOCK_BG);
    tft.setTextColor(CLR_CLOCK_FG);
    tft.setTextSize(2);
    tft.setCursor(8, 42);
    tft.print(timeStr);
    // Nhãn UTC+7 nhỏ bên phải giờ
    tft.setTextSize(1);
    tft.setTextColor(CLR_UTC_FG);
    tft.setCursor(168, 42);
    tft.print("UTC+7");
    strcpy(prevTimeStr, timeStr);
  }

  if (forceRedraw || dateChanged) {
    tft.fillRect(6, 64, 220, 14, CLR_CLOCK_BG);
    tft.setTextColor(CLR_DATE_FG);
    tft.setTextSize(1);
    tft.setCursor(8, 65);
    tft.print(dateStr);
    strcpy(prevDateStr, dateStr);
  }
}

// ─────────────────────────────────────────────────────────
//  Draw next reminder
// ─────────────────────────────────────────────────────────
void drawNextReminder(bool force) {
  static char prevNextStr[30] = "";
  if (!force && strcmp(nextReminderStr, prevNextStr) == 0) return;

  // Background box
  tft.fillRoundRect(5, 82, 230, 18, 5, CLR_REMINDER_BG);
  tft.drawRoundRect(5, 82, 230, 18, 5, CLR_DIVIDER);

  tft.setTextColor(CLR_TITLE);
  tft.setTextSize(1);
  tft.setCursor(12, 86);
  tft.print(nextReminderStr);

  // Separator line
  tft.drawFastHLine(0, 100, 240, CLR_DIVIDER);

  strcpy(prevNextStr, nextReminderStr);
}

// ─────────────────────────────────────────────────────────
//  In trạng thái ra Serial
// ─────────────────────────────────────────────────────────
void printAllStatus() {
  DateTime now = rtc.now();
  Serial.println("========================================");
  Serial.printf("[CLOCK] %s, %02d/%02d/%04d  %02d:%02d:%02d (UTC+7)\n",
    dayNames[now.dayOfTheWeek()],
    now.day(), now.month(), now.year(),
    now.hour(), now.minute(), now.second());
  Serial.println("----------------------------------------");
  for (int i = 0; i < NUM_SENSORS; i++) {
    Serial.printf("  [S%d] %-6s: %s  | MQTT %s -> %s\n",
      i + 1, boxLabels[i],
      prevState[i] ? "CON THUOC" : "HET THUOC",
      mqttTopics[i],
      prevState[i] ? "1" : "0");
  }
  Serial.printf("[MQTT] %s (TLS port %d)\n",
    mqtt.connected() ? "CONNECTED" : "OFFLINE", MQTT_PORT);
  Serial.println("========================================");
}

// ─────────────────────────────────────────────────────────
//  Vẽ toàn bộ UI
// ─────────────────────────────────────────────────────────
void drawUI() {
  tft.fillScreen(CLR_BG);

  // ── Header ──────────────────────────────────────────────
  // Nền trắng ngà, chiều cao 38px
  tft.fillRect(0, 0, 240, 38, CLR_HEADER_BG);
  tft.drawFastHLine(0, 37, 240, CLR_DIVIDER);

  // Đường accent mỏng trên cùng header
  tft.drawFastHLine(0, 0, 240, CLR_CARD_HAS_BD);
  tft.drawFastHLine(0, 1, 240, CLR_CARD_HAS_BD);

  tft.setTextColor(CLR_TITLE);
  tft.setTextSize(2);
  // "QUAN LY THUOC"  → gọn hơn, căn bằng tay
  tft.setCursor(8, 10);
  tft.print("QUAN LY THUOC");

  drawMqttDot(mqtt.connected());

  // ── Clock ───────────────────────────────────────────────
  drawClock(true);

  // ── Next Reminder ───────────────────────────────────────
  drawNextReminder(true);

  // ── Cards ───────────────────────────────────────────────
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    drawCard(i, prevState[i]);
  }
}

// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== KHOI DONG HE THONG ===");

  Wire.begin(8, 10);

  for (int i = 0; i < NUM_SENSORS; i++) {
    pinMode(sensorPins[i], INPUT);
    prevState[i] = false;
  }

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  if (!rtc.begin()) {
    Serial.println("[LOI] Khong tim thay RTC DS3231!");
    while (1) delay(100);
  }
  Serial.println("[RTC] DS3231 OK");

  tft.begin();
  tft.setRotation(0);
  Serial.println("[TFT] ILI9341 OK");

  // Vẽ header sớm trong boot
  tft.fillScreen(CLR_BG);
  tft.fillRect(0, 0, 240, 38, CLR_HEADER_BG);
  tft.drawFastHLine(0, 37, 240, CLR_DIVIDER);
  tft.drawFastHLine(0, 0, 240, CLR_CARD_HAS_BD);
  tft.drawFastHLine(0, 1, 240, CLR_CARD_HAS_BD);
  tft.setTextColor(CLR_TITLE);
  tft.setTextSize(2);
  tft.setCursor(8, 10);
  tft.print("QUAN LY THUOC");

  syncTimeFromNTP();

  loadReminders();

  wifiClient.setInsecure();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);
  mqtt.setCallback(mqttCallback);

  Serial.println("[SENSOR] Trang thai ban dau:");
  for (int i = 0; i < NUM_SENSORS; i++) {
    prevState[i] = digitalRead(sensorPins[i]);
    Serial.printf("  [S%d] %s: %s\n",
      i + 1, boxLabels[i],
      prevState[i] ? "CON THUOC" : "HET THUOC");
  }

  showBootMsg("Ket noi MQTT TLS...", MQTT_SERVER);
  mqttReconnect();

  drawUI();

  for (int i = 0; i < NUM_SENSORS; i++) {
    mqttPublish(i, prevState[i]);
  }

  Serial.println("[OK] He thong san sang!\n");
  printAllStatus();
}

// ─────────────────────────────────────────────────────────
void loop() {
  bool nowConnected = mqtt.connected();
  if (!nowConnected) {
    if (mqttWasConnected) drawMqttDot(false);
    mqttReconnect();
  }
  if (mqtt.connected() != mqttWasConnected) {
    drawMqttDot(mqtt.connected());
    mqttWasConnected = mqtt.connected();
  }
  mqtt.loop();

  drawClock(false);

  // Update next reminder every minute
  if (millis() - lastNextUpdate > 60000UL) {
    updateNextReminder();
    lastNextUpdate = millis();
  }
  drawNextReminder(false);

  checkReminder();

  // Fetch reminders every hour - removed, using MQTT
  // static unsigned long lastFetch = 0;
  // if (millis() - lastFetch > 3600000UL) {
  //   fetchReminders();
  //   lastFetch = millis();
  // }

  for (int i = 0; i < NUM_SENSORS; i++) {
    bool state = digitalRead(sensorPins[i]);
    if (state != prevState[i]) {
      prevState[i] = state;
      drawCard(i, state);
      mqttPublish(i, state);
      Serial.printf("[THAY DOI] HOP %d (%s): %s\n",
        i + 1, boxLabels[i],
        state ? "CON THUOC" : "HET THUOC");
    }
  }

  delay(200);
}