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
  #include <JQ6500_Serial.h>
  #include <HardwareSerial.h>
  #include <Adafruit_NeoPixel.h>

  // ── LED WS2812 ────────────────────────────────────────────
  #define LED_PIN     7
  #define LED_COUNT   4
  Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

  // ── MP3 JQ6500 ────────────────────────────────────────────
  HardwareSerial mySerial(1);
  JQ6500_Serial  mp3(mySerial);
  #define RX_PIN 4
  #define TX_PIN 5

  // ── WiFi ──────────────────────────────────────────────────
  const char* WIFI_SSID = "677 5G";
  const char* WIFI_PASS = "10101010";

  // ── NTP ───────────────────────────────────────────────────
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

  // ── Pin TFT ───────────────────────────────────────────────
  #define TFT_CS   3
  #define TFT_DC   2
  #define TFT_RST  20
  #define TFT_MOSI 18
  #define TFT_SCK  19

  // ── Pin cảm biến ──────────────────────────────────────────
  #define NUM_SENSORS 4
  const int sensorPins[NUM_SENSORS] = {11, 12, 13, 15};

  // ── Bảng màu ──────────────────────────────────────────────
  #define CLR_BG          0xCE59
  #define CLR_HEADER_BG   0xF79E
  #define CLR_DIVIDER     0xB5B6
  #define CLR_TITLE       0x2124
  #define CLR_CLOCK_BG    0xCE59
  #define CLR_CLOCK_FG    0x10A2
  #define CLR_DATE_FG     0x738E
  #define CLR_UTC_FG      0x9CD3
  #define CLR_CARD_HAS_BG 0xF7FE
  #define CLR_CARD_HAS_BD 0x2DC5
  #define CLR_HAS_TEXT    0x1603
  #define CLR_HAS_PILL_A  0x67E4
  #define CLR_HAS_PILL_B  0x2DC5
  #define CLR_CARD_EMP_BG 0xFEF3
  #define CLR_CARD_EMP_BD 0xF204
  #define CLR_EMP_TEXT    0xA000
  #define CLR_EMP_PILL_A  0xFB8C
  #define CLR_EMP_PILL_B  0xF204
  #define CLR_MQTT_OK     0x2DC5
  #define CLR_MQTT_ERR    0xF204
  #define CLR_MQTT_RIM    0xB5B6
  #define CLR_REMINDER_BG 0xE6F1
  #define CLR_LABEL       0x4208

  const char* boxLabels[NUM_SENSORS] = { "HOP 1", "HOP 2", "HOP 3", "HOP 4" };
  const char* dayNames[] = { "CN","T2","T3","T4","T5","T6","T7" };

  // ── Objects ───────────────────────────────────────────────
  Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCK, TFT_RST);
  RTC_DS3231       rtc;
  WiFiClientSecure wifiClient;
  PubSubClient     mqtt(wifiClient);

  bool prevState[NUM_SENSORS];
  char prevTimeStr[9]  = "";
  char prevDateStr[20] = "";
  bool mqttWasConnected = false;

  // ── Reminders ─────────────────────────────────────────────
  #define MAX_REMINDERS 5
  #define MAX_SCHEDULES 7

  struct Schedule {
    String date;
    String times[MAX_REMINDERS];
    int    numTimes;
  };

  Schedule      schedules[MAX_SCHEDULES];
  int           numSchedules      = 0;
  bool          reminderTriggered = false;
  char          nextReminderStr[50] = "Chua co lich uong thuoc";
  unsigned long lastNextUpdate      = 0;

  // ── Trạng thái nhắc nhở thuốc ──────────────────────────────
  enum ReminderState {
    STATE_IDLE,
    STATE_ACTIVE,       // Trong 30s đầu: hiển thị "UONG THUOC!", đèn màu xanh lá sáng liên tục
    STATE_MISSED_ALARM  // Sau 30s: hiển thị "Chưa lấy thuốc!", đèn màu xanh lá nhấp nháy
  };

  ReminderState currentReminderState = STATE_IDLE;
  int           activeReminderBox = -1;
  unsigned long reminderStartTime = 0;
  unsigned long lastBlinkTime = 0;
  bool          blinkLedState = false;

  Preferences preferences;

  // ─────────────────────────────────────────────────────────
  //  LED WS2812
  // ─────────────────────────────────────────────────────────
  void updateLED() {
    if (currentReminderState == STATE_ACTIVE) {
      for (int i = 0; i < NUM_SENSORS; i++) {
        if (i == activeReminderBox) {
          strip.setPixelColor(i, strip.Color(0, 255, 0)); // Xanh lá sáng liên tục
        } else if (!prevState[i]) {
          strip.setPixelColor(i, strip.Color(255, 0, 0)); // Đỏ
        } else {
          strip.setPixelColor(i, strip.Color(0, 0, 255)); // Xanh dương
        }
      }
    } else if (currentReminderState == STATE_MISSED_ALARM) {
      for (int i = 0; i < NUM_SENSORS; i++) {
        if (i == activeReminderBox) {
          if (blinkLedState) {
            strip.setPixelColor(i, strip.Color(0, 255, 0)); // Xanh lá sáng
          } else {
            strip.setPixelColor(i, strip.Color(0, 0, 0));   // Tắt
          }
        } else if (!prevState[i]) {
          strip.setPixelColor(i, strip.Color(255, 0, 0)); // Đỏ
        } else {
          strip.setPixelColor(i, strip.Color(0, 0, 255)); // Xanh dương
        }
      }
    } else {
      // Bình thường
      for (int i = 0; i < NUM_SENSORS; i++) {
        if (!prevState[i]) {
          strip.setPixelColor(i, strip.Color(255, 0, 0)); // Đỏ
        } else {
          strip.setPixelColor(i, strip.Color(0, 0, 255)); // Xanh dương
        }
      }
    }
    strip.show();
  }

  // ─────────────────────────────────────────────────────────
  //  MP3
  // ─────────────────────────────────────────────────────────
  void playEmptyBoxVoice(int boxIndex) {
    int fileNumber = boxIndex + 1;
    Serial.printf("[MP3] Phat canh bao hop %d het thuoc -> file %d\n", boxIndex + 1, fileNumber);
    mp3.playFileByIndexNumber(fileNumber);
    delay(500);
  }

  void playReminderVoice() {
    int fileNumber = -1;
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (prevState[i]) {
        fileNumber = 5 + i;
        Serial.printf("[MP3] Nhac uong thuoc -> hop %d -> file %d\n", i + 1, fileNumber);
        break;
      }
    }
    if (fileNumber == -1) {
      Serial.println("[MP3] Tat ca hop het thuoc, khong phat");
      return;
    }
    mp3.playFileByIndexNumber(fileNumber);
    delay(500);
  }

  // ─────────────────────────────────────────────────────────
  //  Preferences – xóa toàn bộ dữ liệu rác
  // ─────────────────────────────────────────────────────────
  void clearAllReminders() {
    preferences.begin("reminders", false);
    preferences.clear();
    preferences.end();
    numSchedules = 0;
    for (int s = 0; s < MAX_SCHEDULES; s++) {
      schedules[s].date     = "";
      schedules[s].numTimes = 0;
      for (int i = 0; i < MAX_REMINDERS; i++) schedules[s].times[i] = "";
    }
    Serial.println("[REMINDER] Cleared all schedules");
  }

  // ─────────────────────────────────────────────────────────
  //  Preferences – load (lọc bỏ slot rác)
  // ─────────────────────────────────────────────────────────
  void loadReminders() {
    preferences.begin("reminders", false);
    int saved = preferences.getInt("numSch", 0);
    numSchedules = 0;

    for (int s = 0; s < saved; s++) {
      String schKey = "sch" + String(s);
      String d = preferences.getString((schKey + "_date").c_str(), "");
      int    n = preferences.getInt((schKey + "_num").c_str(), 0);
      d.trim();

      // Bỏ qua slot rác: date sai định dạng hoặc không có giờ nào
      if (d.length() != 10 || d[4] != '-' || d[7] != '-' || n == 0) {
        Serial.printf("[REMINDER] Skip slot %d (date='%s' times=%d)\n", s, d.c_str(), n);
        continue;
      }

      schedules[numSchedules].date     = d;
      schedules[numSchedules].numTimes = 0;

      for (int i = 0; i < n; i++) {
        String timeKey = schKey + "_t" + String(i);
        String t = preferences.getString(timeKey.c_str(), "");
        t.trim();
        if (t.length() >= 4) {
          schedules[numSchedules].times[schedules[numSchedules].numTimes++] = t;
          Serial.printf("  [LOAD] slot %d time[%d] = %s\n", numSchedules, i, t.c_str());
        }
      }

      if (schedules[numSchedules].numTimes > 0) {
        Serial.printf("[REMINDER] Loaded: date=%s times=%d\n",
                      d.c_str(), schedules[numSchedules].numTimes);
        numSchedules++;
      } else {
        Serial.printf("[REMINDER] Skip slot %d: khong co gio hop le\n", s);
      }
    }

    preferences.end();
    Serial.printf("[REMINDER] Loaded %d valid schedules\n", numSchedules);
  }

  // ─────────────────────────────────────────────────────────
  //  Preferences – save
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
  //  Cập nhật chuỗi nhắc tiếp theo
  // ─────────────────────────────────────────────────────────
  void updateNextReminder() {
    DateTime now = rtc.now();
    char today[11];
    sprintf(today, "%04d-%02d-%02d", now.year(), now.month(), now.day());

    Serial.printf("[REMINDER] updateNextReminder: today=%s numSchedules=%d\n",
                  today, numSchedules);

    int todayIdx = -1;
    for (int i = 0; i < numSchedules; i++) {
      Serial.printf("  compare '%s' == '%s' ? %s\n",
                    schedules[i].date.c_str(), today,
                    schedules[i].date.equals(String(today)) ? "YES" : "NO");
      if (schedules[i].date.equals(String(today))) { todayIdx = i; break; }
    }

    if (todayIdx == -1) {
      strcpy(nextReminderStr, "Chua co lich hom nay");
      Serial.println("[REMINDER] Khong co lich hom nay");
      return;
    }

    int currentMinutes = now.hour() * 60 + now.minute();
    int nextIdx        = -1;
    int nextMinutes    = 24 * 60;

    for (int i = 0; i < schedules[todayIdx].numTimes; i++) {
      int h = 0, m = 0;
      sscanf(schedules[todayIdx].times[i].c_str(), "%d:%d", &h, &m);
      int remMin = h * 60 + m;
      Serial.printf("  slot[%d]=%s remMin=%d currentMin=%d\n",
                    i, schedules[todayIdx].times[i].c_str(), remMin, currentMinutes);
      if (remMin > currentMinutes && remMin < nextMinutes) {
        nextMinutes = remMin;
        nextIdx     = i;
      }
    }

    if (nextIdx != -1) {
      snprintf(nextReminderStr, sizeof(nextReminderStr),
              "Tiep theo: %s",
              schedules[todayIdx].times[nextIdx].c_str());
    } else {
      strcpy(nextReminderStr, "Da uong du hom nay");
    }

    Serial.printf("[REMINDER] Next: %s\n", nextReminderStr);
  }

  // ─────────────────────────────────────────────────────────
  //  Reminder – kiểm tra & kích hoạt
  // ─────────────────────────────────────────────────────────
  void checkReminder() {
    if (currentReminderState != STATE_IDLE) return;

    DateTime now = rtc.now();
    static int lastMinute = -1;
    if (now.minute() != lastMinute) {
      reminderTriggered = false;
      lastMinute = now.minute();
    }
    if (reminderTriggered) return;

    char currentTime[6];
    sprintf(currentTime, "%02d:%02d", now.hour(), now.minute());

    char today[11];
    sprintf(today, "%04d-%02d-%02d", now.year(), now.month(), now.day());

    int todayIdx = -1;
    for (int i = 0; i < numSchedules; i++) {
      if (schedules[i].date.equals(String(today))) { todayIdx = i; break; }
    }
    if (todayIdx == -1) return;

    for (int i = 0; i < schedules[todayIdx].numTimes; i++) {
      if (schedules[todayIdx].times[i].equals(String(currentTime))) {
        // Tìm hộp đầu tiên có thuốc làm hộp nhắc nhở
        int targetBox = -1;
        for (int b = 0; b < NUM_SENSORS; b++) {
          if (prevState[b]) {
            targetBox = b;
            break;
          }
        }
        if (targetBox == -1) {
          Serial.println("[REMINDER] Den gio uong thuoc nhung tat ca hop deu het!");
          reminderTriggered = true;
          return;
        }

        Serial.printf("[REMINDER] Uong thuoc luc %s. Hop can lay: %d\n", currentTime, targetBox + 1);

        currentReminderState = STATE_ACTIVE;
        activeReminderBox = targetBox;
        reminderStartTime = millis();
        reminderTriggered = true;

        // Vẽ thông báo lên TFT
        tft.fillRect(0, 38, 240, 43, CLR_BG);
        tft.setTextColor(CLR_TITLE);
        tft.setTextSize(2);
        tft.setCursor(54, 50); // Căn giữa "UONG THUOC!"
        tft.print("UONG THUOC!");

        playReminderVoice();
        updateLED();
        break;
      }
    }
  }

  void runReminderState() {
    if (currentReminderState == STATE_IDLE) return;

    // Nếu hộp cần nhắc nhở đã hết thuốc (tức là người dùng đã lấy thuốc ra)
    if (!prevState[activeReminderBox]) {
      Serial.printf("[REMINDER] Da lay thuoc o hop %d. Tat nhac nho.\n", activeReminderBox + 1);
      currentReminderState = STATE_IDLE;
      activeReminderBox = -1;
      drawClock(true); // Vẽ lại đồng hồ để xóa chữ trên màn hình
      updateLED();
      return;
    }

    // Kiểm tra mốc 30 giây
    if (currentReminderState == STATE_ACTIVE) {
      if (millis() - reminderStartTime >= 30000UL) {
        Serial.println("[REMINDER] Qua 30s chua lay thuoc! Chuyen sang canh bao tre.");
        currentReminderState = STATE_MISSED_ALARM;
        lastBlinkTime = millis();
        blinkLedState = true;

        // Hiển thị thông báo trễ giờ lên TFT
        tft.fillRect(0, 38, 240, 43, CLR_BG);
        tft.setTextColor(CLR_EMP_TEXT);
        tft.setTextSize(1);
        tft.setCursor(57, 48); // Căn giữa "DA TOI GIO UONG THUOC"
        tft.print("DA TOI GIO UONG THUOC");
        tft.setCursor(48, 62); // Căn giữa "NHUNG CHUA LAY THUOC RA!"
        tft.print("NHUNG CHUA LAY THUOC RA!");

        updateLED();
      }
    }

    // Nhấp nháy đèn LED xanh lá
    if (currentReminderState == STATE_MISSED_ALARM) {
      if (millis() - lastBlinkTime >= 500UL) {
        blinkLedState = !blinkLedState;
        lastBlinkTime = millis();
        updateLED();
      }
    }
  }

  // ─────────────────────────────────────────────────────────
  //  MQTT Callback
  // ─────────────────────────────────────────────────────────
  void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    msg.trim();
    Serial.printf("[MQTT] Received %s: %s\n", topic, msg.c_str());

    if (strcmp(topic, "esp/schedule") == 0) {

      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, msg);
      if (error) {
        Serial.printf("[MQTT] JSON parse failed: %s\n", error.c_str());
        return;
      }

      const char* dateValue = doc["date"] | "";
      String newDate = String(dateValue);
      newDate.trim();

      if (newDate.length() != 10 || newDate[4] != '-' || newDate[7] != '-') {
        Serial.println("[MQTT] Invalid date format (expected YYYY-MM-DD)");
        return;
      }

      int       times = doc["times"] | 0;
      JsonArray slots = doc["slots"].as<JsonArray>();
      if (slots.isNull() || slots.size() == 0) {
        Serial.println("[MQTT] slots rong hoac thieu");
        return;
      }

      Serial.printf("[MQTT] Date=%s  times=%d  slots=%d  numSchedules=%d\n",
                    newDate.c_str(), times, (int)slots.size(), numSchedules);

      // Tìm ngày đã tồn tại – dùng equals() tránh lỗi so sánh
      int existingIdx = -1;
      for (int i = 0; i < numSchedules; i++) {
        Serial.printf("  check schedules[%d].date='%s' vs newDate='%s'\n",
                      i, schedules[i].date.c_str(), newDate.c_str());
        if (schedules[i].date.equals(newDate)) { existingIdx = i; break; }
      }

      if (existingIdx != -1) {
        // Cập nhật lịch hiện có
        Serial.printf("[MQTT] Update existing idx=%d\n", existingIdx);
        schedules[existingIdx].numTimes = 0;
        for (int i = 0; i < (int)slots.size(); i++) {
          if (schedules[existingIdx].numTimes >= MAX_REMINDERS) break;
          String slotStr = slots[i].as<String>();
          slotStr.trim();
          schedules[existingIdx].times[schedules[existingIdx].numTimes++] = slotStr;
          Serial.printf("  [UPDATE] slot[%d] = %s\n", i, slotStr.c_str());
        }
      } else {
        // Nếu đầy → xóa slot cũ nhất để nhường chỗ
        if (numSchedules >= MAX_SCHEDULES) {
          Serial.println("[MQTT] Max schedules, xoa slot cu nhat");
          for (int i = 0; i < MAX_SCHEDULES - 1; i++) {
            schedules[i] = schedules[i + 1];
          }
          numSchedules = MAX_SCHEDULES - 1;
        }
        // Thêm lịch mới
        schedules[numSchedules].date     = newDate;
        schedules[numSchedules].numTimes = 0;
        for (int i = 0; i < (int)slots.size(); i++) {
          if (schedules[numSchedules].numTimes >= MAX_REMINDERS) break;
          String slotStr = slots[i].as<String>();
          slotStr.trim();
          schedules[numSchedules].times[schedules[numSchedules].numTimes++] = slotStr;
          Serial.printf("  [NEW] slot[%d] = %s\n", i, slotStr.c_str());
        }
        numSchedules++;
        Serial.printf("[MQTT] Added new schedule, numSchedules=%d\n", numSchedules);
      }

      saveReminders();
      updateNextReminder();
      drawNextReminder(true);

      String ackMsg = "{\"status\":\"received\",\"date\":\"" + newDate
                    + "\",\"times\":" + String(times) + "}";
      mqtt.publish("esp/schedule/ack", ackMsg.c_str(), true);
      Serial.printf("[MQTT] ACK: %s\n", ackMsg.c_str());
      return;
    }

    Serial.printf("[MQTT] Unknown topic: %s\n", topic);
  }

  // ─────────────────────────────────────────────────────────
  //  Vẽ UI
  // ─────────────────────────────────────────────────────────
  void drawMqttDot(bool connected) {
    uint16_t fill = connected ? CLR_MQTT_OK : CLR_MQTT_ERR;
    tft.fillRect(218, 10, 18, 18, CLR_HEADER_BG);
    tft.fillCircle(226, 19, 5, fill);
    tft.drawCircle(226, 19, 6, CLR_MQTT_RIM);
  }

  void showBootMsg(const char* line1, const char* line2 = "") {
    tft.fillRect(0, 40, 240, 40, CLR_CLOCK_BG);
    tft.setTextColor(CLR_CLOCK_FG); tft.setTextSize(1);
    tft.setCursor(8, 46); tft.print(line1);
    if (strlen(line2) > 0) {
      tft.setCursor(8, 58); tft.setTextColor(CLR_DATE_FG); tft.print(line2);
    }
  }

  void drawPillIcon(int16_t cx, int16_t cy, bool hasMed) {
    uint16_t colL   = hasMed ? CLR_HAS_PILL_A : CLR_EMP_PILL_A;
    uint16_t colR   = hasMed ? CLR_HAS_PILL_B : CLR_EMP_PILL_B;
    uint16_t cardBg = hasMed ? CLR_CARD_HAS_BG : CLR_CARD_EMP_BG;
    const int16_t r = 9, hw = 18;
    tft.fillRect(cx - hw - r - 1, cy - r - 1, (hw + r) * 2 + 2, r * 2 + 2, cardBg);
    tft.fillCircle(cx - hw, cy, r, colL);
    tft.fillRect(cx - hw, cy - r, hw, r * 2, colL);
    tft.fillCircle(cx + hw, cy, r, colR);
    tft.fillRect(cx, cy - r, hw, r * 2, colR);
    tft.drawFastVLine(cx, cy - r + 1, r * 2 - 2, 0xFFFF);
    tft.drawRoundRect(cx - hw - r, cy - r, (hw + r) * 2, r * 2, r, CLR_DIVIDER);
  }

  void drawCard(uint8_t idx, bool hasMed) {
    const int16_t cardW = 110, cardH = 113, gapX = 8, gapY = 6;
    const int16_t startX = 6, startY = 100;
    uint8_t  col = idx % 2, row = idx / 2;
    int16_t  x = startX + col * (cardW + gapX);
    int16_t  y = startY + row * (cardH + gapY);

    uint16_t bgCard   = hasMed ? CLR_CARD_HAS_BG : CLR_CARD_EMP_BG;
    uint16_t bdCard   = hasMed ? CLR_CARD_HAS_BD : CLR_CARD_EMP_BD;
    uint16_t txtClr   = hasMed ? CLR_HAS_TEXT    : CLR_EMP_TEXT;
    const char* statusTxt = hasMed ? "CON" : "HET";

    tft.fillRoundRect(x + 2, y + 2, cardW, cardH, 10, CLR_DIVIDER);
    tft.fillRoundRect(x, y, cardW, cardH, 10, bgCard);
    tft.drawRoundRect(x,     y,     cardW,     cardH,     10, bdCard);
    tft.drawRoundRect(x + 1, y + 1, cardW - 2, cardH - 2, 9,  bdCard);
    tft.fillRect(x + 10, y, cardW - 20, 4, bdCard);
    tft.setTextColor(CLR_LABEL); tft.setTextSize(1);
    tft.setCursor(x + 8, y + 10); tft.print(boxLabels[idx]);
    drawPillIcon(x + cardW / 2, y + 55, hasMed);
    tft.setTextSize(2); tft.setTextColor(txtClr);
    tft.setCursor(x + (cardW - 36) / 2, y + 84);
    tft.print(statusTxt);
    tft.fillCircle(x + cardW - 12, y + 12, 5, bdCard);
  }

  void drawClock(bool forceRedraw) {
    DateTime now = rtc.now();
    char timeStr[9], dateStr[20];
    sprintf(timeStr, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    sprintf(dateStr, "%s, %02d/%02d/%04d",
            dayNames[now.dayOfTheWeek()], now.day(), now.month(), now.year());

    bool timeChanged = (strcmp(timeStr, prevTimeStr) != 0);
    bool dateChanged = (strcmp(dateStr, prevDateStr) != 0);
    if (!forceRedraw && !timeChanged && !dateChanged) return;

    if (forceRedraw) {
      tft.fillRect(0, 38, 240, 43, CLR_CLOCK_BG);
      tft.drawFastHLine(0, 80, 240, CLR_DIVIDER);
    }
    if (forceRedraw || timeChanged) {
      tft.fillRect(6, 40, 160, 22, CLR_CLOCK_BG);
      tft.setTextColor(CLR_CLOCK_FG); tft.setTextSize(2);
      tft.setCursor(8, 42); tft.print(timeStr);
      tft.setTextSize(1); tft.setTextColor(CLR_UTC_FG);
      tft.setCursor(168, 42); tft.print("UTC+7");
      strcpy(prevTimeStr, timeStr);
    }
    if (forceRedraw || dateChanged) {
      tft.fillRect(6, 64, 220, 14, CLR_CLOCK_BG);
      tft.setTextColor(CLR_DATE_FG); tft.setTextSize(1);
      tft.setCursor(8, 65); tft.print(dateStr);
      strcpy(prevDateStr, dateStr);
    }
  }

  void drawNextReminder(bool force) {
    static char prevNextStr[50] = "";
    if (!force && strcmp(nextReminderStr, prevNextStr) == 0) return;

    tft.fillRect(0, 81, 240, 20, CLR_BG);
    tft.fillRoundRect(5, 82, 230, 16, 4, CLR_REMINDER_BG);
    tft.drawRoundRect(5, 82, 230, 16, 4, CLR_DIVIDER);
    tft.setTextColor(CLR_TITLE);
    tft.setTextSize(1);
    tft.setCursor(10, 86);
    tft.print(nextReminderStr);
    tft.drawFastHLine(0, 100, 240, CLR_DIVIDER);

    strcpy(prevNextStr, nextReminderStr);
  }

  void drawUI() {
    tft.fillScreen(CLR_BG);
    tft.fillRect(0, 0, 240, 38, CLR_HEADER_BG);
    tft.drawFastHLine(0, 37, 240, CLR_DIVIDER);
    tft.drawFastHLine(0, 0,  240, CLR_CARD_HAS_BD);
    tft.drawFastHLine(0, 1,  240, CLR_CARD_HAS_BD);
    tft.setTextColor(CLR_TITLE); tft.setTextSize(2);
    tft.setCursor(8, 10); tft.print("QUAN LY THUOC");
    drawMqttDot(mqtt.connected());
    drawClock(true);
    drawNextReminder(true);
    for (uint8_t i = 0; i < NUM_SENSORS; i++) drawCard(i, prevState[i]);
  }

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
        mqttTopics[i], prevState[i] ? "1" : "0");
    }
    Serial.printf("[MQTT] %s (TLS port %d)\n",
      mqtt.connected() ? "CONNECTED" : "OFFLINE", MQTT_PORT);
    Serial.println("----------------------------------------");
    Serial.printf("[SCHEDULES] %d lich luu:\n", numSchedules);
    for (int s = 0; s < numSchedules; s++) {
      Serial.printf("  [%d] date=%s times=%d: ", s, schedules[s].date.c_str(), schedules[s].numTimes);
      for (int i = 0; i < schedules[s].numTimes; i++)
        Serial.printf("%s ", schedules[s].times[i].c_str());
      Serial.println();
    }
    Serial.println("========================================");
  }

  // ─────────────────────────────────────────────────────────
  //  WiFi & MQTT
  // ─────────────────────────────────────────────────────────
  bool syncTimeFromNTP() {
    Serial.print("[WiFi] Ket noi: "); Serial.println(WIFI_SSID);
    showBootMsg("Ket noi WiFi...", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint8_t retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) { delay(500); Serial.print("."); retry++; }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] THAT BAI");
      showBootMsg("WiFi that bai!", "Dung gio RTC cu..."); delay(1500);
      return false;
    }
    Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP());
    showBootMsg("WiFi OK! Lay gio NTP...", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER1, NTP_SERVER2);
    struct tm timeinfo;
    uint8_t ntpRetry = 0;
    Serial.print("[NTP] Dong bo");
    while (!getLocalTime(&timeinfo) && ntpRetry < 20) { delay(500); Serial.print("."); ntpRetry++; }
    Serial.println();
    if (ntpRetry >= 20) {
      Serial.println("[NTP] THAT BAI");
      showBootMsg("NTP that bai!", "Dung gio RTC cu..."); delay(1500);
      return false;
    }
    rtc.adjust(DateTime(
      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
    char buf[40];
    sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d",
      timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    Serial.print("[NTP] OK: "); Serial.println(buf);
    showBootMsg("NTP OK!", buf); delay(1000);
    return true;
  }

  void mqttPublish(uint8_t idx, bool hasMed) {
    const char* payload = hasMed ? "1" : "0";
    if (mqtt.connected()) {
      bool ok = mqtt.publish(mqttTopics[idx], payload, true);
      Serial.printf("[MQTT] %s -> %s  (%s)\n", mqttTopics[idx], payload, ok ? "OK" : "FAIL");
    } else {
      Serial.printf("[MQTT] %s -> %s  (OFFLINE)\n", mqttTopics[idx], payload);
    }
  }

  void mqttReconnect() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (mqtt.connected()) return;
    Serial.print("[MQTT] Ket noi broker (TLS 8883)...");
    bool ok = (strlen(MQTT_USER) > 0)
              ? mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS_B)
              : mqtt.connect(MQTT_CLIENT);
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
  //  SETUP
  // ─────────────────────────────────────────────────────────
  void setup() {
    Serial.begin(115200);

    strip.begin();
    strip.setBrightness(80);
    strip.show();
    delay(500);
    Serial.println("\n=== KHOI DONG HE THONG ===");

    Wire.begin(8, 10);

    for (int i = 0; i < NUM_SENSORS; i++) {
      pinMode(sensorPins[i], INPUT);
      prevState[i] = false;
    }

    if (!rtc.begin()) {
      Serial.println("[LOI] Khong tim thay RTC DS3231!");
      while (1) delay(100);
    }
    Serial.println("[RTC] DS3231 OK");

    tft.begin();
    tft.setRotation(2);
    Serial.println("[TFT] ILI9341 OK");

    tft.fillScreen(CLR_BG);
    tft.fillRect(0, 0, 240, 38, CLR_HEADER_BG);
    tft.drawFastHLine(0, 37, 240, CLR_DIVIDER);
    tft.drawFastHLine(0, 0,  240, CLR_CARD_HAS_BD);
    tft.drawFastHLine(0, 1,  240, CLR_CARD_HAS_BD);
    tft.setTextColor(CLR_TITLE); tft.setTextSize(2);
    tft.setCursor(8, 10); tft.print("QUAN LY THUOC");

    syncTimeFromNTP();

    // ── Xóa dữ liệu rác từ lần nạp code cũ ──────────────
    // Chỉ cần chạy 1 lần. Sau khi kiểm tra Serial thấy
    // "Loaded 0 valid schedules" thì comment dòng dưới lại.
    clearAllReminders();

    loadReminders();
    updateNextReminder();

    wifiClient.setInsecure();
    mqtt.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt.setKeepAlive(30);
    mqtt.setSocketTimeout(10);
    mqtt.setCallback(mqttCallback);

    Serial.println("[SENSOR] Trang thai ban dau:");
    for (int i = 0; i < NUM_SENSORS; i++) {
      prevState[i] = digitalRead(sensorPins[i]);
      Serial.printf("  [S%d] %s: %s\n", i + 1, boxLabels[i],
                    prevState[i] ? "CON THUOC" : "HET THUOC");
    }

    showBootMsg("Ket noi MQTT TLS...", MQTT_SERVER);
    mqttReconnect();

    drawUI();
    updateLED();

    for (int i = 0; i < NUM_SENSORS; i++) mqttPublish(i, prevState[i]);

    Serial.println("[OK] He thong san sang!\n");
    printAllStatus();

    mySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
    mp3.reset();
    mp3.setVolume(30);
    mp3.playFileByIndexNumber(1);
    Serial.println("[MP3] Playing track 1 (boot sound)");
    while (mp3.getStatus() == MP3_STATUS_PLAYING) delay(100);
    delay(1000);
  }

  // ─────────────────────────────────────────────────────────
  //  LOOP
  // ─────────────────────────────────────────────────────────
  void loop() {
    // ── MQTT ──────────────────────────────────────────────
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

    // ── Đồng hồ ───────────────────────────────────────────
    if (currentReminderState == STATE_IDLE) {
      drawClock(false);
    }

    // ── Cập nhật lịch nhắc mỗi 30 giây ──────────────────
    if (millis() - lastNextUpdate > 30000UL) {
      updateNextReminder();
      drawNextReminder(false);
      lastNextUpdate = millis();
    }

    checkReminder();
    runReminderState();

    // ── Đọc cảm biến ─────────────────────────────────────
    for (int i = 0; i < NUM_SENSORS; i++) {
      bool state = digitalRead(sensorPins[i]);
      if (state != prevState[i]) {
        prevState[i] = state;
        drawCard(i, state);
        mqttPublish(i, state);
        updateLED();
        Serial.printf("[THAY DOI] HOP %d (%s): %s\n",
          i + 1, boxLabels[i],
          state ? "CON THUOC" : "HET THUOC");

        if (!state) {
          // Kiểm tra lấy thuốc sớm (khi không có lịch nhắc hoặc lấy sai hộp)
          bool isEarly = false;
          if (currentReminderState == STATE_IDLE) {
            isEarly = true;
          } else {
            if (i != activeReminderBox) {
              isEarly = true;
            }
          }

          if (isEarly) {
            // Hiển thị cảnh báo lấy sớm lên TFT
            tft.fillRect(0, 38, 240, 43, CLR_BG);
            tft.setTextColor(CLR_EMP_TEXT);
            tft.setTextSize(2);
            tft.setCursor(48, 42); // Căn giữa "CHUA DEN GIO"
            tft.print("CHUA DEN GIO");
            tft.setCursor(54, 62); // Căn giữa "UONG HOP X!"
            tft.printf("UONG HOP %d!", i + 1);

            Serial.printf("[CANH BAO] Lay thuoc som tai hop %d! Phat file 9.\n", i + 1);
            mp3.playFileByIndexNumber(9);
            delay(300);
            unsigned long waitStart = millis();
            // Đợi file 9 phát xong (tối đa 5 giây)
            while (mp3.getStatus() == MP3_STATUS_PLAYING && (millis() - waitStart < 5000UL)) {
              delay(100);
            }

            // Phát cảnh báo hết thuốc như cũ (file i + 1)
            playEmptyBoxVoice(i);
            delay(300);
            waitStart = millis();
            // Đợi file hết thuốc phát xong (tối đa 4 giây)
            while (mp3.getStatus() == MP3_STATUS_PLAYING && (millis() - waitStart < 4000UL)) {
              delay(100);
            }

            // Vẽ lại đồng hồ để xóa chữ cảnh báo trên màn hình
            drawClock(true);
          } else {
            // Lấy thuốc đúng hộp đang nhắc nhở
            playEmptyBoxVoice(i);
          }
        }
      }
    }

    delay(200);
  }
