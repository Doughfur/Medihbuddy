#include <Wire.h>
#include <RTClib.h>
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
#define WIFI_SSID     "SKYFiber_MESH_CC74"
#define WIFI_PASSWORD "531099270"
#define FIREBASE_HOST "https://medibuddy-27ca8-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "AIzaSyANYFFHg2K8s9FkyCHH4XxwdhjMrOkjjD0"
#define GMT_OFFSET    28800
#define DST_OFFSET    0
#define NTP_SERVER    "pool.ntp.org"
#define T_7AM         25200
// ============================================================

// ── Hardware ─────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;
#define RTC_SDA 13
#define RTC_SCL 14
const int ledPins[]  = {23, 19, 18, 17, 16, 4, 15};
const int buzzerPin  = 12;
const int upButton   = 27, selectButton = 26, downButton = 25, stopButton = 33;
const int itemCount  = 7;
const char* boxNames[] = {"Box A","Box B","Box C","Box D","Box E","Box F","Box G"};

// ── Firebase ─────────────────────────────────────────────────
FirebaseData   fbdo;
FirebaseAuth   fbAuth;
FirebaseConfig fbConfig;
bool fbReady = false;
bool wifiOK  = false;
bool rtcOK   = false;

// ── Presets ───────────────────────────────────────────────────
struct Preset { const char* name; int frequency; int interval; long startTime; bool isLocked; };
Preset presets[] = {
  {"Morn/Night", 2, 900,  T_7AM, true},
  {"B-L-D",      3, 360,  T_7AM, true},
  {"1x Daily",   1, 0,    T_7AM, false},
  {"2x Daily",   2, 720,  T_7AM, false},
  {"3x Daily",   3, 480,  T_7AM, false},
  {"4x Daily",   4, 360,  T_7AM, false}
};
const int presetCount = sizeof(presets)/sizeof(presets[0]);
const char* actionItems[] = {"Presets","Custom","Turn Off"};

// ── Box data ─────────────────────────────────────────────────
long boxStartTimes[itemCount];
int  boxFrequencies[itemCount];
int  boxIntervals[itemCount];
int  boxPillDose[itemCount];
bool alarmDismissed[itemCount][12] = {};
unsigned long localSaveTime = 0;

// ── State machine ─────────────────────────────────────────────
enum Mode {
  CLOCK_HOME, BROWSE, BOX_ACTION, SELECT_PRESET,
  EDIT_START, EDIT_FREQ, EDIT_INTERVAL, EDIT_DOSE,
  ALARM_ACTIVE, DEBUG_MENU, VIEW_SUMMARY,
  SET_CLOCK_HOUR, SET_CLOCK_MINUTE, SET_CLOCK_AMPM,
  SET_CLOCK_MONTH, SET_CLOCK_DAY, SET_CLOCK_YEAR
};
Mode currentMode = CLOCK_HOME;
int cursorLine=0, actionCursor=0, presetCursor=0, debugCursor=0, summaryPage=0;
int  tsHour=12, tsMinute=0, tsMonth=1, tsDay=1, tsYear=2025;
bool tsAM=true;
const int daysInMonth[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};

// ── Alarm state ───────────────────────────────────────────────
int  activeAlarmBox=-1;
unsigned long alarmStartTime=0;
bool waitingConfirm=false;
bool debugQueueActive=false;
int melody[]        = {294,330,392,330,494,494,440,294,330,392,330,440,440,392,370,330};
int noteDurations[] = {125,125,125,125,250,250,500,125,125,125,125,250,250,125,125,500};
const int melodyLen = sizeof(melody)/sizeof(melody[0]);
int  melodyIdx=0;
unsigned long nextNoteTime=0;

// ── Alarm queue ───────────────────────────────────────────────
struct QueuedAlarm { int boxIdx; int doseIdx; };
QueuedAlarm alarmQueue[itemCount*12];
int alarmQueueHead=0, alarmQueueTail=0;
int lastCheckedMinute=-1, lastCheckedDay=-1;

// ── Button debounce ───────────────────────────────────────────
#define DEBOUNCE_MS  50
#define REPEAT_DELAY 400
#define REPEAT_RATE  150
struct ButtonState { int pin; bool lastRaw,stable,consumed; unsigned long edgeTime,repeatTime; };
ButtonState buttons[] = {
  {upButton,false,false,true,0,0},{selectButton,false,false,true,0,0},
  {downButton,false,false,true,0,0},{stopButton,false,false,true,0,0}
};
#define BTN_UP 0
#define BTN_SEL 1
#define BTN_DOWN 2
#define BTN_STOP 3

