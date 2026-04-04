#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ============================================================
//  CONFIGURE THESE
// ============================================================
#define WIFI_SSID      "YourNetworkName"
#define WIFI_PASSWORD  "YourPassword"

#define FIREBASE_HOST  "https://medibuddy-27ca8-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH  "AIzaSyANYFFHg2K8s9FkyCHH4XxwdhjMrOkjjD0"

#define NURSE_EMAIL    "nurse@example.com"   // where missed-dose emails go
// ============================================================

// --- Hardware (same as pillbox firmware) ---
LiquidCrystal_I2C lcd(0x27, 16, 2);
const int ledPins[]  = {23, 19, 18, 17, 16, 4, 15};
const int buzzerPin  = 12;
const int upButton   = 27, selectButton = 26, downButton = 25, stopButton = 33;
const int itemCount  = 7;
const char* boxNames[] = {"Box A","Box B","Box C","Box D","Box E","Box F","Box G"};

// --- Firebase ---
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;
bool fbReady = false;

// --- NTP ---
#define NTP_SERVER  "pool.ntp.org"
#define GMT_OFFSET  28800   // UTC+8 (Philippines) — adjust if needed
#define DST_OFFSET  0

// --- Box data (loaded from LittleFS / Firebase) ---
struct Box {
  int  freq;        // alarms per day (0 = off)
  int  interval;    // minutes between doses
  int  dose;        // pills per dose
  long startSecs;   // seconds since midnight for first alarm
};
Box boxes[itemCount];
bool alarmDismissed[itemCount][12];

// --- Alarm state ---
int  activeAlarmBox  = -1;
unsigned long alarmStartTime = 0;
bool waitingConfirm  = false;   // true after alarm fires, waiting for STOP press

// --- Melody ---
int melody[]        = {294,330,392,330,494,494,440,294,330,392,330,440,440,392,370,330};
int noteDurations[] = {125,125,125,125,250,250,500,125,125,125,125,250,250,125,125,500};
const int melodyLen = sizeof(melody)/sizeof(melody[0]);
int  melodyIdx      = 0;
unsigned long nextNoteTime = 0;

// --- Button debounce ---
#define DEBOUNCE_MS  50
struct ButtonState {
  int pin; bool lastRaw, stable, consumed;
  unsigned long edgeTime, repeatTime;
};
ButtonState buttons[] = {
  {upButton,false,false,true,0,0},{selectButton,false,false,true,0,0},
  {downButton,false,false,true,0,0},{stopButton,false,false,true,0,0}
};
#define BTN_UP 0
#define BTN_SEL 1
#define BTN_DOWN 2
#define BTN_STOP 3

void updateButtons() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    bool raw = !digitalRead(buttons[i].pin);
    if (raw != buttons[i].lastRaw) { buttons[i].edgeTime = now; buttons[i].lastRaw = raw; }
    if ((now - buttons[i].edgeTime) >= DEBOUNCE_MS && raw != buttons[i].stable) {
      buttons[i].stable = raw; buttons[i].consumed = !raw;
    }
  }
}
bool buttonPressed(int idx) {
  ButtonState& b = buttons[idx];
  if (b.stable && !b.consumed) { b.consumed = true; return true; }
  return false;
}

// ============================================================
//  LITTLEFS  –  load/save alarms.json
// ============================================================
void loadFromLittleFS() {
  if (!LittleFS.exists("/alarms.json")) return;
  File f = LittleFS.open("/alarms.json", "r");
  if (!f) return;
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, f)) { f.close(); return; }
  f.close();
  for (int i = 0; i < itemCount; i++) {
    JsonObject b = doc[String(i)];
    if (b.isNull()) continue;
    boxes[i].freq      = b["freq"]      | 0;
    boxes[i].interval  = b["interval"]  | 0;
    boxes[i].dose      = b["dose"]      | 1;
    boxes[i].startSecs = b["startSecs"] | 25200;
  }
  Serial.println("Loaded alarms from LittleFS");
}

