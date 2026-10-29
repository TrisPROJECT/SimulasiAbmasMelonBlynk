// =====================================================================
//  Penyiraman Otomatis ESP32-S3 + WiFiManager + Blynk + Continuous Log
// =====================================================================

#define BLYNK_TEMPLATE_ID "TMPL6AgsBMKHP"
#define BLYNK_TEMPLATE_NAME "yang ini terakhir abmas yang bener aja"
#define BLYNK_AUTH_TOKEN "3LXJ9hxnURfimXdVz4TVbFnzmPauZU-y"

#define BLYNK_PRINT Serial

// --- LIBRARIES ---
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <time.h>
#include <Preferences.h>
#include <DNSServer.h>

LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS3231 rtc;
Preferences preferences;
BlynkTimer timer;

#define SDA_PIN 7
#define SCL_PIN 6
#define BTN_NAV_PIN 38
#define BTN_SELECT_PIN 39
#define BTN_MANUAL_MAIN_PIN 21
#define RELAY_PIN 8
#define BUZZER_PIN 42

char auth[] = BLYNK_AUTH_TOKEN;

const char* ntpServer = "pool.ntp.org";
const long   gmtOffset_sec = 25200; // WIB (UTC+7)
const int    daylightOffset_sec = 0;

unsigned long SIMULATION_SPEED = 1;

bool isSystemRunning = false;
int currentFase = 0;

uint32_t startRealUnix = 0;
uint32_t baseSimUnix = 0;
uint32_t simStartUnix = 0;

int currentDayCount = 1;

volatile bool needUpdate = true;
bool isOutputActive = false;
bool isMainManualActive = false;

bool lastNavState = LOW;
bool lastSelectState = LOW;

unsigned long lastRtcUpdate = 0;

unsigned long lastLcdReset = 0;
const unsigned long LCD_RESET_INTERVAL = 15000;
const unsigned long LCD_MIN_REFRESH_INTERVAL = 150;

const int relayPins[4]  = {15, 16, 17, 18};
const int buttonPins[6] = {10, 11, 12, 13, 38, 39};

const unsigned long durasiOtomatisDetik = 480;

unsigned long autoStartSimUnix = 0;
bool isAutoTriggered = false;
int lastTriggeredHour = -1;

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;
const unsigned long BUZZER_ON_TIME  = 200;
const unsigned long BUZZER_OFF_TIME = 800;

bool isWatering[4] = {false, false, false, false};

volatile bool blynkManualState[4] = {false, false, false, false};
volatile bool blynkMainManualState = false;

volatile bool reqNextPhase = false;
volatile bool reqReset = false;

portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
char stFase[16]   = "STANDBY";
char stHari[8]    = "-";
char stStatus[16] = "STANDBY";
char stWaktu[12]  = "--:--:--";

bool ntpConfigured = false;
bool ntpSynced = false;
unsigned long lastNtpAttempt = 0;

// FORWARD DECLARATIONS
void updateDisplay(unsigned long currentMillis, DateTime simNow);
void setPhaseAndStart(int targetPhase, DateTime realNow);
void updateSystemOutputs();
void triggerAllActuators(bool turnOn);
void saveSystemState();
void loadSystemState();
void goNextPhase(DateTime realNow);
void resetToStandby();
void printSystemLog();

bool isAnyManualActive() {
  for (int i = 0; i < 4; i++) {
    if (isWatering[i]) return true;
  }
  return false;
}