// ============================================================
//  BUTTON FUNCTIONS
// ============================================================
void updateButtons() {
  unsigned long now=millis();
  for (int i=0;i<4;i++) {
    bool raw=!digitalRead(buttons[i].pin);
    if (raw!=buttons[i].lastRaw) { buttons[i].edgeTime=now; buttons[i].lastRaw=raw; }
    if ((now-buttons[i].edgeTime)>=DEBOUNCE_MS && raw!=buttons[i].stable) {
      buttons[i].stable=raw; buttons[i].consumed=!raw;
      if (raw) buttons[i].repeatTime=now+REPEAT_DELAY;
    }
  }
}
bool buttonFired(int idx) {
  unsigned long now=millis(); ButtonState& b=buttons[idx];
  if (!b.stable) return false;
  if (!b.consumed) { b.consumed=true; return true; }
  if (now>=b.repeatTime) { b.repeatTime=now+REPEAT_RATE; return true; }
  return false;
}
bool buttonPressed(int idx) {
  ButtonState& b=buttons[idx];
  if (b.stable&&!b.consumed) { b.consumed=true; return true; }
  return false;
}
bool buttonHeld(int idx) { return buttons[idx].stable; }

// ============================================================
//  TIME HELPERS
// ============================================================
void printDigits(int d) { if(d<10) lcd.print('0'); lcd.print(d); }

void print12h(int h, int m, bool showSpace=true) {
  int dh=h%12; if(dh==0) dh=12;
  printDigits(dh); lcd.print(":"); printDigits(m);
  if(showSpace) lcd.print(" ");
  lcd.print(h<12?"AM":"PM");
}

void printFormattedTime(long s) { print12h((int)(s/3600),(int)((s%3600)/60)); }

struct tm getCurrentTime() {
  struct tm ti={};
  if (wifiOK) { time_t now; time(&now); localtime_r(&now,&ti); }
  else if (rtcOK) {
    DateTime dt=rtc.now();
    ti.tm_hour=dt.hour(); ti.tm_min=dt.minute(); ti.tm_sec=dt.second();
    ti.tm_mday=dt.day(); ti.tm_mon=dt.month()-1; ti.tm_year=dt.year()-1900;
  }
  return ti;
}

int clampDay(int d,int m,int y) {
  int maxD=daysInMonth[m];
  if(m==2&&((y%4==0&&y%100!=0)||y%400==0)) maxD=29;
  return constrain(d,1,maxD);
}

// ============================================================
//  LITTLEFS
// ============================================================
void saveToLittleFS(bool showMessage=false) {
  if(showMessage) { lcd.clear(); lcd.print("Saving..."); }
  StaticJsonDocument<1024> doc;
  for(int i=0;i<itemCount;i++) {
    JsonObject b=doc.createNestedObject(String(i));
    b["startSecs"]=boxStartTimes[i]; b["freq"]=boxFrequencies[i];
    b["interval"]=boxIntervals[i];   b["dose"]=boxPillDose[i];
  }
  time_t now; time(&now);
  doc["savedAt"]=(now>1000000000L)?(long)now:(long)(millis()/1000);
  localSaveTime=millis();
  File f=LittleFS.open("/alarms.json","w");
  if(f) { serializeJson(doc,f); f.close(); }
  Serial.println("Saved to LittleFS");
}

void loadFromLittleFS() {
  if(!LittleFS.exists("/alarms.json")) {
    for(int i=0;i<itemCount;i++) { boxStartTimes[i]=T_7AM; boxFrequencies[i]=0; boxIntervals[i]=0; boxPillDose[i]=1; }
    return;
  }
  File f=LittleFS.open("/alarms.json","r");
  if(!f) return;
  StaticJsonDocument<1024> doc;
  if(deserializeJson(doc,f)) { f.close(); return; }
  f.close();
  for(int i=0;i<itemCount;i++) {
    JsonObject b=doc[String(i)];
    if(b.isNull()) continue;
    boxStartTimes[i] =b["startSecs"]|(long)T_7AM;
    boxFrequencies[i]=b["freq"]|0;
    boxIntervals[i]  =b["interval"]|0;
    boxPillDose[i]   =b["dose"]|1;
  }
}

void resetToDefaults(int b) {
  boxStartTimes[b]=T_7AM; boxFrequencies[b]=0; boxIntervals[b]=0; boxPillDose[b]=1;
}

// ============================================================
//  FIREBASE FUNCTIONS
// ============================================================
void pushBoxesToFirebase() {
  if(!fbReady) return;
  FirebaseJson json;
  for(int i=0;i<itemCount;i++) {
    String base="/"+String(i)+"/";
    json.set(base+"startSecs",(int)boxStartTimes[i]);
    json.set(base+"freq",boxFrequencies[i]);
    json.set(base+"interval",boxIntervals[i]);
    json.set(base+"dose",boxPillDose[i]);
    json.set(base+"name",boxNames[i]);
  }
  Firebase.RTDB.setJSON(&fbdo,"/boxes",&json);
}

