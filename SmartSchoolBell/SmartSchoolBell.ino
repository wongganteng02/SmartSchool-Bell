/*
 * ============================================================
 *  SMART SCHOOL BELL OTOMATIS v2.1
 *  NodeMCU ESP8266 + RTC DS1302 + DFPlayer Mini + TOA
 * ============================================================
 *  Struktur file project:
 *
 *  SmartSchoolBell/
 *  ├── SmartSchoolBell.ino   ← sketch ini
 *  └── data/
 *      ├── index.html        ← dashboard web
 *      └── logo.png          ← logo sekolah (taruh manual)
 *
 *  Cara upload file ke LittleFS:
 *  Arduino IDE → Tools → ESP8266 Sketch Data Upload
 *  (Plugin: https://github.com/earlephilhower/arduino-littlefs-upload)
 *
 * ============================================================
 *  Wiring:
 *   RTC DS1302 : CLK→D5, DAT→D6, RST→D7, VCC→3.3V, GND→GND
 *   DFPlayer   : RX→D1 (via 1kΩ), TX→D2, VCC→5V, GND→GND
 *   Amplifier  : DAC_L/R DFPlayer → RCA → AUX IN Amplifier TOA
 *
 * ============================================================
 *  SD Card DFPlayer (isi manual):
 *   Format FAT32, taruh file di folder /mp3/
 *   Contoh:
 *     001_bel_masuk.mp3   → fileIndex = 1
 *     002_istirahat.mp3   → fileIndex = 2
 *     003_pulang.mp3      → fileIndex = 3
 *   Penamaan HARUS mulai 001 dan tidak boleh skip nomor.
 *
 * ============================================================
 *  Libraries (Arduino Library Manager):
 *   - ArduinoJson         v6.x  by Benoit Blanchon
 *   - Ds1302              by Rafa Couto
 *   - DFRobotDFPlayerMini by DFRobot
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ─── WiFi Credentials ────────────────────────────────────────
const char* WIFI_SSID     = "NAMA_WIFI_KAMU";
const char* WIFI_PASSWORD = "PASSWORD_WIFI_KAMU";

// ─── Pin Definitions ─────────────────────────────────────────
#define RTC_CLK_PIN  D5
#define RTC_DAT_PIN  D6
#define RTC_RST_PIN  D7
#define DF_RX_PIN    D2   // ESP RX ← DFPlayer TX
#define DF_TX_PIN    D1   // ESP TX → DFPlayer RX (via 1kΩ)

// ─── File Paths (LittleFS) ───────────────────────────────────
#define SCHEDULE_FILE  "/schedules.json"
#define SOUNDLIST_FILE "/soundlist.json"

// ─── Objects ─────────────────────────────────────────────────
ThreeWire rtcWire(RTC_DAT_PIN, RTC_CLK_PIN, RTC_RST_PIN); // DAT, CLK, RST
RtcDS1302<ThreeWire> rtc(rtcWire);
SoftwareSerial dfSerial(DF_RX_PIN, DF_TX_PIN);
DFRobotDFPlayerMini dfPlayer;
ESP8266WebServer server(80);

// ─── Data Structures ─────────────────────────────────────────

// Daftar suara yang tersedia di SD card DFPlayer
// Diisi manual lewat dashboard, index sesuai nomor file fisik di SD card
struct SoundEntry {
  int    index;   // nomor file di SD card (001 = 1, 002 = 2, dst)
  String nama;    // nama deskriptif, misal "Bel Masuk"
};

// Jadwal bel
struct Schedule {
  String jam;       // format "HH:MM"
  String hari;      // misal "Senin,Selasa,Rabu" atau "Semua"
  int    fileIndex; // index file SD card yang dibunyikan
  String nama;      // keterangan jadwal
};

std::vector<SoundEntry> soundList;
std::vector<Schedule>   schedules;

String  currentTime  = "00:00:00";
String  currentDay   = "Senin";
String  nextBellInfo = "-";
String  ipAddress    = "";
bool    bellPlaying  = false;
unsigned long lastCheck = 0;

const char* HARI_ID[] = {"Minggu","Senin","Selasa","Rabu","Kamis","Jumat","Sabtu"};

// ─── Forward Declarations ────────────────────────────────────
void loadSchedules();
void saveSchedules();
void loadSoundList();
void saveSoundList();
void checkBell(RtcDateTime& dt);
void updateNextBell();
String getSoundNama(int fileIndex);
String formatTime(RtcDateTime& dt);
String formatHHMM(RtcDateTime& dt);
String getMimeType(String filename);

// =============================================================
//  SETUP
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n\n=== Smart School Bell v2.1 Boot ==="));

  // ── LittleFS ──
  if (!LittleFS.begin()) {
    Serial.println(F("[FS] Mount FAILED! Formatting..."));
    LittleFS.format();
    LittleFS.begin();
  }
  Serial.println(F("[FS] LittleFS OK"));

  if (!LittleFS.exists("/index.html")) {
    Serial.println(F("[FS] WARNING: /index.html tidak ditemukan!"));
    Serial.println(F("[FS] Jalankan Tools > ESP8266 Sketch Data Upload"));
  }

  // ── RTC DS1302 ──
  rtc.Begin();
  // ► Uncomment SEKALI untuk set waktu, lalu comment lagi & upload ulang:
  // RtcDateTime compiled(__DATE__, __TIME__);
  // rtc.SetDateTime(compiled);
  // rtc.SetIsWriteProtected(false);
  // rtc.SetIsRunning(true);
  Serial.println(F("[RTC] DS1302 OK"));

  // ── DFPlayer Mini ──
  dfSerial.begin(9600);
  delay(1000);
  if (!dfPlayer.begin(dfSerial)) {
    Serial.println(F("[DF] DFPlayer tidak terdeteksi! Cek wiring & resistor 1kΩ."));
  } else {
    dfPlayer.volume(25); // 0–30
    delay(500);
    dfPlayer.play(1);
    dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
    Serial.println(F("[DF] DFPlayer OK"));
  }

  // ── WiFi ──
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("[WiFi] Connecting"));
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    ipAddress = WiFi.localIP().toString();
    Serial.println("\n[WiFi] Connected! IP: " + ipAddress);
  } else {
    Serial.println(F("\n[WiFi] FAILED → AP Mode"));
    WiFi.mode(WIFI_AP);
    WiFi.softAP("SmartBell_Setup", "12345678");
    ipAddress = WiFi.softAPIP().toString();
    Serial.println("[WiFi] AP IP: " + ipAddress);
  }

  // ── Load Data ──
  loadSoundList();
  loadSchedules();
  updateNextBell();

  // ── Web Routes ──

  // Halaman utama
  server.on("/", HTTP_GET, []() {
    if (LittleFS.exists("/index.html")) {
      File f = LittleFS.open("/index.html", "r");
      server.streamFile(f, "text/html");
      f.close();
    } else {
      server.send(200, "text/html",
        "<h2>index.html tidak ditemukan!</h2>"
        "<p>Jalankan: <b>Tools &rarr; ESP8266 Sketch Data Upload</b></p>"
      );
    }
  });

  // Logo sekolah
  server.on("/logo.png", HTTP_GET, []() {
    if (LittleFS.exists("/logo.png")) {
      File f = LittleFS.open("/logo.png", "r");
      server.streamFile(f, "image/png");
      f.close();
    } else {
      server.send(204);
    }
  });

  // ── API Endpoints ──
  server.on("/api/status",    HTTP_GET,  handleStatus);
  server.on("/api/schedules", HTTP_GET,  handleGetSchedules);
  server.on("/api/schedules", HTTP_POST, handleAddSchedule);
  server.on("/api/delete",    HTTP_POST, handleDeleteSchedule);
  server.on("/api/test",      HTTP_POST, handleTest);
  server.on("/api/sounds",    HTTP_GET,  handleGetSounds);
  server.on("/api/sounds",    HTTP_POST, handleAddSound);
  server.on("/api/sounds/delete", HTTP_POST, handleDeleteSound);

  // File statis lain dari LittleFS
  server.onNotFound([]() {
    String path = server.uri();
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      server.streamFile(f, getMimeType(path));
      f.close();
    } else {
      server.send(404, "text/plain", "Not found: " + path);
    }
  });

  server.begin();
  Serial.println(F("[Web] Server started"));
  Serial.println("► Buka browser: http://" + ipAddress);
}

// =============================================================
//  LOOP
// =============================================================
void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastCheck >= 1000) {
    lastCheck = now;

    RtcDateTime dt = rtc.GetDateTime();
    currentTime = formatTime(dt);
    currentDay  = String(HARI_ID[dt.DayOfWeek()]);

    checkBell(dt);
    updateNextBell();
  }

  if (dfPlayer.available()) {
    if (dfPlayer.readType() == DFPlayerPlayFinished) {
      bellPlaying = false;
    }
  }
}

// =============================================================
//  HELPERS
// =============================================================
String formatTime(RtcDateTime& dt) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", dt.Hour(), dt.Minute(), dt.Second());
  return String(buf);
}

String formatHHMM(RtcDateTime& dt) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", dt.Hour(), dt.Minute());
  return String(buf);
}

String getMimeType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css"))  return "text/css";
  if (filename.endsWith(".js"))   return "application/javascript";
  if (filename.endsWith(".png"))  return "image/png";
  if (filename.endsWith(".jpg"))  return "image/jpeg";
  if (filename.endsWith(".ico"))  return "image/x-icon";
  if (filename.endsWith(".json")) return "application/json";
  return "text/plain";
}

// Ambil nama suara berdasarkan fileIndex
String getSoundNama(int fileIndex) {
  for (auto& s : soundList) {
    if (s.index == fileIndex) return s.nama;
  }
  return "File #" + String(fileIndex);
}

// =============================================================
//  BELL LOGIC
// =============================================================
void checkBell(RtcDateTime& dt) {
  if (bellPlaying) return;
  if (dt.Second() != 0) return; // trigger tepat di detik :00

  String nowHHMM = formatHHMM(dt);
  String today   = String(HARI_ID[dt.DayOfWeek()]);

  for (auto& s : schedules) {
    if (s.jam != nowHHMM) continue;
    // Cek hari: "Semua" atau nama hari ada di string hari jadwal
    if (s.hari != "Semua" && s.hari.indexOf(today) == -1) continue;

    Serial.println("[BELL] ► " + s.nama + " | file index: " + s.fileIndex);
    dfPlayer.play(s.fileIndex);
    bellPlaying = true;
    break;
  }
}

void updateNextBell() {
  RtcDateTime dt = rtc.GetDateTime();
  String nowHHMM = formatHHMM(dt);
  String today   = String(HARI_ID[dt.DayOfWeek()]);

  String bestTime  = "99:99";
  int    bestIndex = -1;
  String bestNama  = "";

  for (auto& s : schedules) {
    bool hariCocok = (s.hari == "Semua" || s.hari.indexOf(today) != -1);
    if (!hariCocok) continue;
    if (s.jam > nowHHMM && s.jam < bestTime) {
      bestTime  = s.jam;
      bestIndex = s.fileIndex;
      bestNama  = s.nama;
    }
  }

  if (bestTime == "99:99") {
    nextBellInfo = "Tidak ada jadwal hari ini";
  } else {
    nextBellInfo = bestTime + " – " + bestNama;
  }
}

// =============================================================
//  SOUND LIST (daftar suara di SD card, diinput manual)
// =============================================================
void loadSoundList() {
  soundList.clear();
  File f = LittleFS.open(SOUNDLIST_FILE, "r");
  if (!f) return;
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    for (JsonObject obj : doc.as<JsonArray>()) {
      SoundEntry e;
      e.index = obj["index"].as<int>();
      e.nama  = obj["nama"].as<String>();
      soundList.push_back(e);
    }
  }
  f.close();
  Serial.println("[FS] " + String(soundList.size()) + " sound(s) loaded");
}

void saveSoundList() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& e : soundList) {
    JsonObject obj = arr.createNestedObject();
    obj["index"] = e.index;
    obj["nama"]  = e.nama;
  }
  File f = LittleFS.open(SOUNDLIST_FILE, "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// =============================================================
//  SCHEDULES
// =============================================================
void loadSchedules() {
  schedules.clear();
  File f = LittleFS.open(SCHEDULE_FILE, "r");
  if (!f) return;
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    for (JsonObject obj : doc.as<JsonArray>()) {
      Schedule s;
      s.jam       = obj["jam"].as<String>();
      s.hari      = obj["hari"].as<String>();
      s.fileIndex = obj["fileIndex"].as<int>();
      s.nama      = obj["nama"].as<String>();
      schedules.push_back(s);
    }
  }
  f.close();
  Serial.println("[FS] " + String(schedules.size()) + " schedule(s) loaded");
}

void saveSchedules() {
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& s : schedules) {
    JsonObject obj = arr.createNestedObject();
    obj["jam"]       = s.jam;
    obj["hari"]      = s.hari;
    obj["fileIndex"] = s.fileIndex;
    obj["nama"]      = s.nama;
  }
  File f = LittleFS.open(SCHEDULE_FILE, "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

// =============================================================
//  API HANDLERS
// =============================================================

// GET /api/status
void handleStatus() {
  DynamicJsonDocument doc(512);
  doc["time"]     = currentTime;
  doc["day"]      = currentDay;
  doc["ip"]       = ipAddress;
  doc["nextBell"] = nextBellInfo;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// GET /api/schedules
void handleGetSchedules() {
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < (int)schedules.size(); i++) {
    JsonObject obj = arr.createNestedObject();
    obj["id"]        = i;
    obj["jam"]       = schedules[i].jam;
    obj["hari"]      = schedules[i].hari;
    obj["fileIndex"] = schedules[i].fileIndex;
    obj["nama"]      = schedules[i].nama;
    obj["soundNama"] = getSoundNama(schedules[i].fileIndex);
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/schedules  body: {"jam":"07:00","hari":"Semua","fileIndex":1,"nama":"Bel Masuk"}
void handleAddSchedule() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

  Schedule s;
  s.jam       = doc["jam"].as<String>();
  s.hari      = doc["hari"].as<String>();
  s.fileIndex = doc["fileIndex"].as<int>();
  s.nama      = doc["nama"].as<String>();

  if (s.jam.length() != 5 || s.fileIndex < 1) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Data tidak valid\"}");
    return;
  }

  schedules.push_back(s);
  saveSchedules();
  updateNextBell();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/delete  body: {"id":0}
void handleDeleteSchedule() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument doc(128);
  deserializeJson(doc, server.arg("plain"));
  int id = doc["id"];
  if (id >= 0 && id < (int)schedules.size()) {
    schedules.erase(schedules.begin() + id);
    saveSchedules();
    updateNextBell();
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(404, "application/json", "{\"ok\":false}");
  }
}

// POST /api/test  body: {"fileIndex":1}
void handleTest() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument doc(128);
  deserializeJson(doc, server.arg("plain"));
  int idx = doc["fileIndex"].as<int>();
  if (idx < 1) { server.send(400, "application/json", "{\"ok\":false}"); return; }
  Serial.println("[TEST] ► Play index: " + String(idx));
  dfPlayer.play(idx);
  bellPlaying = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// GET /api/sounds  → list suara yang terdaftar
void handleGetSounds() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& e : soundList) {
    JsonObject obj = arr.createNestedObject();
    obj["index"] = e.index;
    obj["nama"]  = e.nama;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /api/sounds  body: {"index":1,"nama":"Bel Masuk"}
// Untuk daftarkan suara baru yang sudah ada di SD card
void handleAddSound() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "Bad JSON"); return; }

  int    idx  = doc["index"].as<int>();
  String nama = doc["nama"].as<String>();

  if (idx < 1 || nama.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Index minimal 1 dan nama tidak boleh kosong\"}");
    return;
  }

  // Cek duplikat index
  for (auto& e : soundList) {
    if (e.index == idx) {
      server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Index sudah terdaftar\"}");
      return;
    }
  }

  SoundEntry e;
  e.index = idx;
  e.nama  = nama;
  soundList.push_back(e);
  saveSoundList();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/sounds/delete  body: {"index":1}
void handleDeleteSound() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "No body"); return; }
  DynamicJsonDocument doc(128);
  deserializeJson(doc, server.arg("plain"));
  int idx = doc["index"].as<int>();

  for (int i = 0; i < (int)soundList.size(); i++) {
    if (soundList[i].index == idx) {
      soundList.erase(soundList.begin() + i);
      saveSoundList();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
  }
  server.send(404, "application/json", "{\"ok\":false,\"msg\":\"Tidak ditemukan\"}");
}
