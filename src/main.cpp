#define BLYNK_TEMPLATE_ID "TMPL6AgsBMKHP"     // Ganti dengan Template ID Blynk kamu
#define BLYNK_TEMPLATE_NAME "yang ini terakhir abmas yang bener aja"
#define BLYNK_AUTH_TOKEN "3LXJ9hxnURfimXdVz4TVbFnzmPauZU-y"  // Ganti dengan Auth Token Blynk kamu

#define BLYNK_PRINT Serial
#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>

LiquidCrystal_I2C lcd(0x27, 20, 4);
RTC_DS1307 rtc;

#define SDA_PIN 41
#define SCL_PIN 40
#define BTN_NAV_PIN 38     // Tombol Biru: Reset / Standby
#define BTN_SELECT_PIN 39  // Tombol Hijau: Start / Mulai Sistem
#define RELAY_PIN 36
#define LED_PIN 35
#define BUZZER_PIN 42

// WiFi Credential untuk Blynk
char auth[] = BLYNK_AUTH_TOKEN; // Ganti dengan Auth Token Blynk kamu
char ssid[] = "Wokwi-GUEST";     // Ganti SSID WiFi/Hotspot
char pass[] = ""; // Ganti Password WiFi

// =========================================================================
// AKSELERASI SIMULASI
// =========================================================================
unsigned long SIMULATION_SPEED = 1; // Default 1x
// =========================================================================

bool isSystemRunning = false;
int currentFase = 0;

// TIMESTAMPS UNTUK SIMULASI DAN PERGANTIAN HARI
uint32_t startRealUnix = 0; 
uint32_t baseSimUnix = 0; 
uint32_t simStartUnix = 0;  // Anchored to 00:00:00 of Start Day

int currentDayCount = 1;

bool needUpdate = true;
bool isOutputActive = false;

bool lastNavState = HIGH;
bool lastSelectState = HIGH;
unsigned long lastDebounceTimeNav = 0;
unsigned long lastDebounceTimeSelect = 0;
const unsigned long debounceDelay = 50;
unsigned long lastRtcUpdate = 0;

const int relayPins[8]  = {4, 5, 6, 7, 15, 16, 17, 18}; 
const int servoPins[4]  = {1, 2, 8, 14}; 
const int buttonPins[6] = {10, 11, 12, 13, 38, 39};            

const int angleTertutup = 0;  
const int angleTerbuka  = 90; 

// DURASI SIMULASI DALAM DETIK (480 Detik = 8 Menit Simulasi)
const unsigned long durasiPenyiramanManualDetik = 5; 
const unsigned long durasiOtomatisDetik = 480; 

unsigned long autoStartSimUnix = 0; // Menggunakan Unix Time Simulasi
bool isAutoTriggered = false;
int lastTriggeredHour = -1;

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;
const unsigned long BUZZER_ON_TIME  = 200;
const unsigned long BUZZER_OFF_TIME = 800;

bool isWatering[4] = {false, false, false, false};
unsigned long wateringEndTime[4] = {0, 0, 0, 0};

// State tombol manual untuk edge detection
bool lastButtonState[4] = {HIGH, HIGH, HIGH, HIGH};

void updateDisplay(unsigned long currentMillis, DateTime simNow);

void writeServoAngle(int index, int angle) {
  uint32_t duty = map(angle, 0, 180, 410, 2048); 
  ledcWrite(index, duty); // index di sini langsung merujuk ke PWM channel (0-3)
}

bool isAnyManualActive() {
  for (int i = 0; i < 4; i++) {
    if (isWatering[i]) return true;
  }
  return false;
}