void syncBoxesFromFirebase() {
  if(!fbReady) return;
  Serial.println("Boot sync from Firebase...");
  if(!Firebase.RTDB.getJSON(&fbdo,"boxes")) {
    Serial.println("Boot sync: no data, pushing local");
    pushBoxesToFirebase(); return;
  }
  String payload=fbdo.payload();
  if(payload.length()<10) { pushBoxesToFirebase(); return; }
  StaticJsonDocument<2048> doc;
  if(deserializeJson(doc,payload)) { Serial.println("Boot sync: JSON error"); return; }
  // Temporarily zero localSaveTime so applyBoxDataFromDoc doesn't skip on boot
  unsigned long savedLocal=localSaveTime; localSaveTime=0;
  applyBoxDataFromDoc(doc);
  localSaveTime=savedLocal;
}

// Uses ArduinoJson directly — bypasses FirebaseJson which returns empty objects
void applyBoxDataFromDoc(JsonDocument& doc) {
  // Don't overwrite local data if we saved via physical buttons within last 30s
  if(millis()-localSaveTime<30000UL) {
    Serial.println("Skipping Firebase apply — recent local save");
    return;
  }
  bool changed = false;

  // Firebase returns boxes as a JSON array [ {...}, {...} ]
  // because the keys are integers 0-6
  JsonArray arr = doc.as<JsonArray>();
  bool isArray = !arr.isNull();

  for(int i=0; i<itemCount; i++) {
    JsonObject box;
    if(isArray) {
      // Array format: [{"freq":2,...}, {"freq":1,...}]
      if(i >= (int)arr.size()) continue;
      box = arr[i].as<JsonObject>();
    } else {
      // Object format: {"0":{"freq":2,...}, "1":{...}}
      String key = String(i);
      if(!doc.containsKey(key)) continue;
      box = doc[key].as<JsonObject>();
    }
    if(box.isNull()) continue;

    long st   = box["startSecs"] | (long)T_7AM;
    int  freq = box["freq"]      | 0;
    int  intv = box["interval"]  | 0;
    int  dose = box["dose"]      | 1;

    Serial.print("  Box "); Serial.print(i);
    Serial.print(": FB freq="); Serial.print(freq);
    Serial.print(" start="); Serial.print(st);
    Serial.print(" | Local freq="); Serial.print(boxFrequencies[i]);
    Serial.print(" start="); Serial.println(boxStartTimes[i]);

    if(boxStartTimes[i]!=st||boxFrequencies[i]!=freq||
       boxIntervals[i]!=intv||boxPillDose[i]!=dose) {
      boxStartTimes[i]=st; boxFrequencies[i]=freq;
      boxIntervals[i]=intv; boxPillDose[i]=dose;
      changed=true;
      Serial.print("  >> Updated "); Serial.println(boxNames[i]);
    }
  }
  if(changed) { saveToLittleFS(); Serial.println("Boxes saved to LittleFS"); }
  else        Serial.println("No box changes");
}


void applyTimeCommand() {
  // Called from stream callback — reads and applies setTime command
  int h=0,mi=0,s=0,day=1,mon=1,yr=2025;
  if(Firebase.RTDB.getInt(&fbdo,"commands/setTime/hour"))   h  =fbdo.intData();
  else return; // no command
  if(Firebase.RTDB.getInt(&fbdo,"commands/setTime/minute")) mi =fbdo.intData();
  if(Firebase.RTDB.getInt(&fbdo,"commands/setTime/second")) s  =fbdo.intData();
  if(Firebase.RTDB.getInt(&fbdo,"commands/setTime/day"))    day=fbdo.intData();
  if(Firebase.RTDB.getInt(&fbdo,"commands/setTime/month"))  mon=fbdo.intData();
  if(Firebase.RTDB.getInt(&fbdo,"commands/setTime/year"))   yr =fbdo.intData();
  struct tm t={}; t.tm_hour=h; t.tm_min=mi; t.tm_sec=s;
  t.tm_mday=day; t.tm_mon=mon-1; t.tm_year=yr-1900;
  time_t newTime=mktime(&t);
  struct timeval tv={newTime,0}; settimeofday(&tv,nullptr);
  if(rtcOK) rtc.adjust(DateTime(yr,mon,day,h,mi,s));
  Firebase.RTDB.deleteNode(&fbdo,"commands/setTime");
  Serial.print("Time set to "); Serial.print(h); Serial.print(":"); Serial.println(mi);
  lcd.clear(); lcd.print("Time synced!"); lcd.setCursor(0,1); print12h(h,mi);
  delay(1500); drawScreen();
}

void logToFirebase(int boxIdx,bool taken) {
  if(!fbReady) return;
  time_t now; time(&now);
  FirebaseJson entry;
  entry.set("box",boxNames[boxIdx]); entry.set("taken",taken); entry.set("timestamp",(int)now);
  Firebase.RTDB.pushJSON(&fbdo,"/log",&entry);
  String path="/boxes/"+String(boxIdx)+(taken?"/lastTaken":"/lastMissed");
  Firebase.RTDB.setInt(&fbdo,path.c_str(),(int)now);
}

