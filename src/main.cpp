#define BLYNK_TEMPLATE_ID "TMPL6AgsBMKHP"
#define BLYNK_TEMPLATE_NAME "yang ini terakhir abmas yang bener aja"
#define BLYNK_AUTH_TOKEN "3LXJ9hxnURfimXdVz4TVbFnzmPauZU-y"

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
#define BTN_NAV_PIN 38
#define BTN_SELECT_PIN 39
#define RELAY_PIN 36
#define LED_PIN 35
#define BUZZER_PIN 42

char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = "Wokwi-GUEST";
char pass[] = "";

unsigned long SIMULATION_SPEED = 1;

bool isSystemRunning = false;
int currentFase = 0;

uint32_t startRealUnix = 0; 
uint32_t baseSimUnix = 0; 
uint32_t simStartUnix = 0;  

int currentDayCount = 1;

bool needUpdate = true;
bool isOutputActive = false;

// State Tombol Hijau (Reset)
bool lastNavState = HIGH;

// State Tombol Biru (Pindah Fase)
bool lastSelectState = HIGH;

unsigned long lastRtcUpdate = 0;

const int relayPins[8]  = {4, 5, 6, 7, 15, 16, 17, 18}; 
const int servoPins[4]  = {1, 2, 8, 14}; 
const int buttonPins[6] = {10, 11, 12, 13, 38, 39};            

const int angleTertutup = 0;  
const int angleTerbuka  = 90; 

const unsigned long durasiOtomatisDetik = 480; // 8 Menit Simulasi

unsigned long autoStartSimUnix = 0; 
bool isAutoTriggered = false;
int lastTriggeredHour = -1;

unsigned long lastBuzzerToggle = 0;
bool buzzerState = false;
const unsigned long BUZZER_ON_TIME  = 200;
const unsigned long BUZZER_OFF_TIME = 800;

// State Latching Penyiraman Manual
bool isWatering[4] = {false, false, false, false};
bool lastButtonState[4] = {HIGH, HIGH, HIGH, HIGH};

// FORWARD DECLARATIONS
void updateDisplay(unsigned long currentMillis, DateTime simNow);
void setPhaseAndStart(int targetPhase, DateTime realNow);
void updateSystemOutputs();
void triggerAllActuators(bool turnOn);

void writeServoAngle(int index, int angle) {
  uint32_t duty = map(angle, 0, 180, 410, 2048); 
  ledcWrite(index, duty); 
}

bool isAnyManualActive() {
  for (int i = 0; i < 4; i++) {
    if (isWatering[i]) return true;
  }
  return false;
}

void toggleManualWatering(int index) {
  if (isAutoTriggered) return; 

  isWatering[index] = !isWatering[index]; 
  
  int targetAngle = isWatering[index] ? angleTerbuka : angleTertutup;
  writeServoAngle(index, targetAngle);
  digitalWrite(relayPins[index * 2], isWatering[index] ? HIGH : LOW);
  digitalWrite(relayPins[(index * 2) + 1], isWatering[index] ? HIGH : LOW);
  
  updateSystemOutputs();
  needUpdate = true;
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

// BLYNK HANDLERS
BLYNK_WRITE(V1) { if (param.asInt() == 1) toggleManualWatering(0); }
BLYNK_WRITE(V2) { if (param.asInt() == 1) toggleManualWatering(1); }
BLYNK_WRITE(V3) { if (param.asInt() == 1) toggleManualWatering(2); }
BLYNK_WRITE(V4) { if (param.asInt() == 1) toggleManualWatering(3); }

BLYNK_WRITE(V5) {
  if (param.asInt() == 1) {
    DateTime realNow = rtc.now();
    int nextFase = isSystemRunning ? (currentFase + 1) % 4 : 0;
    setPhaseAndStart(nextFase, realNow);
  }
}

BLYNK_WRITE(V6) {
  if (param.asInt() == 1) {
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
    ledcSetup(i, 50, 14);          
    ledcAttachPin(servoPins[i], i); 
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

  Blynk.begin(auth, ssid, pass);
}

void loop() {
  Blynk.run();

  unsigned long currentMillis = millis();
  DateTime realNow = rtc.now();

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

  // 1. KONTROL MANUAL LATCHING (FISIK)
  for (int i = 0; i < 4; i++) {
    bool currentBtnState = digitalRead(buttonPins[i]);
    if (currentBtnState == LOW && lastButtonState[i] == HIGH) {
      toggleManualWatering(i);
    }
    lastButtonState[i] = currentBtnState;
  }

  // 2. UPDATE BUZZER PATTERN
  updateBuzzerPattern(currentMillis);

  // 3. LOGIKA TOMBOL BIRU (BTN_NAV_PIN / Pin 38) -> RESET / STANDBY
  bool currentNavState = digitalRead(BTN_NAV_PIN);
  if (currentNavState == LOW && lastNavState == HIGH) {
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
  }
  lastNavState = currentNavState;

  // 4. LOGIKA TOMBOL HIJAU (BTN_SELECT_PIN / Pin 39) -> START / PINDAH FASE NEXT
  bool currentSelectState = digitalRead(BTN_SELECT_PIN);
  if (currentSelectState == LOW && lastSelectState == HIGH) {
    int targetFase = isSystemRunning ? (currentFase + 1) % 4 : 0;
    Serial.print("PINDAH FASE KE: ");
    Serial.println(targetFase);
    setPhaseAndStart(targetFase, realNow);
  }
  lastSelectState = currentSelectState;

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
  // 1. DISPLAY LCD
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

  // Baris ke-4: Tetap Jam berjalan
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

  // 2. DASHBOARD BLYNK (V7 - V10)
  const char* faseStr = "STANDBY";
  if (isSystemRunning) {
    if (currentFase == 0) faseStr = "Awal";
    else if (currentFase == 1) faseStr = "Pembungaan";
    else if (currentFase == 2) faseStr = "Pembesaran";
    else if (currentFase == 3) faseStr = "Pematangan";
  }
  Blynk.virtualWrite(V7, faseStr);

  if (isSystemRunning) {
    Blynk.virtualWrite(V8, currentDayCount);
  } else {
    Blynk.virtualWrite(V8, "-");
  }

  Blynk.virtualWrite(V9, isSystemActive ? "Keran Aktif" : "STANDBY");

  if (isAutoTriggered) {
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