// --- FUNGSI LOG SERIAL MONITOR DENGAN FORMAT AMAN ---
void printSystemLog() {
  Serial.println("\n==================================================");
  Serial.println("[STATUS SYSTEM LOG - 4s LOOP]");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" > WiFi Status    : CONNECTED (STA Router)\n > IP Address      : ");
    Serial.println(WiFi.localIP());
  } else if ((WiFi.getMode() & WIFI_AP) || (WiFi.softAPIP() != IPAddress(0,0,0,0))) {
    Serial.print(" > WiFi Status    : CAPTIVE PORTAL (AP Active)\n > IP Address      : ");
    Serial.print(WiFi.softAPIP());
    Serial.println(" [Connect to Setup-Melon-IoT]");
  } else {
    Serial.println(" > WiFi Status    : CONNECTING / INITIALIZING");
    Serial.println(" > IP Address      : 0.0.0.0");
  }

  Serial.printf(" > Blynk Cloud     : %s\n", Blynk.connected() ? "ONLINE" : "OFFLINE");
  Serial.printf(" > System State    : %s\n", isSystemRunning ? "RUNNING" : "STANDBY");
  Serial.printf(" > Fase            : %s\n", stFase);
  Serial.printf(" > Hari Ke         : %s\n", stHari);
  Serial.printf(" > Status Pompa    : %s\n", stStatus);
  Serial.printf(" > Waktu / Durasi  : %s\n", stWaktu);
  Serial.printf(" > Manual 1..4     : [%d, %d, %d, %d]\n", isWatering[0], isWatering[1], isWatering[2], isWatering[3]);
  Serial.printf(" > Main Manual     : %s\n", isMainManualActive ? "AKTIF" : "NONAKTIF");
  Serial.println("==================================================");
}

// --- FREERTOS TASK LOG SERIAL DI CORE 0 ---
void serialLogTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  for (;;) {
    printSystemLog();
    vTaskDelay(pdMS_TO_TICKS(4000));
  }
}

// --- FUNGSI NVS FLASH MEMORY ---
void saveSystemState() {
  preferences.begin("system_state", false);
  preferences.putBool("isRunning", isSystemRunning);
  preferences.putInt("fase", currentFase);
  preferences.putInt("dayCount", currentDayCount);
  preferences.putUInt("startReal", startRealUnix);
  preferences.putUInt("baseSim", baseSimUnix);
  preferences.putUInt("simStart", simStartUnix);
  preferences.end();
  Serial.println("[NVS] Status sistem berhasil disimpan ke Flash!");
}

void loadSystemState() {
  preferences.begin("system_state", true);
  isSystemRunning = preferences.getBool("isRunning", false);
  currentFase = preferences.getInt("fase", 0);
  currentDayCount = preferences.getInt("dayCount", 1);
  startRealUnix = preferences.getUInt("startReal", 0);
  baseSimUnix = preferences.getUInt("baseSim", 0);
  simStartUnix = preferences.getUInt("simStart", 0);
  preferences.end();

  if (isSystemRunning) {
    Serial.printf("[NVS] Memulihkan Status: Running = YA, Fase = %d, Hari Ke = %d\n", currentFase, currentDayCount);
  } else {
    Serial.println("[NVS] Status Terakhir: STANDBY");
  }
}