void pushHeartbeat() {
  if(!fbReady) return;
  struct tm ti=getCurrentTime();
  char timeBuf[10],dateBuf[12];
  strftime(timeBuf,sizeof(timeBuf),"%I:%M %p",&ti);
  strftime(dateBuf,sizeof(dateBuf),"%b %d %Y",&ti);
  FirebaseJson dev; time_t now; time(&now);
  dev.set("time",timeBuf); dev.set("date",dateBuf);
  dev.set("heartbeat",(int)now); dev.set("rssi",WiFi.RSSI());
  Firebase.RTDB.setJSON(&fbdo,"/device",&dev);
}

// Stream removed — using fast polling instead (more reliable with Firebase ESP Client)

// ============================================================
//  MELODY
// ============================================================
void playMegalovania() {
  if(millis()>nextNoteTime) {
    noTone(buzzerPin);
    if(melody[melodyIdx]>0) tone(buzzerPin,melody[melodyIdx]);
    nextNoteTime+=noteDurations[melodyIdx];
    if(nextNoteTime<millis()) nextNoteTime=millis()+noteDurations[melodyIdx];
    melodyIdx=(melodyIdx+1)%melodyLen;
  }
}

// ============================================================
//  LED HELPERS
// ============================================================
void updateBoxLEDs() {
  for(int i=0;i<itemCount;i++) {
    bool on=(currentMode==ALARM_ACTIVE&&i==activeAlarmBox)
          ||(i==cursorLine&&(currentMode==BROWSE||currentMode==BOX_ACTION||
             currentMode==SELECT_PRESET||currentMode==EDIT_START||
             currentMode==EDIT_FREQ||currentMode==EDIT_INTERVAL||currentMode==EDIT_DOSE));
    digitalWrite(ledPins[i],on?HIGH:LOW);
  }
}

void runLedTest() {
  lcd.clear(); lcd.print("Testing LEDs...");
  for(int i=0;i<itemCount;i++) { digitalWrite(ledPins[i],HIGH); delay(200); digitalWrite(ledPins[i],LOW); }
}

// ============================================================
//  DISPLAY
// ============================================================
void printSetHint(bool isLast=false) {
  lcd.setCursor(0,1);
  lcd.print(isLast?"SEL=Save STOP=Bk":"SEL=Next STOP=Bk");
}

void drawScreen() {
  lcd.clear();
  struct tm ti=getCurrentTime();
  if(currentMode==ALARM_ACTIVE) {
    lcd.print(boxNames[activeAlarmBox]);
    lcd.setCursor(0,1); lcd.print("Pills:"); lcd.print(boxPillDose[activeAlarmBox]); lcd.print("x STOP");
    return;
  }
  switch(currentMode) {
    case CLOCK_HOME:
      lcd.print("Date: "); printDigits(ti.tm_mon+1); lcd.print('/');
      printDigits(ti.tm_mday); lcd.print('/'); lcd.print(ti.tm_year+1900);
      lcd.setCursor(0,1); lcd.print("Time: "); print12h(ti.tm_hour,ti.tm_min); break;
    case BROWSE:
      lcd.print("Select Box:"); lcd.setCursor(0,1); lcd.print("> "); lcd.print(boxNames[cursorLine]); break;
    case BOX_ACTION:
      lcd.print("Setup "); lcd.print(boxNames[cursorLine]);
      lcd.setCursor(0,1); lcd.print("> "); lcd.print(actionItems[actionCursor]); break;
    case SELECT_PRESET:
      lcd.print("Schedule:"); lcd.setCursor(0,1); lcd.print("> "); lcd.print(presets[presetCursor].name); break;
    case EDIT_FREQ:
      lcd.print("Alarms/Day:"); lcd.setCursor(0,1); lcd.print("> "); lcd.print(boxFrequencies[cursorLine]); break;
    case EDIT_INTERVAL:
      lcd.print("Interval (Hrs):"); lcd.setCursor(0,1); lcd.print("> "); lcd.print(boxIntervals[cursorLine]/60.0,1); break;
    case EDIT_START:
      lcd.print("First Alarm:"); lcd.setCursor(0,1); lcd.print("> "); printFormattedTime(boxStartTimes[cursorLine]); break;
    case EDIT_DOSE:
      lcd.print("Pills/Dose:"); lcd.setCursor(0,1); lcd.print("> "); lcd.print(boxPillDose[cursorLine]); break;
    case DEBUG_MENU: {
      lcd.print("DEBUG MODE"); lcd.setCursor(0,1);
      const char* dOpts[]={"LEDs","Buzzer","Queue","Summary","Set Clock","Restart"};
      lcd.print("> "); lcd.print(dOpts[debugCursor]); break;
    }
    case VIEW_SUMMARY:
      lcd.print(boxNames[summaryPage]); lcd.print(":");
      lcd.setCursor(0,1);
      if(boxFrequencies[summaryPage]==0) lcd.print("Status: OFF");
      else { lcd.print(boxFrequencies[summaryPage]); lcd.print("x @ "); printFormattedTime(boxStartTimes[summaryPage]); }
      break;
    case SET_CLOCK_HOUR:   lcd.print("Set Hour:  ["); printDigits(tsHour);   lcd.print("]"); printSetHint(); break;
    case SET_CLOCK_MINUTE: lcd.print("Set Min:   ["); printDigits(tsMinute); lcd.print("]"); printSetHint(); break;
    case SET_CLOCK_AMPM:   lcd.print("Set AM/PM: ["); lcd.print(tsAM?"AM":"PM"); lcd.print("]"); printSetHint(); break;
    case SET_CLOCK_MONTH:  lcd.print("Set Month: ["); printDigits(tsMonth);  lcd.print("]"); printSetHint(); break;
    case SET_CLOCK_DAY:    lcd.print("Set Day:   ["); printDigits(tsDay);    lcd.print("]"); printSetHint(); break;
    case SET_CLOCK_YEAR:   lcd.print("Set Year:  ["); lcd.print(tsYear);     lcd.print("]"); printSetHint(true); break;
    default: break;
  }
}