void saveToLittleFS() {
  StaticJsonDocument<1024> doc;
  for (int i = 0; i < itemCount; i++) {
    JsonObject b    = doc.createNestedObject(String(i));
    b["freq"]      = boxes[i].freq;
    b["interval"]  = boxes[i].interval;
    b["dose"]      = boxes[i].dose;
    b["startSecs"] = boxes[i].startSecs;
  }
  File f = LittleFS.open("/alarms.json", "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  Serial.println("Saved alarms to LittleFS");
}

// ============================================================
//  FIREBASE  –  sync boxes down, push logs up
// ============================================================
void syncBoxesFromFirebase() {
  if (!fbReady) return;
  if (!Firebase.RTDB.getJSON(&fbdo, "/boxes")) return;
  FirebaseJson& json = fbdo.jsonObject();
  FirebaseJsonData d;
  bool changed = false;
  for (int i = 0; i < itemCount; i++) {
    String base = "/" + String(i) + "/";
    int freq=0, interval=0, dose=1; long startSecs=25200;
    if (json.get(d, base+"freq"))      freq      = d.intValue;
    if (json.get(d, base+"interval"))  interval  = d.intValue;
    if (json.get(d, base+"dose"))      dose      = d.intValue;
    if (json.get(d, base+"startSecs")) startSecs = d.intValue;
    if (boxes[i].freq != freq || boxes[i].interval != interval ||
        boxes[i].dose != dose || boxes[i].startSecs != startSecs) {
      boxes[i] = {freq, interval, dose, startSecs};
      changed = true;
    }
  }
  if (changed) { saveToLittleFS(); Serial.println("Synced boxes from Firebase"); }
}

void logToFirebase(int boxIdx, bool taken) {
  if (!fbReady) return;
  time_t now; time(&now);
  FirebaseJson entry;
  entry.set("box",       boxNames[boxIdx]);
  entry.set("taken",     taken);
  entry.set("timestamp", (int)now);
  Firebase.RTDB.pushJSON(&fbdo, "/log", &entry);

  // Update lastTaken / lastMissed on the box record
  String path = "/boxes/" + String(boxIdx) + (taken ? "/lastTaken" : "/lastMissed");
  Firebase.RTDB.setInt(&fbdo, path.c_str(), (int)now);
}

void pushHeartbeat() {
  if (!fbReady) return;
  time_t now; time(&now);
  struct tm ti; localtime_r(&now, &ti);
  char timeBuf[9], dateBuf[12];
  strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &ti);
  strftime(dateBuf, sizeof(dateBuf), "%b %d %Y", &ti);

  FirebaseJson dev;
  dev.set("time",      timeBuf);
  dev.set("date",      dateBuf);
  dev.set("heartbeat", (int)now);

  int rssi = WiFi.RSSI();
  dev.set("rssi", rssi);
  Firebase.RTDB.setJSON(&fbdo, "/device", &dev);
}

// ── Check for time-sync command from dashboard ───────────────
void checkTimeCommand() {
  if (!fbReady) return;
  if (!Firebase.RTDB.getJSON(&fbdo, "/commands/setTime")) return;
  FirebaseJson& json = fbdo.jsonObject();
  FirebaseJsonData d;
  int h=0,mi=0,s=0,day=1,mon=1,yr=2025;
  long savedTs = 0;
  if (json.get(d,"/ts")) savedTs = d.intValue;
  time_t now; time(&now);
  if (abs((long)now - savedTs) > 60) return; // stale command, ignore

  if (json.get(d,"/hour"))   h   = d.intValue;
  if (json.get(d,"/minute")) mi  = d.intValue;
  if (json.get(d,"/second")) s   = d.intValue;
  if (json.get(d,"/day"))    day = d.intValue;
  if (json.get(d,"/month"))  mon = d.intValue;
  if (json.get(d,"/year"))   yr  = d.intValue;

  struct tm t = {};
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
  t.tm_mday = day; t.tm_mon = mon-1; t.tm_year = yr-1900;
  time_t newTime = mktime(&t);
  struct timeval tv = { newTime, 0 };
  settimeofday(&tv, nullptr);

  // Clear command so it doesn't re-fire
  Firebase.RTDB.deleteNode(&fbdo, "/commands/setTime");
  lcd.clear(); lcd.print("Time synced!"); delay(1200);
}

// ============================================================
//  MELODY
// ============================================================
void playMelody() {
  if (millis() > nextNoteTime) {
    noTone(buzzerPin);
    if (melody[melodyIdx] > 0) tone(buzzerPin, melody[melodyIdx]);
    nextNoteTime += noteDurations[melodyIdx];
    if (nextNoteTime < millis()) nextNoteTime = millis() + noteDurations[melodyIdx];
    melodyIdx = (melodyIdx + 1) % melodyLen;
  }
}

// ============================================================
//  LCD HELPERS
// ============================================================
void printDigits(int d) { if (d<10) lcd.print('0'); lcd.print(d); }

void drawClock() {
  time_t now; time(&now);
  struct tm ti; localtime_r(&now, &ti);
  lcd.clear();
  lcd.print("Date: ");
  printDigits(ti.tm_mon+1); lcd.print('/');
  printDigits(ti.tm_mday);  lcd.print('/');
  lcd.print(ti.tm_year+1900);
  lcd.setCursor(0,1);
  int h = ti.tm_hour; int dh = h%12; if(dh==0) dh=12;
  lcd.print("Time: ");
  printDigits(dh); lcd.print(":"); printDigits(ti.tm_min);
  lcd.print(h<12?" AM":" PM");
}