void updateBuzzerPattern(unsigned long currentMillis) {
  if (isOutputActive || isAnyManualActive() || isMainManualActive) {
    if (buzzerState) {
      if (currentMillis - lastBuzzerToggle >= BUZZER_ON_TIME) {
        buzzerState = false;
        digitalWrite(BUZZER_PIN, LOW);
        lastBuzzerToggle = currentMillis;
      }
    } else {
      if (currentMillis - lastBuzzerToggle >= BUZZER_OFF_TIME) {
        buzzerState = true;
        digitalWrite(BUZZER_PIN, HIGH);
        lastBuzzerToggle = currentMillis;
      }
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false; 
  }
}

void updateSystemOutputs() {
  bool shouldOutputsBeOn = isOutputActive || isAnyManualActive() || isMainManualActive;
  digitalWrite(RELAY_PIN, shouldOutputsBeOn ? LOW : HIGH);
}

void triggerAllActuators(bool turnOn) {
  for (int i = 0; i < 4; i++) {
    isWatering[i] = false;
    digitalWrite(relayPins[i], turnOn ? LOW : HIGH);
  }
}

bool checkPhaseSchedule(int phase, int hour) {
  if (phase == 0)      return (hour == 8 || hour == 12 || hour == 16);
  else if (phase == 1) return (hour >= 8 || hour == 15);
  else if (phase == 2) return (hour == 8 || hour == 16 || hour == 20);
  else if (phase == 3) return (hour == 8 || hour == 16 || hour == 20);
  else return false;
}

void setPhaseAndStart(int targetPhase, DateTime realNow) {
  isSystemRunning = true;
  currentFase = targetPhase;

  if (targetPhase == 0)      currentDayCount = 1;
  else if (targetPhase == 1) currentDayCount = 11;
  else if (targetPhase == 2) currentDayCount = 31;
  else if (targetPhase == 3) currentDayCount = 61;

  startRealUnix = realNow.unixtime();
  baseSimUnix = realNow.unixtime();

  DateTime startOfDay(realNow.year(), realNow.month(), realNow.day(), 0, 0, 0);
  simStartUnix = startOfDay.unixtime() - ((uint32_t)(currentDayCount - 1) * 86400UL);

  isAutoTriggered = false;
  isOutputActive = false;
  lastTriggeredHour = -1;
  needUpdate = true;

  updateSystemOutputs();
  saveSystemState();
}

void goNextPhase(DateTime realNow) {
  int targetFase = isSystemRunning ? (currentFase + 1) % 4 : 0;
  Serial.print("PINDAH FASE KE: ");
  Serial.println(targetFase);
  setPhaseAndStart(targetFase, realNow);
}

void resetToStandby() {
  Serial.println("RESET SISTEM TO STANDBY");
  isSystemRunning = false;
  currentFase = 0;
  currentDayCount = 1;
  simStartUnix = 0;
  isAutoTriggered = false;
  isOutputActive = false;
  lastTriggeredHour = -1;
  updateSystemOutputs();
  triggerAllActuators(false);
  needUpdate = true;
  saveSystemState();
}

void checkSerialSpeedInput(DateTime realNow, DateTime &simNow) {
  if (Serial.available() > 0) {
    int val = Serial.parseInt();
    if (val > 0) {
      if (isSystemRunning) {
        baseSimUnix = simNow.unixtime();
        startRealUnix = realNow.unixtime();
        saveSystemState();
      }
      SIMULATION_SPEED = val;
      Serial.print("-> Kecepatan Simulasi Diubah ke: ");
      Serial.print(SIMULATION_SPEED);
      Serial.println("x");
    }
  }
}

void syncNtpIfNeeded() {
  if (ntpSynced || WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (lastNtpAttempt != 0 && now - lastNtpAttempt < 10000) return;
  lastNtpAttempt = now;

  if (!ntpConfigured) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    ntpConfigured = true;
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 200)) {
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
    ntpSynced = true;
    Serial.println("[NTP] RTC Berhasil Disinkronkan!");
  }
}

void publishStatus(const char* fase, const char* hari, const char* status, const char* waktu) {
  strlcpy(stFase, fase, sizeof(stFase));
  strlcpy(stHari, hari, sizeof(stHari));
  strlcpy(stStatus, status, sizeof(stStatus));
  strlcpy(stWaktu, waktu, sizeof(stWaktu));
}

char lastF[16] = "", lastH[8] = "", lastS[16] = "", lastW[12] = "";

void sendBlynkStatus() {
  if (!Blynk.connected()) return;

  if (strcmp(stFase, lastF))   { Blynk.virtualWrite(V7, stFase);   strlcpy(lastF, stFase, sizeof(lastF)); }
  if (strcmp(stHari, lastH))   { Blynk.virtualWrite(V8, stHari);   strlcpy(lastH, stHari, sizeof(lastH)); }
  if (strcmp(stStatus, lastS)) { Blynk.virtualWrite(V9, stStatus); strlcpy(lastS, stStatus, sizeof(lastS)); }
  if (strcmp(stWaktu, lastW))  { Blynk.virtualWrite(V10, stWaktu); strlcpy(lastW, stWaktu, sizeof(lastW)); }
}