// ============================================================
//  TIME SET
// ============================================================
void enterSetClock() {
  struct tm ti=getCurrentTime();
  tsHour=ti.tm_hour%12; if(tsHour==0) tsHour=12;
  tsMinute=ti.tm_min; tsAM=(ti.tm_hour<12);
  tsMonth=ti.tm_mon+1; tsDay=ti.tm_mday; tsYear=ti.tm_year+1900;
  currentMode=SET_CLOCK_HOUR;
}

void commitSetClock() {
  int h24=tsHour%12; if(!tsAM) h24+=12;
  tsDay=clampDay(tsDay,tsMonth,tsYear);
  struct tm t={}; t.tm_hour=h24; t.tm_min=tsMinute; t.tm_sec=0;
  t.tm_mday=tsDay; t.tm_mon=tsMonth-1; t.tm_year=tsYear-1900;
  time_t newTime=mktime(&t); struct timeval tv={newTime,0}; settimeofday(&tv,nullptr);
  if(rtcOK) rtc.adjust(DateTime(tsYear,tsMonth,tsDay,h24,tsMinute,0));
}

// ============================================================
//  ALARM QUEUE + CHECK
// ============================================================
bool queueEmpty() { return alarmQueueHead==alarmQueueTail; }

void enqueueAlarm(int boxIdx,int doseIdx) {
  alarmQueue[alarmQueueTail]={boxIdx,doseIdx};
  alarmQueueTail=(alarmQueueTail+1)%(itemCount*12);
  Serial.print("Queued: "); Serial.print(boxNames[boxIdx]); Serial.print(" dose "); Serial.println(doseIdx);
}

void fireNextInQueue() {
  if(queueEmpty()) return;
  QueuedAlarm qa=alarmQueue[alarmQueueHead];
  alarmQueueHead=(alarmQueueHead+1)%(itemCount*12);
  activeAlarmBox=qa.boxIdx; alarmStartTime=millis(); waitingConfirm=true;
  nextNoteTime=millis(); melodyIdx=0;
  Serial.print("ALARM FIRING: "); Serial.println(boxNames[qa.boxIdx]);
  updateBoxLEDs(); drawScreen();
}

void checkAlarms() {
  if(currentMode==DEBUG_MENU||currentMode==ALARM_ACTIVE) return;
  if(!queueEmpty()) { fireNextInQueue(); currentMode=ALARM_ACTIVE; return; }
  struct tm ti=getCurrentTime();
  int nowMinute=ti.tm_hour*60+ti.tm_min;
  int nowDay=ti.tm_mday;
  if(nowDay!=lastCheckedDay&&ti.tm_hour==0&&ti.tm_min<2) {
    for(int i=0;i<itemCount;i++) for(int j=0;j<12;j++) alarmDismissed[i][j]=false;
    lastCheckedDay=nowDay; alarmQueueHead=alarmQueueTail=0;
    Serial.println("Midnight reset");
  }
  if(nowMinute==lastCheckedMinute) return;
  lastCheckedMinute=nowMinute;
  Serial.print("Alarm scan at "); Serial.print(ti.tm_hour); Serial.print(":"); Serial.println(ti.tm_min);
  bool anyQueued=false;
  for(int i=0;i<itemCount;i++) {
    if(boxFrequencies[i]<=0) continue;
    for(int f=0;f<boxFrequencies[i];f++) {
      long trigger=(boxStartTimes[i]+(long)f*boxIntervals[i]*60L)%86400L;
      int  trigMin=(int)(trigger/60);
      Serial.print("  "); Serial.print(boxNames[i]); Serial.print(" dose "); Serial.print(f);
      Serial.print(" @min "); Serial.print(trigMin); Serial.print(" dismissed="); Serial.println(alarmDismissed[i][f]);
      if(nowMinute==trigMin&&!alarmDismissed[i][f]) {
        alarmDismissed[i][f]=true; enqueueAlarm(i,f); anyQueued=true;
      }
    }
  }
  if(anyQueued) { fireNextInQueue(); currentMode=ALARM_ACTIVE; }
}

