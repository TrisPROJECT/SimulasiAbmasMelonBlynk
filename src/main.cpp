// =====================================================================
//  Penyiraman Otomatis ESP32-S3 + WiFiManager + Blynk + Continuous Log
//  - Jadwal PER FASE (fungsi checkPhaseSchedule), jalan dengan/tanpa WiFi
//  - Otomatis: HANYA pompa nyala, valve tidak dialiri arus
//  - Safety manual: maksimal 3 valve manual bersamaan (<4)
//  - Set jam RTC manual via Serial: ketik j1835 (18:35)
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

// Timeout portal AP WiFiManager (detik)
#define PORTAL_TIMEOUT_SEC 30

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

const unsigned long durasiOtomatisDetik = 480; // 8 Menit

uint32_t autoStartSimUnix = 0;
bool isAutoTriggered = false;
int lastTriggeredTimeSlot = -1;

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;
const unsigned long BUZZER_ON_TIME  = 200;
const unsigned long BUZZER_OFF_TIME = 800;

bool isWatering[4] = {false, false, false, false};

volatile bool blynkManualState[4] = {false, false, false, false};
volatile bool blynkMainManualState = false;

volatile bool reqNextPhase = false;
volatile bool reqReset = false;

// --- SAFETY MANUAL ---
const int MAX_MANUAL_VALVES = 3;
volatile bool manualSafetyTrip = false;

portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
char stFase[16]   = "STANDBY";
char stHari[8]    = "-";
char stStatus[16] = "STANDBY";
char stWaktu[12]  = "--:--:--";

bool ntpConfigured = false;
bool ntpSynced = false;
unsigned long lastNtpAttempt = 0;

// Jam yang dipakai penjadwal (untuk log)
volatile int dbgHour = 0;
volatile int dbgMinute = 0;

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
void appLoop();
void appTask(void* pv);
void setPhaseAndStartCustomDay(int targetPhase, int customDay, DateTime realNow);
void rebaseSimToRtc(DateTime now);

bool isAnyManualActive() {
  for (int i = 0; i < 4; i++) {
    if (isWatering[i]) return true;
  }
  return false;
}

// Pompa aktif jika: jadwal otomatis, atau manual (selama tidak safety trip)
bool isPumpActive() {
  return isOutputActive || (!manualSafetyTrip && (isAnyManualActive() || isMainManualActive));
}

// --- FUNGSI LOG SERIAL MONITOR ---
void printSystemLog() {
  Serial.println("\n==================================================");
  Serial.println("[STATUS SYSTEM LOG - 1s LOOP]");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" > WiFi Status    : CONNECTED (STA Router)\n > IP Address      : ");
    Serial.println(WiFi.localIP());
  } else if ((WiFi.getMode() & WIFI_AP) || (WiFi.softAPIP() != IPAddress(0,0,0,0))) {
    Serial.print(" > WiFi Status    : CAPTIVE PORTAL (AP Active)\n > IP Address      : ");
    Serial.print(WiFi.softAPIP());
    Serial.println(" [Connect to Setup-Melon-IoT]");
  } else {
    Serial.println(" > WiFi Status    : OFFLINE (jadwal tetap jalan)");
    Serial.println(" > IP Address      : 0.0.0.0");
  }

  Serial.printf(" > Blynk Cloud     : %s\n", Blynk.connected() ? "ONLINE" : "OFFLINE");
  Serial.printf(" > System State    : %s\n", isSystemRunning ? "RUNNING" : "STANDBY");
  Serial.printf(" > Fase            : %s\n", stFase);
  Serial.printf(" > Hari Ke         : %s\n", stHari);
  Serial.printf(" > Status Pompa    : %s\n", stStatus);
  Serial.printf(" > Waktu / Durasi  : %s\n", stWaktu);
  Serial.printf(" > Jam Penjadwal   : %02d:%02d (fase %d) | NTP: %s\n", dbgHour, dbgMinute, currentFase, ntpSynced ? "SINKRON" : "BELUM");
  Serial.printf(" > Manual 1..4     : [%d, %d, %d, %d]\n", isWatering[0], isWatering[1], isWatering[2], isWatering[3]);
  Serial.printf(" > Main Manual     : %s\n", isMainManualActive ? "AKTIF" : "NONAKTIF");
  Serial.printf(" > Safety Trip     : %s\n", manualSafetyTrip ? "TRIP (lepas semua tombol valve)" : "OK");
  Serial.printf(" > Auto Trigger    : %d | OutputActive: %d\n", isAutoTriggered, isOutputActive);
  Serial.printf(" > Relay Pompa     : %d (0 = NYALA)\n", digitalRead(RELAY_PIN));
  Serial.printf(" > Relay Valve 1..4: [%d, %d, %d, %d] (0 = NYALA)\n",
                digitalRead(relayPins[0]), digitalRead(relayPins[1]),
                digitalRead(relayPins[2]), digitalRead(relayPins[3]));
  Serial.println("==================================================");
}