// BLYNK HANDLERS
BLYNK_WRITE(V1) { blynkManualState[0] = (param.asInt() == 1); needUpdate = true; }
BLYNK_WRITE(V2) { blynkManualState[1] = (param.asInt() == 1); needUpdate = true; }
BLYNK_WRITE(V3) { blynkManualState[2] = (param.asInt() == 1); needUpdate = true; }
BLYNK_WRITE(V4) { blynkManualState[3] = (param.asInt() == 1); needUpdate = true; }

BLYNK_WRITE(V5) { if (param.asInt() == 1) reqNextPhase = true; }
BLYNK_WRITE(V6) { if (param.asInt() == 1) reqReset = true; }
BLYNK_WRITE(V11) { blynkMainManualState = (param.asInt() == 1); needUpdate = true; }

// LOGIKA UTAMA PERANGKAT (FREERTOS TASK)
void appLoop() {
  unsigned long currentMillis = millis();
  DateTime realNow = rtc.now();

  syncNtpIfNeeded();

  if (reqNextPhase) { reqNextPhase = false; goNextPhase(realNow); }
  if (reqReset)     { reqReset = false;     resetToStandby(); }

  DateTime simNow;
  if (isSystemRunning) {
    uint32_t elapsedRealSeconds = realNow.unixtime() - startRealUnix;
    uint32_t addedSimSeconds = elapsedRealSeconds * SIMULATION_SPEED;
    uint32_t simUnix = baseSimUnix + addedSimSeconds;
    simNow = DateTime(simUnix);

    uint32_t totalSimSecondsPassed = simUnix - simStartUnix;
    int calculatedDay = (totalSimSecondsPassed / 86400UL) + 1;

    if (calculatedDay != currentDayCount) {
      currentDayCount = calculatedDay;
      saveSystemState();
    }
  } else {
    simNow = realNow;
  }

  checkSerialSpeedInput(realNow, simNow);

  if (!isAutoTriggered) {
    for (int i = 0; i < 4; i++) {
      bool currentBtnState = digitalRead(buttonPins[i]);
      bool shouldBeWatering = (currentBtnState == HIGH) || blynkManualState[i];

      if (isWatering[i] != shouldBeWatering) {
        isWatering[i] = shouldBeWatering;
        digitalWrite(relayPins[i], isWatering[i] ? LOW : HIGH);
        updateSystemOutputs();
        needUpdate = true;
      }
    }
  }

  updateBuzzerPattern(currentMillis);

  bool currentNavState = digitalRead(BTN_NAV_PIN);
  if (currentNavState == HIGH && lastNavState == LOW) {
    resetToStandby();
  }
  lastNavState = currentNavState;

  bool currentSelectState = digitalRead(BTN_SELECT_PIN);
  if (currentSelectState == HIGH && lastSelectState == LOW) {
    goNextPhase(realNow);
  }
  lastSelectState = currentSelectState;

  bool currentMainManualState = (digitalRead(BTN_MANUAL_MAIN_PIN) == HIGH) || blynkMainManualState;
  if (isMainManualActive != currentMainManualState) {
    isMainManualActive = currentMainManualState;
    updateSystemOutputs();
    needUpdate = true;
  }

  if (isSystemRunning) {
    int oldFase = currentFase;
    if (currentDayCount <= 10)      currentFase = 0;
    else if (currentDayCount <= 30) currentFase = 1;
    else if (currentDayCount <= 60) currentFase = 2;
    else if (currentDayCount <= 75) currentFase = 3;
    else {
      isSystemRunning = false;
      saveSystemState();
    }

    if (oldFase != currentFase) {
      saveSystemState();
    }

    if (isAutoTriggered) {
      uint32_t simSecondsPassed = simNow.unixtime() - autoStartSimUnix;

      if (simSecondsPassed >= durasiOtomatisDetik) {
        isAutoTriggered = false;
        isOutputActive = false;
        updateSystemOutputs();
        triggerAllActuators(false);
        needUpdate = true;
      }
    }
    else {
      bool isScheduleMatch = checkPhaseSchedule(currentFase, simNow.hour());

      if (isScheduleMatch && (simNow.hour() != lastTriggeredHour)) {
        isAutoTriggered = true;
        isOutputActive = true;
        autoStartSimUnix = simNow.unixtime();
        lastTriggeredHour = simNow.hour();

        updateSystemOutputs();
        triggerAllActuators(true);
        needUpdate = true;
      }
    }
  } else {
    if (isOutputActive || isAutoTriggered) {
      isOutputActive = false;
      isAutoTriggered = false;
      updateSystemOutputs();
      triggerAllActuators(false);
      needUpdate = true;
    }
  }

  if (currentMillis - lastRtcUpdate >= 200) {
    needUpdate = true;
  }

  if (currentMillis - lastLcdReset >= LCD_RESET_INTERVAL) {
    lastLcdReset = currentMillis;
    lcd.init();
    lcd.backlight();
    needUpdate = true;
  }

  if (needUpdate && (currentMillis - lastRtcUpdate >= LCD_MIN_REFRESH_INTERVAL)) {
    lastRtcUpdate = currentMillis;
    updateDisplay(currentMillis, simNow);
    needUpdate = false;
  }
}