// ============================================================
//  INPUT HANDLER
// ============================================================
void handleInputs() {
  updateButtons();
  static unsigned long comboStart=0;
  if(buttonHeld(BTN_UP)&&buttonHeld(BTN_DOWN)) {
    if(comboStart==0) comboStart=millis();
    if(millis()-comboStart>2000&&currentMode!=DEBUG_MENU) {
      currentMode=DEBUG_MENU; debugCursor=0; tone(buzzerPin,1500,150); comboStart=0; drawScreen(); return;
    }
  } else comboStart=0;

  if(currentMode==ALARM_ACTIVE&&waitingConfirm) {
    playMegalovania(); digitalWrite(ledPins[activeAlarmBox],HIGH);
    bool timeout=(millis()-alarmStartTime>300000UL);
    if(buttonPressed(BTN_STOP)||timeout) {
      bool confirmed=!timeout;
      noTone(buzzerPin); digitalWrite(ledPins[activeAlarmBox],LOW);
      logToFirebase(activeAlarmBox,confirmed);
      lcd.clear();
      if(confirmed) { lcd.print("Confirmed!"); tone(buzzerPin,1046,100); delay(150); tone(buzzerPin,1318,200); delay(250); noTone(buzzerPin); }
      else lcd.print("MISSED - logged");
      lcd.setCursor(0,1); lcd.print(boxNames[activeAlarmBox]); delay(2000);
      waitingConfirm=false; activeAlarmBox=-1; debugQueueActive=false;
      if(!queueEmpty()) { fireNextInQueue(); currentMode=ALARM_ACTIVE; }
      else currentMode=CLOCK_HOME;
      updateBoxLEDs(); drawScreen();
    }
    return;
  }

  bool up=buttonFired(BTN_UP), down=buttonFired(BTN_DOWN);
  bool sel=buttonPressed(BTN_SEL), stop=buttonPressed(BTN_STOP);
  if(!up&&!down&&!sel&&!stop) return;
  bool changed=true;

  switch(currentMode) {
    case DEBUG_MENU:
      if(up)   debugCursor=(debugCursor+1)%6;
      if(down) debugCursor=(debugCursor-1+6)%6;
      if(sel) {
        if(debugCursor==0) runLedTest();
        if(debugCursor==1) { unsigned long s=millis(); while(millis()-s<3000) playMegalovania(); noTone(buzzerPin); }
        if(debugCursor==2) { debugQueueActive=true; activeAlarmBox=0; currentMode=ALARM_ACTIVE; waitingConfirm=true; alarmStartTime=millis(); nextNoteTime=millis(); melodyIdx=0; }
        if(debugCursor==3) { currentMode=VIEW_SUMMARY; summaryPage=0; }
        if(debugCursor==4) enterSetClock();
        if(debugCursor==5) { lcd.clear(); lcd.print("Restarting..."); delay(1000); ESP.restart(); }
      }
      if(stop) currentMode=CLOCK_HOME;
      break;
    case CLOCK_HOME: if(sel) currentMode=BROWSE; else changed=false; break;
    case BROWSE:
      if(up)   cursorLine=(cursorLine+1)%itemCount;
      if(down) cursorLine=(cursorLine-1+itemCount)%itemCount;
      if(sel)  { actionCursor=0; currentMode=BOX_ACTION; }
      if(stop) currentMode=CLOCK_HOME;
      break;
    case BOX_ACTION:
      if(up)   actionCursor=(actionCursor+1)%3;
      if(down) actionCursor=(actionCursor-1+3)%3;
      if(sel) {
        if(actionCursor==0) { presetCursor=0; currentMode=SELECT_PRESET; }
        else if(actionCursor==1) { resetToDefaults(cursorLine); currentMode=EDIT_FREQ; }
        else { boxFrequencies[cursorLine]=0; saveToLittleFS(true); pushBoxesToFirebase(); currentMode=CLOCK_HOME; }
      }
      if(stop) currentMode=BROWSE;
      break;
    case SELECT_PRESET:
      if(up)   presetCursor=(presetCursor+1)%presetCount;
      if(down) presetCursor=(presetCursor-1+presetCount)%presetCount;
      if(sel) {
        boxStartTimes[cursorLine]=presets[presetCursor].startTime;
        boxFrequencies[cursorLine]=presets[presetCursor].frequency;
        boxIntervals[cursorLine]=presets[presetCursor].interval;
        boxPillDose[cursorLine]=1;
        currentMode=presets[presetCursor].isLocked?EDIT_DOSE:EDIT_START;
      }
      if(stop) currentMode=BOX_ACTION;
      break;
    case EDIT_FREQ:
      if(up)   { if(boxFrequencies[cursorLine]<12) boxFrequencies[cursorLine]++; }
      if(down) { if(boxFrequencies[cursorLine]>1)  boxFrequencies[cursorLine]--; }
      if(sel)  currentMode=(boxFrequencies[cursorLine]==1)?EDIT_START:EDIT_INTERVAL;
      if(stop) currentMode=BOX_ACTION;
      break;
    case EDIT_INTERVAL:
      if(up)   { if(boxIntervals[cursorLine]<720) boxIntervals[cursorLine]+=30; }
      if(down) { if(boxIntervals[cursorLine]>30)  boxIntervals[cursorLine]-=30; }
      if(sel)  currentMode=EDIT_START;
      if(stop) currentMode=EDIT_FREQ;
      break;
    case EDIT_START:
      if(up)   boxStartTimes[cursorLine]=(boxStartTimes[cursorLine]+1800)%86400;
      if(down) boxStartTimes[cursorLine]=(boxStartTimes[cursorLine]-1800+86400)%86400;
      if(sel)  currentMode=EDIT_DOSE;
      if(stop) currentMode=(actionCursor==0)?SELECT_PRESET:(boxFrequencies[cursorLine]==1)?EDIT_FREQ:EDIT_INTERVAL;
      break;
    case EDIT_DOSE:
      if(up)   { if(boxPillDose[cursorLine]<20) boxPillDose[cursorLine]++; }
      if(down) { if(boxPillDose[cursorLine]>1)  boxPillDose[cursorLine]--; }
      if(sel)  { saveToLittleFS(true); pushBoxesToFirebase(); currentMode=CLOCK_HOME; }
      if(stop) currentMode=EDIT_START;
      break;
    case VIEW_SUMMARY:
      if(up||sel) summaryPage=(summaryPage+1)%itemCount;
      if(down)    summaryPage=(summaryPage-1+itemCount)%itemCount;
      if(stop)    currentMode=DEBUG_MENU;
      break;
    case SET_CLOCK_HOUR:
      if(up)   tsHour=(tsHour%12)+1;
      if(down) tsHour=((tsHour-2+12)%12)+1;
      if(sel)  currentMode=SET_CLOCK_MINUTE;
      if(stop) currentMode=DEBUG_MENU;
      break;
    case SET_CLOCK_MINUTE:
      if(up)   tsMinute=(tsMinute+1)%60;
      if(down) tsMinute=(tsMinute-1+60)%60;
      if(sel)  currentMode=SET_CLOCK_AMPM;
      if(stop) currentMode=SET_CLOCK_HOUR;
      break;
    case SET_CLOCK_AMPM:
      if(up||down) tsAM=!tsAM;
      if(sel)  currentMode=SET_CLOCK_MONTH;
      if(stop) currentMode=SET_CLOCK_MINUTE;
      break;
    case SET_CLOCK_MONTH:
      if(up)   tsMonth=(tsMonth%12)+1;
      if(down) tsMonth=((tsMonth-2+12)%12)+1;
      tsDay=clampDay(tsDay,tsMonth,tsYear);
      if(sel)  currentMode=SET_CLOCK_DAY;
      if(stop) currentMode=SET_CLOCK_AMPM;
      break;
    case SET_CLOCK_DAY: {
      int maxD=daysInMonth[tsMonth];
      if(tsMonth==2&&((tsYear%4==0&&tsYear%100!=0)||tsYear%400==0)) maxD=29;
      if(up)   tsDay=(tsDay%maxD)+1;
      if(down) tsDay=((tsDay-2+maxD)%maxD)+1;
      if(sel)  currentMode=SET_CLOCK_YEAR;
      if(stop) currentMode=SET_CLOCK_MONTH;
      break;
    }
    case SET_CLOCK_YEAR:
      if(up)              tsYear++;
      if(down&&tsYear>2020) tsYear--;
      if(sel) { commitSetClock(); lcd.clear(); lcd.print("Time & Date"); lcd.setCursor(0,1); lcd.print("Saved!"); delay(1200); currentMode=CLOCK_HOME; }
      if(stop) currentMode=SET_CLOCK_DAY;
      break;
    default: changed=false; break;
  }
  if(changed) { updateBoxLEDs(); drawScreen(); }
}

