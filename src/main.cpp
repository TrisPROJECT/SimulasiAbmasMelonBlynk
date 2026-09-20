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
#define BTN_MANUAL_MAIN_PIN 21
#define RELAY_PIN 8   // Relay utama di bawah (kontrol LED bawah)
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
bool isMainManualActive = false;

// State Tombol
bool lastNavState = LOW;
bool lastSelectState = LOW;  

unsigned long lastRtcUpdate = 0;

// 4 Relay Atas
const int relayPins[4]  = {15, 16, 17, 18}; 
const int buttonPins[6] = {10, 11, 12, 13, 38, 39};            

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
bool blynkManualState[4] = {false, false, false, false};

// FORWARD DECLARATIONS
void updateDisplay(unsigned long currentMillis, DateTime simNow);
void setPhaseAndStart(int targetPhase, DateTime realNow);
void updateSystemOutputs();
void triggerAllActuators(bool turnOn);

bool isAnyManualActive() {
  for (int i = 0; i < 4; i++) {
    if (isWatering[i]) return true;
  }
  return false;
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
  // Relay Utama HANYA merespons jadwal otomatis ATAU tombol manual utamanya sendiri
  bool isMainRelayOn = isOutputActive || isMainManualActive;
  
  // Fisik Relay: LOW untuk nyala, HIGH untuk mati
  digitalWrite(RELAY_PIN, isMainRelayOn ? LOW : HIGH);
}

void triggerAllActuators(bool turnOn) {
  // Fisik Relay: LOW untuk nyala, HIGH untuk mati
  for (int i = 0; i < 4; i++) { 
    isWatering[i] = false; 
    digitalWrite(relayPins[i], turnOn ? LOW : HIGH); 
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
  
  updateSystemOutputs();
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
BLYNK_WRITE(V1) { blynkManualState[0] = (param.asInt() == 1); needUpdate = true; }
BLYNK_WRITE(V2) { blynkManualState[1] = (param.asInt() == 1); needUpdate = true; }
BLYNK_WRITE(V3) { blynkManualState[2] = (param.asInt() == 1); needUpdate = true; }
BLYNK_WRITE(V4) { blynkManualState[3] = (param.asInt() == 1); needUpdate = true; }

BLYNK_WRITE(V5) {
  if (param.asInt() == 1) {
    DateTime realNow = rtc.now();
    int targetFase = isSystemRunning ? (currentFase + 1) % 4 : 0;
    setPhaseAndStart(targetFase, realNow);
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

  // Set Relay ke HIGH agar posisi awal mati (karena Active-LOW)
  for (int i = 0; i < 4; i++) { 
    pinMode(relayPins[i], OUTPUT); 
    digitalWrite(relayPins[i], HIGH); 
  }
  
  // Kembalikan ke PULLDOWN karena kabel fisikmu tersambung ke 3.3V
  for (int i = 0; i < 4; i++) { 
    pinMode(buttonPins[i], INPUT_PULLDOWN); 
  }

  pinMode(BTN_NAV_PIN, INPUT_PULLDOWN);
  pinMode(BTN_SELECT_PIN, INPUT_PULLDOWN);
  pinMode(BTN_MANUAL_MAIN_PIN, INPUT_PULLDOWN); 
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Relay utama dimatikan awal (Active-LOW)
  
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  rtc.begin();

  lcd.setCursor(0, 0);
  lcd.print("Menyambung WiFi...  ");

  WiFi.begin(ssid, pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(200);
  }

  Blynk.config(auth);
  if (WiFi.status() == WL_CONNECTED) Blynk.connect(5000);

  updateSystemOutputs();
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

  // 1. KONTROL MANUAL TOMBOL (Kabel 3.3V -> Sinyalnya HIGH saat ditekan)
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

  // 3. LOGIKA TOMBOL BIRU (RESET / STANDBY)
  bool currentNavState = digitalRead(BTN_NAV_PIN);
  if (currentNavState == HIGH && lastNavState == LOW) { 
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

  // 4. LOGIKA TOMBOL HIJAU (START / PINDAH FASE)
  bool currentSelectState = digitalRead(BTN_SELECT_PIN);
  if (currentSelectState == HIGH && lastSelectState == LOW) { 
    int targetFase = isSystemRunning ? (currentFase + 1) % 4 : 0;
    Serial.print("PINDAH FASE KE: ");
    Serial.println(targetFase);
    setPhaseAndStart(targetFase, realNow);
  }
  lastSelectState = currentSelectState;

  // LOGIKA TOMBOL MERAH MANUAL MAIN (Sinyal HIGH saat ditekan)
  bool currentMainManualState = (digitalRead(BTN_MANUAL_MAIN_PIN) == HIGH); 
  if (isMainManualActive != currentMainManualState) {
    isMainManualActive = currentMainManualState;
    updateSystemOutputs(); 
    needUpdate = true;     
  }

  // 5. LOGIKA OTOMATIS
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
  lcd.print(isSystemActive ? "STATUS : Keran Aktif" : "STATUS  : STANDBY   ");

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
  Blynk.virtualWrite(V7, faseStr);
  Blynk.virtualWrite(V8, isSystemRunning ? String(currentDayCount) : "-");
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