void appTask(void* pv) {
  for (;;) {
    appLoop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void updateDisplay(unsigned long currentMillis, DateTime simNow) {
  lcd.setCursor(0, 0);
  if (!isSystemRunning) {
    lcd.print("FASE: STANDBY       ");
  } else {
    if (currentFase == 0)      lcd.print("FASE: Awal          ");
    else if (currentFase == 1) lcd.print("FASE: Pembungaan    ");
    else if (currentFase == 2) lcd.print("FASE: Pembesaran    ");
    else if (currentFase == 3) lcd.print("FASE: Pematangan    ");
  }

  lcd.setCursor(0, 1);
  char line2Buf[21];
  if (isSystemRunning) {
    snprintf(line2Buf, sizeof(line2Buf), "HARI KE : %-2d        ", currentDayCount);
  } else {
    snprintf(line2Buf, sizeof(line2Buf), "HARI KE : -         ");
  }
  lcd.print(line2Buf);

  lcd.setCursor(0, 2);
  bool isSystemActive = isOutputActive || isAnyManualActive() || isMainManualActive;
  lcd.print(isSystemActive ? "STATUS : Pompa Aktif" : "STATUS  : STANDBY   ");

  lcd.setCursor(0, 3);
  char line4Buf[21];
  long remainingSimSeconds = 0;

  if (isAutoTriggered) {
    uint32_t simSecondsPassed = simNow.unixtime() - autoStartSimUnix;
    if (simSecondsPassed < durasiOtomatisDetik) {
      remainingSimSeconds = durasiOtomatisDetik - simSecondsPassed;
    }
    int displayMin = remainingSimSeconds / 60;
    int displaySec = remainingSimSeconds % 60;
    snprintf(line4Buf, sizeof(line4Buf), "DURASI  : %02d:%02d      ", displayMin, displaySec);
  } else {
    snprintf(line4Buf, sizeof(line4Buf), "JAM     : %02d:%02d:%02d  ", simNow.hour(), simNow.minute(), simNow.second());
  }
  lcd.print(line4Buf);

  const char* faseStr = "STANDBY";
  if (isSystemRunning) {
    if (currentFase == 0) faseStr = "Awal";
    else if (currentFase == 1) faseStr = "Pembungaan";
    else if (currentFase == 2) faseStr = "Pembesaran";
    else if (currentFase == 3) faseStr = "Pematangan";
  }

  char hariBuf[8];
  if (isSystemRunning) snprintf(hariBuf, sizeof(hariBuf), "%d", currentDayCount);
  else strlcpy(hariBuf, "-", sizeof(hariBuf));

  char waktuBuf[12];
  if (isAutoTriggered) {
    int displayMin = remainingSimSeconds / 60;
    int displaySec = remainingSimSeconds % 60;
    snprintf(waktuBuf, sizeof(waktuBuf), "%02d:%02d", displayMin, displaySec);
  } else {
    snprintf(waktuBuf, sizeof(waktuBuf), "%02d:%02d:%02d", simNow.hour(), simNow.minute(), simNow.second());
  }

  publishStatus(faseStr, hariBuf, isSystemActive ? "Keran Aktif" : "STANDBY", waktuBuf);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n[BOOT] Memulai Sistem ESP32-S3...");

  // 1. PIN IN/OUT
  for (int i = 0; i < 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH);
    pinMode(buttonPins[i], INPUT_PULLDOWN);
  }
  pinMode(BTN_NAV_PIN, INPUT_PULLDOWN);
  pinMode(BTN_SELECT_PIN, INPUT_PULLDOWN);
  pinMode(BTN_MANUAL_MAIN_PIN, INPUT_PULLDOWN);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // 2. JALANKAN SERIAL LOG TASK
  xTaskCreatePinnedToCore(serialLogTask, "serialLogTask", 8192, NULL, 1, NULL, 0);

  // 3. INISIALISASI I2C SAFE
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(250);

  // 4. INISIALISASI LCD & RTC
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting Sistem...");

  if (!rtc.begin()) {
    Serial.println("[ERROR] RTC DS3231 Tidak Terdeteksi! Lanjut tanpa RTC.");
  } else {
    Serial.println("[OK] RTC DS3231 Terdeteksi.");
    if (rtc.lostPower()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  loadSystemState();
  updateSystemOutputs();
  lastLcdReset = millis();

  // 5. KONEKSI WIFIMANAGER
  lcd.setCursor(0, 0);
  lcd.print("Konek WiFi & Blynk..");

  WiFi.disconnect(true, true); 
  delay(200);
  WiFi.setSleep(false);

  WiFiManager wm;
  wm.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  wm.setConfigPortalTimeout(180);
  
  DNSServer dnsServer;
  wm.setAPCallback([&dnsServer](WiFiManager *myWiFiManager) {
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    Serial.println("[DNS] Wildcard Redirect Aktif! Memaksa Pop-Up Captive Portal di HP...");
  });

  // PAKSA REDIRECT OTOMATIS SAAT HP CONNECT TO WIFI
  wm.setCaptivePortalEnable(true);          // Aktifkan pemicu pop-up Captive Portal
  wm.setConfigPortalTimeout(180);

  Serial.println("[WiFiManager] Reset Wi-Fi tersimpan...");
  wm.resetSettings();

  bool res = wm.autoConnect("Setup-Melon-IoT");

  // Begitu portal selesai/timeout, matikan DNS Server kustom
  dnsServer.stop();

  if (!res) {
    Serial.println("[WiFi] Portal Timeout / Gagal. Mode Offline!");
    lcd.clear();
    lcd.print("WiFi: Offline");
  } else {
    Serial.println("[WiFi] TERHUBUNG ke Router!");
    Serial.print("[WiFi] IP Address: ");
    Serial.println(WiFi.localIP());

    delay(1000);
    Blynk.config(auth, "128.199.144.129", 80);
    if (Blynk.connect(5000)) {
      Serial.println("[Blynk] TERHUBUNG KE DASHBOARD!");
    } else {
      Serial.println("[Blynk] Gagal konek ke Cloud!");
    }
  }

  timer.setInterval(1000L, sendBlynkStatus);
  xTaskCreatePinnedToCore(appTask, "appTask", 8192, NULL, 1, NULL, 1);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!Blynk.connected()) {
      Blynk.connect(1000);
    } else {
      Blynk.run();
    }
  }
  timer.run();
}