// ============================================================
//  FIREBASE POLLING  (replaces streaming — more reliable)
// ============================================================
void pollFirebase() {
  // ── Check for box changes from dashboard ─────────────────
  if(Firebase.RTDB.getJSON(&fbdo,"boxes")) {
    String payload = fbdo.payload();
    if(payload.length() > 10) {
      StaticJsonDocument<2048> doc;
      DeserializationError err = deserializeJson(doc, payload);
      if(err) {
        Serial.print("JSON parse error: "); Serial.println(err.c_str());
      } else {
        Serial.println("JSON parsed OK, applying...");
        applyBoxDataFromDoc(doc);
      }
    } else {
      Serial.println("Payload too short, skipping");
    }
  } else {
    Serial.print("Poll failed: "); Serial.println(fbdo.errorReason());
  }

  // ── Check for time sync command ───────────────────────────
  if(Firebase.RTDB.getInt(&fbdo,"commands/setTime/hour")) {
    applyTimeCommand();
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  lcd.init(); lcd.backlight();
  for(int i=0;i<itemCount;i++) { pinMode(ledPins[i],OUTPUT); digitalWrite(ledPins[i],LOW); }
  pinMode(buzzerPin,OUTPUT);
  pinMode(upButton,INPUT_PULLUP); pinMode(selectButton,INPUT_PULLUP);
  pinMode(downButton,INPUT_PULLUP); pinMode(stopButton,INPUT_PULLUP);

  Wire1.begin(RTC_SDA,RTC_SCL);
  if(rtc.begin(&Wire1)) { rtcOK=true; if(rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__),F(__TIME__))); }
  else { lcd.print("RTC not found"); delay(1500); }

  if(!LittleFS.begin(true)) { lcd.clear(); lcd.print("FS Error!"); delay(2000); }
  loadFromLittleFS();

  lcd.clear(); lcd.print("Connecting WiFi"); lcd.setCursor(0,1); lcd.print(WIFI_SSID);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  int dotPos=0; unsigned long wifiStart=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-wifiStart<15000) { delay(400); lcd.setCursor(dotPos%16,1); lcd.print("."); dotPos++; }

  if(WiFi.status()==WL_CONNECTED) {
    wifiOK=true;
    lcd.clear(); lcd.print("WiFi Connected!"); lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString()); delay(1500);
    lcd.clear(); lcd.print("Syncing time...");
    configTime(GMT_OFFSET,DST_OFFSET,NTP_SERVER);
    unsigned long ntpStart=millis();
    while(time(nullptr)<1000000000&&millis()-ntpStart<5000) delay(200);
    if(time(nullptr)>1000000000) {
      if(rtcOK) { time_t now=time(nullptr); struct tm ti; localtime_r(&now,&ti); rtc.adjust(DateTime(ti.tm_year+1900,ti.tm_mon+1,ti.tm_mday,ti.tm_hour,ti.tm_min,ti.tm_sec)); }
      lcd.clear(); lcd.print("Time synced!"); lcd.setCursor(0,1); struct tm ti=getCurrentTime(); print12h(ti.tm_hour,ti.tm_min); delay(1500);
    }
    lcd.clear(); lcd.print("Connecting"); lcd.setCursor(0,1); lcd.print("Firebase...");
    fbConfig.host=FIREBASE_HOST;
    fbConfig.signer.tokens.legacy_token=FIREBASE_AUTH;
    fbConfig.token_status_callback=tokenStatusCallback;
    Firebase.begin(&fbConfig,&fbAuth);
    Firebase.reconnectWiFi(true);
    delay(1000); fbReady=true;
    syncBoxesFromFirebase();
    pushHeartbeat();
    // Listen to /boxes only — avoids callback loops from /log and /device writes
    Serial.println("Firebase ready — using polling");
    lcd.clear(); lcd.print("Firebase OK!"); delay(1000);
  } else {
    lcd.clear(); lcd.print("WiFi failed!");
    lcd.setCursor(0,1); lcd.print(rtcOK?"Using RTC time":"No time source"); delay(2000);
  }
  drawScreen();
}

// ============================================================
//  LOOP
// ============================================================
unsigned long lastHeartbeat=0;
int lastDrawnMin=-1;

void loop() {
  handleInputs();
  checkAlarms();
  struct tm ti=getCurrentTime();
  if(ti.tm_min!=lastDrawnMin&&currentMode==CLOCK_HOME) { lastDrawnMin=ti.tm_min; drawScreen(); }
  if(wifiOK&&millis()-lastHeartbeat>60000UL) { lastHeartbeat=millis(); pushHeartbeat(); }
  // Poll Firebase every 3s for dashboard changes + time commands
  // Poll Firebase every 3s — ONLY on clock home so buttons never lag
  static unsigned long lastPoll=0;
  if(wifiOK&&fbReady&&currentMode==CLOCK_HOME&&millis()-lastPoll>3000UL) {
    lastPoll=millis();
    pollFirebase();
  }
}