void updateBuzzerPattern(unsigned long currentMillis) {
  if (isOutputActive || isAnyManualActive()) {
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
  bool shouldOutputsBeOn = isOutputActive || isAnyManualActive();
  digitalWrite(RELAY_PIN, shouldOutputsBeOn ? HIGH : LOW);
  digitalWrite(LED_PIN, shouldOutputsBeOn ? HIGH : LOW);
}

void triggerAllActuators(bool turnOn) {
  int targetAngle = turnOn ? angleTerbuka : angleTertutup;
  for (int i = 0; i < 4; i++) { 
    isWatering[i] = false; 
    writeServoAngle(i, targetAngle); 
  }
  for (int i = 0; i < 8; i++) { 
    digitalWrite(relayPins[i], turnOn ? HIGH : LOW); 
  }
}

bool checkPhaseSchedule(int phase, int hour) {
  if (phase == 0)      return (hour == 8 || hour == 12 || hour == 16);
  else if (phase == 1) return (hour >= 8 && hour <= 15);
  else if (phase == 2) return (hour >= 8 && hour <= 16);
  else if (phase == 3) return (hour == 8 || hour == 16);
  return false;
}

void checkSerialSpeedInput(DateTime realNow, DateTime &simNow) {
  if (Serial.available() > 0) {
    int val = Serial.parseInt();
    if (val > 0) {
      if (isSystemRunning) {
        baseSimUnix = simNow.unixtime();
        startRealUnix = realNow.unixtime();
      }
      SIMULATION_SPEED = val;
      Serial.print("-> Kecepatan Simulasi Diubah ke: ");
      Serial.print(SIMULATION_SPEED);
      Serial.println("x");
    }
  }
}

// =========================================================================
// BLYNK VIRTUAL PIN HANDLERS (V1 - V6)
// =========================================================================

// Kontrol Manual Servo 1 (V1)
BLYNK_WRITE(V1) {
  int pinValue = param.asInt();
  if (pinValue == 1 && !isAutoTriggered && !isWatering[0]) {
    isWatering[0] = true;
    wateringEndTime[0] = millis() + (durasiPenyiramanManualDetik * 1000UL);
    writeServoAngle(0, angleTerbuka);
    digitalWrite(relayPins[0], HIGH);
    digitalWrite(relayPins[1], HIGH);
    updateSystemOutputs();
    needUpdate = true;
  }
}

// Kontrol Manual Servo 2 (V2)
BLYNK_WRITE(V2) {
  int pinValue = param.asInt();
  if (pinValue == 1 && !isAutoTriggered && !isWatering[1]) {
    isWatering[1] = true;
    wateringEndTime[1] = millis() + (durasiPenyiramanManualDetik * 1000UL);
    writeServoAngle(1, angleTerbuka);
    digitalWrite(relayPins[2], HIGH);
    digitalWrite(relayPins[3], HIGH);
    updateSystemOutputs();
    needUpdate = true;
  }
}

// Kontrol Manual Servo 3 (V3)
BLYNK_WRITE(V3) {
  int pinValue = param.asInt();
  if (pinValue == 1 && !isAutoTriggered && !isWatering[2]) {
    isWatering[2] = true;
    wateringEndTime[2] = millis() + (durasiPenyiramanManualDetik * 1000UL);
    writeServoAngle(2, angleTerbuka);
    digitalWrite(relayPins[4], HIGH);
    digitalWrite(relayPins[5], HIGH);
    updateSystemOutputs();
    needUpdate = true;
  }
}

// Kontrol Manual Servo 4 (V4)
BLYNK_WRITE(V4) {
  int pinValue = param.asInt();
  if (pinValue == 1 && !isAutoTriggered && !isWatering[3]) {
    isWatering[3] = true;
    wateringEndTime[3] = millis() + (durasiPenyiramanManualDetik * 1000UL);
    writeServoAngle(3, angleTerbuka);
    digitalWrite(relayPins[6], HIGH);
    digitalWrite(relayPins[7], HIGH);
    updateSystemOutputs();
    needUpdate = true;
  }
}

// Tombol Start Sistem dari Blynk (V5) - Menggantikan Tombol Hijau
BLYNK_WRITE(V5) {
  int pinValue = param.asInt();
  if (pinValue == 1 && !isSystemRunning) {
    isSystemRunning = true;
    DateTime realNow = rtc.now();
    startRealUnix = realNow.unixtime(); 
    baseSimUnix = realNow.unixtime();
    
    DateTime startOfDay(realNow.year(), realNow.month(), realNow.day(), 0, 0, 0);
    simStartUnix = startOfDay.unixtime();

    currentFase = 0;
    currentDayCount = 1; 
    isAutoTriggered = false;
    isOutputActive = false;
    lastTriggeredHour = -1;
    needUpdate = true;
  }
}

// Tombol Reset / Standby dari Blynk (V6) - Menggantikan Tombol Biru
BLYNK_WRITE(V6) {
  int pinValue = param.asInt();
  if (pinValue == 1) {
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
  }
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 4; i++) {
    ledcSetup(i, 50, 14);          // Inisialisasi channel PWM (0-3)
    ledcAttachPin(servoPins[i], i); // Hubungkan pin servo ke channel
    writeServoAngle(i, angleTertutup);
  }

  for (int i = 0; i < 8; i++) { 
    pinMode(relayPins[i], OUTPUT); 
    digitalWrite(relayPins[i], LOW); 
  }
  for (int i = 0; i < 4; i++) { 
    pinMode(buttonPins[i], INPUT_PULLUP); 
  }
  
  pinMode(BTN_NAV_PIN, INPUT_PULLUP);
  pinMode(BTN_SELECT_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  rtc.begin();

  // Koneksi ke Blynk IoT
  Blynk.begin(auth, ssid, pass);
}

void loop() {
  Blynk.run(); // Menjaga koneksi Blynk tetap aktif

  unsigned long currentMillis = millis();
  DateTime realNow = rtc.now();

  // Hitung Waktu Simulasi & Pergantian Hari
  DateTime simNow;
  if (isSystemRunning) {
    uint32_t elapsedRealSeconds = realNow.unixtime() - startRealUnix;
    uint32_t addedSimSeconds = elapsedRealSeconds * SIMULATION_SPEED;
    uint32_t simUnix = baseSimUnix + addedSimSeconds;
    simNow = DateTime(simUnix);

    uint32_t totalSimSecondsPassed = simUnix - simStartUnix; 
    currentDayCount = (totalSimSecondsPassed / 86400UL) + 1;
  } else {
    simNow = realNow;
  }

  checkSerialSpeedInput(realNow, simNow);

  // 1. PENYIRAMAN MANUAL DENGAN EDGE DETECTION (FISIK & TIMEOUT)
  if (!isAutoTriggered) {
    for (int i = 0; i < 4; i++) {
      bool currentBtnState = digitalRead(buttonPins[i]);
      
      if (currentBtnState == LOW && lastButtonState[i] == HIGH && !isWatering[i]) {
        isWatering[i] = true;
        wateringEndTime[i] = currentMillis + (durasiPenyiramanManualDetik * 1000UL);
        writeServoAngle(i, angleTerbuka);
        digitalWrite(relayPins[i * 2], HIGH);
        digitalWrite(relayPins[(i * 2) + 1], HIGH);
        updateSystemOutputs();
        needUpdate = true;
      }
      lastButtonState[i] = currentBtnState;

      if (isWatering[i] && currentMillis >= wateringEndTime[i]) {
        isWatering[i] = false;
        writeServoAngle(i, angleTertutup);
        digitalWrite(relayPins[i * 2], LOW);
        digitalWrite(relayPins[(i * 2) + 1], LOW);
        updateSystemOutputs();
        needUpdate = true;
      }
    }
  }

  // 2. Update Buzzer
  updateBuzzerPattern(currentMillis);

  // 3. Tombol Reset / Standby Fisik (BTN_NAV_PIN)
  bool readingNav = digitalRead(BTN_NAV_PIN);
  if (readingNav != lastNavState) lastDebounceTimeNav = currentMillis;
  if ((currentMillis - lastDebounceTimeNav) > debounceDelay) {
    static bool navState = HIGH;
    if (readingNav == LOW && navState == HIGH) {
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
    }
    navState = readingNav;
  }
  lastNavState = readingNav;

  // 4. Tombol Start Fisik (BTN_SELECT_PIN)
  bool readingSelect = digitalRead(BTN_SELECT_PIN);
  if (readingSelect != lastSelectState) lastDebounceTimeSelect = currentMillis;
  if ((currentMillis - lastDebounceTimeSelect) > debounceDelay) {
    static bool selectState = HIGH;
    if (readingSelect == LOW && selectState == HIGH) {
      isSystemRunning = true;
      startRealUnix = realNow.unixtime(); 
      baseSimUnix = realNow.unixtime();
      
      DateTime startOfDay(realNow.year(), realNow.month(), realNow.day(), 0, 0, 0);
      simStartUnix = startOfDay.unixtime();

      currentFase = 0;
      currentDayCount = 1; 
      isAutoTriggered = false;
      isOutputActive = false;
      lastTriggeredHour = -1;
      needUpdate = true;
    }
    selectState = readingSelect;
  }
  lastSelectState = readingSelect;

  // 5. LOGIKA OTOMATIS & FASE
  if (isSystemRunning) {
    if (currentDayCount <= 10)      currentFase = 0; 
    else if (currentDayCount <= 30) currentFase = 1; 
    else if (currentDayCount <= 60) currentFase = 2; 
    else if (currentDayCount <= 75) currentFase = 3; 
    else isSystemRunning = false; 

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
    lastRtcUpdate = currentMillis; 
    needUpdate = true; 
  }

  if (needUpdate) {
    updateDisplay(currentMillis, simNow);
    needUpdate = false;
  }
}

void updateDisplay(unsigned long currentMillis, DateTime simNow) {
  // 1. TAMPILAN LCD FISIK
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
  bool isSystemActive = isOutputActive || isAnyManualActive();
  lcd.print(isSystemActive ? "STATUS : Keran Aktif" : "STATUS  : STANDBY   ");

  lcd.setCursor(0, 3);
  char line4Buf[21];
  long remainingSimSeconds = 0;

  if (isSystemActive) {
    if (isAutoTriggered) {
      uint32_t simSecondsPassed = simNow.unixtime() - autoStartSimUnix;
      if (simSecondsPassed < durasiOtomatisDetik) {
        remainingSimSeconds = durasiOtomatisDetik - simSecondsPassed;
      }
    } else if (isAnyManualActive()) {
      for (int i = 0; i < 4; i++) {
        if (isWatering[i] && currentMillis < wateringEndTime[i]) {
          long sec = (wateringEndTime[i] - currentMillis) / 1000UL;
          if (sec > remainingSimSeconds) remainingSimSeconds = sec;
        }
      }
    }

    int displayMin = remainingSimSeconds / 60;
    int displaySec = remainingSimSeconds % 60;
    snprintf(line4Buf, sizeof(line4Buf), "DURASI  : %02d:%02d      ", displayMin, displaySec);
  } else {
    snprintf(line4Buf, sizeof(line4Buf), "JAM     : %02d:%02d:%02d  ", simNow.hour(), simNow.minute(), simNow.second());
  }
  lcd.print(line4Buf);

  // =========================================================================
  // 2. KIRIM KE 4 VIRTUAL PIN BLYNK TERPISAH (V7 - V10)
  // =========================================================================
  
  // Tentukan String Fase (V7)
  const char* faseStr = "STANDBY";
  if (isSystemRunning) {
    if (currentFase == 0) faseStr = "Awal";
    else if (currentFase == 1) faseStr = "Pembungaan";
    else if (currentFase == 2) faseStr = "Pembesaran";
    else if (currentFase == 3) faseStr = "Pematangan";
  }
  Blynk.virtualWrite(V7, faseStr);

  // Tentukan Hari Ke (V8)
  if (isSystemRunning) {
    Blynk.virtualWrite(V8, currentDayCount);
  } else {
    Blynk.virtualWrite(V8, "-");
  }

  // Tentukan Status Sistem (V9)
  Blynk.virtualWrite(V9, isSystemActive ? "Keran Aktif" : "STANDBY");

  // Tentukan Jam atau Durasi Sisa (V10)
  if (isSystemActive) {
    int displayMin = remainingSimSeconds / 60;
    int displaySec = remainingSimSeconds % 60;
    char durasiBuf[10];
    snprintf(durasiBuf, sizeof(durasiBuf), "%02d:%02d", displayMin, displaySec);
    Blynk.virtualWrite(V10, durasiBuf);
  } else {
    char jamBuf[10];
    snprintf(jamBuf, sizeof(jamBuf), "%02d:%02d:%02d", simNow.hour(), simNow.minute(), simNow.second());
    Blynk.virtualWrite(V10, jamBuf);
  }
}