// --- FREERTOS TASK LOG SERIAL DI CORE 0 ---
void serialLogTask(void* pv) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  for (;;) {
    printSystemLog();
    vTaskDelay(pdMS_TO_TICKS(1000));
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
  if (isPumpActive()) {
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

// SATU-SATUNYA tempat menulis ke relay (active-LOW: LOW = nyala, HIGH = mati)
// Pompa : nyala saat otomatis ATAU manual
// Valve : nyala HANYA saat manual (isWatering)
void updateSystemOutputs() {
  digitalWrite(RELAY_PIN, isPumpActive() ? LOW : HIGH);

  for (int i = 0; i < 4; i++) {
    digitalWrite(relayPins[i], isWatering[i] ? LOW : HIGH);
  }
}

// Pastikan semua valve manual mati, lalu terapkan ke relay.
void triggerAllActuators(bool turnOn) {
  for (int i = 0; i < 4; i++) {
    isWatering[i] = false;
  }
  updateSystemOutputs();
}

// --- PEMERIKSAAN JADWAL (JAM DAN MENIT SPESIFIK) ---
// Ubah jam penyiraman langsung di baris fase masing-masing.
// Contoh 06:40 -> (hour == 6 && minute == 40)
bool checkPhaseSchedule(int phase, int hour, int minute) {
  if (phase == 0) {         // Fase 0 AWAL (sama dengan fase 1), dipakai juga saat STANDBY
    return (hour == 7 && minute == 16) ||
           (hour == 15 && minute == 0)  ||
           (hour == 20 && minute == 30);
  }
  else if (phase == 1) {    // Fase 1 PEMBUNGAAN
    return (hour == 7  && minute == 16) ||
           (hour == 15 && minute == 0)  ||
           (hour == 20 && minute == 30);
  }
  else if (phase == 2) {    // Fase 2 PEMBESARAN
    return (hour == 9  && minute == 30) ||
           (hour == 16 && minute == 0)  ||
           (hour == 21 && minute == 0);
  }
  else if (phase == 3) {    // Fase 3 PEMATANGAN
    return (hour == 9  && minute == 0)  ||
           (hour == 17 && minute == 30) ||
           (hour == 21 && minute == 0);
  }
  else {
    return false;
  }
}

// Selaraskan hitungan simulasi ke jam RTC sekarang, nomor hari tetap.
// Dipakai saat RTC direset / diatur ulang ketika sistem RUNNING.
void rebaseSimToRtc(DateTime now) {
  uint32_t secOfDay = (uint32_t)now.hour() * 3600UL + (uint32_t)now.minute() * 60UL + now.second();
  startRealUnix = now.unixtime();
  baseSimUnix = simStartUnix + (uint32_t)(currentDayCount - 1) * 86400UL + secOfDay;
  saveSystemState();
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

  // Hitung simStartUnix agar jam/hari simulasi pas di hari awal fase tersebut
  DateTime startOfDay(realNow.year(), realNow.month(), realNow.day(), 0, 0, 0);
  simStartUnix = startOfDay.unixtime() - ((uint32_t)(currentDayCount - 1) * 86400UL);

  isAutoTriggered = false;
  isOutputActive = false;
  lastTriggeredTimeSlot = -1;
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
  lastTriggeredTimeSlot = -1;
  triggerAllActuators(false);
  needUpdate = true;
  saveSystemState();
}

// Serial:
//   j1835  -> set jam RTC ke 18:35 (tanggal tetap)
//   angka  -> kecepatan simulasi (mis. 10 untuk 10x)
void checkSerialSpeedInput(DateTime realNow, DateTime &simNow) {
  if (Serial.available() > 0) {
    char c = Serial.peek();

    if (c == 'j' || c == 'J') {
      Serial.read();
      int hhmm = Serial.parseInt();
      int h = hhmm / 100, m = hhmm % 100;
      if (h >= 0 && h < 24 && m >= 0 && m < 60) {
        DateTime n = rtc.now();
        rtc.adjust(DateTime(n.year(), n.month(), n.day(), h, m, 0));
        if (isSystemRunning) rebaseSimToRtc(rtc.now());
        lastTriggeredTimeSlot = -1;
        Serial.printf("-> Jam RTC diatur ke %02d:%02d\n", h, m);
      } else {
        Serial.println("-> Format salah. Contoh: j1840");
      }
      return;
    }

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

// Sinkron NTP hanya BONUS kalau ada WiFi. Jadwal tidak bergantung padanya.
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
    if (isSystemRunning) rebaseSimToRtc(rtc.now());
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

// --- FUNGSI PEMBANTU UNTUK INPUT HARI KUSTOM VIA BLYNK ---
void setPhaseAndStartCustomDay(int targetPhase, int customDay, DateTime realNow) {
  isSystemRunning = true;
  currentFase = targetPhase;
  currentDayCount = customDay;

  startRealUnix = realNow.unixtime();
  baseSimUnix = realNow.unixtime();

  DateTime startOfDay(realNow.year(), realNow.month(), realNow.day(), 0, 0, 0);
  simStartUnix = startOfDay.unixtime() - ((uint32_t)(currentDayCount - 1) * 86400UL);

  isAutoTriggered = false;
  isOutputActive = false;
  lastTriggeredTimeSlot = -1;
  needUpdate = true;

  updateSystemOutputs();
  saveSystemState();
}

// --- HANDLER INPUT HARI VIRTUAL PIN V12 ---
BLYNK_WRITE(V12) {
  int inputDay = param.asInt();

  if (inputDay < 1 || inputDay > 75) {
    Serial.printf("[BLYNK INPUT] Hari %d tidak valid! Reset ke Hari 1.\n", inputDay);
    inputDay = 1;
    Blynk.virtualWrite(V12, 1);
  }

  DateTime realNow = rtc.now();

  int targetFase = 0;
  if (inputDay <= 10)      targetFase = 0;
  else if (inputDay <= 30) targetFase = 1;
  else if (inputDay <= 60) targetFase = 2;
  else if (inputDay <= 75) targetFase = 3;

  setPhaseAndStartCustomDay(targetFase, inputDay, realNow);

  Serial.printf("[BLYNK INPUT] Hari berhasil diubah ke: %d | Fase: %d\n", currentDayCount, currentFase);
}


// LOGIKA UTAMA PERANGKAT (FREERTOS TASK)
void appLoop() {
  unsigned long currentMillis = millis();
  DateTime realNow = rtc.now();

  syncNtpIfNeeded();

  if (reqNextPhase) { reqNextPhase = false; goNextPhase(realNow); }
  if (reqReset)     { reqReset = false;     resetToStandby(); }

  DateTime simNow;
  if (isSystemRunning) {
    // Pengaman: RTC lebih "muda" dari waktu mulai tersimpan (RTC reset) -> rebase
    if (realNow.unixtime() < startRealUnix) {
      Serial.println("[RTC] Jam RTC lebih lama dari data tersimpan (RTC reset?). Rebase simulasi ke jam RTC.");
      rebaseSimToRtc(realNow);
    }

    uint32_t elapsedRealSeconds = realNow.unixtime() - startRealUnix;
    uint32_t addedSimSeconds = elapsedRealSeconds * SIMULATION_SPEED;
    uint32_t simUnix = baseSimUnix + addedSimSeconds;
    simNow = DateTime(simUnix);

    uint32_t totalSimSecondsPassed = simUnix - simStartUnix;
    currentDayCount = (totalSimSecondsPassed / 86400UL) + 1;

    int calculatedFase = currentFase;
    if (currentDayCount <= 10)      calculatedFase = 0;
    else if (currentDayCount <= 30) calculatedFase = 1;
    else if (currentDayCount <= 60) calculatedFase = 2;
    else if (currentDayCount <= 75) calculatedFase = 3;
    else {
      isSystemRunning = false;
      saveSystemState();
    }

    currentFase = calculatedFase;
  } else {
    simNow = realNow;
  }

  dbgHour = simNow.hour();
  dbgMinute = simNow.minute();

  checkSerialSpeedInput(realNow, simNow);

  // --- MANUAL OVERRIDE + SAFETY (aktif jika TIDAK sedang siram otomatis) ---
  if (!isAutoTriggered) {
    bool req[4];
    int reqCount = 0;
    for (int i = 0; i < 4; i++) {
      req[i] = (digitalRead(buttonPins[i]) == HIGH) || blynkManualState[i];
      if (req[i]) reqCount++;
    }

    if (reqCount > MAX_MANUAL_VALVES) {
      if (!manualSafetyTrip) {
        manualSafetyTrip = true;
        Serial.println("[SAFETY] Semua valve manual dinyalakan! Pompa & valve dimatikan.");
      }
    } else if (reqCount == 0) {
      if (manualSafetyTrip) {
        manualSafetyTrip = false;
        Serial.println("[SAFETY] Tombol valve dilepas, safety trip direset.");
      }
    }

    bool changed = false;
    for (int i = 0; i < 4; i++) {
      bool newState = manualSafetyTrip ? false : req[i];
      if (isWatering[i] != newState) {
        isWatering[i] = newState;
        changed = true;
      }
    }

    static bool lastTripApplied = false;
    if (changed || lastTripApplied != manualSafetyTrip) {
      lastTripApplied = manualSafetyTrip;
      updateSystemOutputs();
      needUpdate = true;
    }
  }

  updateBuzzerPattern(currentMillis);

  // --- TOMBOL FISIK NAVIGASI ---
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

  // --- PEMATANGAN/FASING OTOMATIS SAAT RUNNING ---
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
  }

  // --- PENJADWALAN OTOMATIS PER FASE (RUNNING MAUPUN STANDBY, TANPA SYARAT WIFI) ---
  if (isAutoTriggered) {
    uint32_t simSecondsPassed = simNow.unixtime() - autoStartSimUnix;

    if (simSecondsPassed >= durasiOtomatisDetik) {
      Serial.println("[JADWAL] Penyiraman Otomatis Selesai!");
      isAutoTriggered = false;
      isOutputActive = false;
      triggerAllActuators(false);
      needUpdate = true;
    }
  }
  else {
    int currentTimeSlot = (simNow.hour() * 100) + simNow.minute();
    bool isScheduleMatch = checkPhaseSchedule(currentFase, simNow.hour(), simNow.minute());

    if (isScheduleMatch) {
      if (currentTimeSlot != lastTriggeredTimeSlot) {
        Serial.printf("[JADWAL] POMPA NYALA di jam %02d:%02d fase %d (hanya pompa, valve mati)\n",
                      simNow.hour(), simNow.minute(), currentFase);
        isAutoTriggered = true;
        isOutputActive = true;
        autoStartSimUnix = simNow.unixtime();
        lastTriggeredTimeSlot = currentTimeSlot;

        triggerAllActuators(true);
        needUpdate = true;
      }
    } else {
      // Di luar menit jadwal: lepas kunci slot supaya jam yang sama bisa jalan lagi besok
      lastTriggeredTimeSlot = -1;
    }
  }

  // --- REFRESH DISPLAY ---
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
  bool isSystemActive = isPumpActive();
  bool isTripShown = manualSafetyTrip && !isAutoTriggered;
  if (isTripShown)         lcd.print("STATUS : SAFETY TRIP");
  else if (isSystemActive) lcd.print("STATUS : Pompa Aktif");
  else                     lcd.print("STATUS  : STANDBY   ");

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

  const char* statusStr = isTripShown ? "SAFETY TRIP" : (isSystemActive ? "Keran Aktif" : "STANDBY");
  publishStatus(faseStr, hariBuf, statusStr, waktuBuf);
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
  Wire.setTimeOut(100);

  // 4. INISIALISASI LCD & RTC
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting Sistem...");

  if (!rtc.begin()) {
    Serial.println("[ERROR] RTC DS3231 Tidak Terdeteksi!");
  } else {
    Serial.println("[OK] RTC DS3231 Terdeteksi.");
    if (rtc.lostPower()) {
      Serial.println("[RTC] Baterai habis / jam hilang! Atur jam: ketik j1835 di Serial, atau tunggu NTP.");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  loadSystemState();
  updateSystemOutputs();
  lastLcdReset = millis();

  // 5. JALANKAN LOGIKA UTAMA (JADWAL) SEKARANG, SEBELUM WIFI,
  //    supaya jadwal pompa jalan walau WiFi/portal belum selesai atau tidak ada.
  xTaskCreatePinnedToCore(appTask, "appTask", 8192, NULL, 1, NULL, 1);

  // 6. KONEKSI WIFIMANAGER (tidak menyentuh LCD supaya tidak bentrok dengan appTask)
  WiFi.disconnect(true, false); // JANGAN hapus kredensial tersimpan
  delay(200);
  WiFi.setSleep(false);

  WiFiManager wm;
  wm.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));

  DNSServer dnsServer;
  wm.setAPCallback([&dnsServer](WiFiManager *myWiFiManager) {
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
    Serial.println("[DNS] Wildcard Redirect Aktif! Memaksa Pop-Up Captive Portal di HP...");
  });

  wm.setCaptivePortalEnable(true);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_SEC);

  // wm.resetSettings(); // DIMATIKAN: aktifkan hanya kalau mau menghapus WiFi tersimpan

  bool res = wm.autoConnect("Setup-Melon-IoT");

  dnsServer.stop();

  if (!res) {
    Serial.println("[WiFi] Portal Timeout / Gagal. Mode Offline! (jadwal tetap jalan)");
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

// --- PEMBUNGKUS FREERTOS TASK UNTUK APPLOOP ---
void appTask(void* pv) {
  for (;;) {
    appLoop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}