// ============================================================
//  ALARM CHECK
// ============================================================
void checkAlarms() {
  if (activeAlarmBox >= 0) return; // already ringing
  time_t now; time(&now);
  struct tm ti; localtime_r(&now, &ti);
  long secondsToday = ti.tm_hour*3600L + ti.tm_min*60L + ti.tm_sec;

  if (secondsToday < 5) {
    for (int i=0;i<itemCount;i++) for(int j=0;j<12;j++) alarmDismissed[i][j]=false;
  }

  for (int i=0; i<itemCount; i++) {
    if (boxes[i].freq <= 0) continue;
    for (int f=0; f<boxes[i].freq; f++) {
      long trigger = (boxes[i].startSecs + (long)f*boxes[i].interval*60L) % 86400L;
      if (secondsToday >= trigger && secondsToday < trigger+59 && !alarmDismissed[i][f]) {
        alarmDismissed[i][f] = true;
        activeAlarmBox = i;
        alarmStartTime = millis();
        waitingConfirm = true;
        nextNoteTime   = millis();
        melodyIdx      = 0;
        // Light up the correct LED
        for(int x=0;x<itemCount;x++) digitalWrite(ledPins[x], x==i ? HIGH : LOW);
        lcd.clear();
        lcd.print(boxNames[i]);
        lcd.setCursor(0,1);
        lcd.print("Pills: "); lcd.print(boxes[i].dose); lcd.print("x — STOP");
        return;
      }
    }
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.backlight();
  for (int i=0;i<itemCount;i++) { pinMode(ledPins[i], OUTPUT); digitalWrite(ledPins[i],LOW); }
  pinMode(buzzerPin, OUTPUT);
  pinMode(upButton,INPUT_PULLUP); pinMode(selectButton,INPUT_PULLUP);
  pinMode(downButton,INPUT_PULLUP); pinMode(stopButton,INPUT_PULLUP);

  // LittleFS
  if (!LittleFS.begin(true)) { lcd.print("FS Error"); delay(2000); }
  loadFromLittleFS();

  // WiFi
  lcd.clear(); lcd.print("Connecting WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); tries++;
    lcd.setCursor(tries % 16, 1); lcd.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    lcd.clear(); lcd.print("WiFi failed"); lcd.setCursor(0,1); lcd.print("Running offline");
    delay(2000);
  } else {
    lcd.clear(); lcd.print("WiFi OK");
    lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString());
    delay(1500);

    // NTP time sync
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
    lcd.clear(); lcd.print("Syncing time...");
    delay(2000);

    // Firebase
    config.host           = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_AUTH;
    config.token_status_callback = tokenStatusCallback;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    fbReady = true;

    syncBoxesFromFirebase();
    pushHeartbeat();
  }

  drawClock();
}

// ============================================================
//  LOOP
// ============================================================
unsigned long lastHeartbeat   = 0;
unsigned long lastFbSync      = 0;
unsigned long lastClockDraw   = 0;
int lastDrawnMin = -1;

void loop() {
  updateButtons();

  // ── Alarm active ─────────────────────────────────────────
  if (activeAlarmBox >= 0 && waitingConfirm) {
    playMelody();

    bool timeout = (millis() - alarmStartTime > 300000UL); // 5 min

    if (buttonPressed(BTN_STOP) || timeout) {
      bool confirmed = !timeout; // if timed out = missed
      noTone(buzzerPin);
      digitalWrite(ledPins[activeAlarmBox], LOW);

      // Log to Firebase
      logToFirebase(activeAlarmBox, confirmed);

      // Show result on LCD
      lcd.clear();
      if (confirmed) {
        lcd.print("Confirmed!");
        tone(buzzerPin, 1046, 100); delay(150); tone(buzzerPin, 1318, 200); delay(250); noTone(buzzerPin);
      } else {
        lcd.print("MISSED - logged");
      }
      lcd.setCursor(0,1); lcd.print(boxNames[activeAlarmBox]);
      delay(2000);

      waitingConfirm = false;
      activeAlarmBox = -1;
      drawClock();
    }
    return;
  }

  // ── Periodic Firebase sync (every 30 s) ──────────────────
  if (millis() - lastFbSync > 30000UL) {
    lastFbSync = millis();
    syncBoxesFromFirebase();
    checkTimeCommand();
  }

  // ── Heartbeat to Firebase (every 60 s) ───────────────────
  if (millis() - lastHeartbeat > 60000UL) {
    lastHeartbeat = millis();
    pushHeartbeat();
  }

  // ── Check alarms ─────────────────────────────────────────
  checkAlarms();

  // ── Redraw clock every minute ─────────────────────────────
  time_t now; time(&now);
  struct tm ti; localtime_r(&now, &ti);
  if (ti.tm_min != lastDrawnMin) {
    lastDrawnMin = ti.tm_min;
    drawClock();
  }
}
