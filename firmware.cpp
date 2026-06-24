// ============================================================
//  MacroPad Firmware – ESP32-S3
//  - 9 Tasten: frei belegbare HID-Makros (via Webserver)
//  - KY-040 Encoder: Lautstärke +/- (HID Media Keys)
//  - 1.8" ST7735 Display: zeigt GIFs und Tastenbeschriftung
//  - WiFi Webserver: Tasten belegen + GIF hochladen
// ============================================================

// ---- TFT_eSPI User Setup (Inline-Defines) ------------------
#define USER_SETUP_LOADED
#define ST7735_DRIVER
#define TFT_WIDTH  128
#define TFT_HEIGHT 160
#define TFT_CS   10
#define TFT_DC   14
#define TFT_RST  21
#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_MISO 13
#define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_GFXFF
#define SMOOTH_FONT

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <AnimatedGIF.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDConsumerControl.h>

// ---- Pin Definitions ----------------------------------------
#define ENC_CLK  4
#define ENC_DT   5
#define ENC_SW   6

struct MacroEntry {
  uint8_t type;        // 0=key, 1=text, 2=media
  uint8_t modifier;    // HID modifier byte (type 0)
  uint8_t keycode;     // HID keycode (type 0)
  char    text[64];    // String to type (type 1)
  uint8_t mediaCode;   // Consumer code (type 2)
  char    label[24];   // Display label
};


// Forward declarations
void gifDraw(GIFDRAW* pDraw);
void loadMacros();
void saveMacros();
void drawKeyLabels();
void fireMacro(int idx);
void IRAM_ATTR encoderISR();
async function init();
function buildGrid();
function typeChange(i);
async function saveMacros();
async function loadGifList();
async function uploadGif();
async function playGif(name);
async function stopGif();
void handleGif();
void stopGifPlayback();

const int BTN_PINS[9] = {1, 2, 3, 7, 15, 16, 17, 18, 40};

// ---- Objects ------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
AnimatedGIF gif;
AsyncWebServer server(80);
USBHIDKeyboard Keyboard;
USBHIDConsumerControl Consumer;

// ---- WiFi Credentials (AP Mode) ----------------------------
const char* AP_SSID = "MacroPad";
const char* AP_PASS = "macropad123";

// ---- Macro Storage ------------------------------------------
// Each key has a type: 0=key combo, 1=text, 2=media


MacroEntry macros[9];

// ---- GIF State ---------------------------------------------
bool     gifPlaying   = false;
File     gifFile;
uint8_t* gifBuf       = nullptr;
size_t   gifBufLen    = 0;
String   currentGifName = "";

// ---- Encoder State -----------------------------------------
volatile int  encPos     = 0;
volatile bool encPressed = false;
int  lastEncPos  = 0;
bool lastEncSW   = HIGH;
unsigned long lastEncDebounce = 0;
unsigned long lastBtnDebounce[9] = {0};
bool lastBtnState[9];
bool btnPressed[9] = {false};

// ---- GIF Draw Callback -------------------------------------
void gifDraw(GIFDRAW* pDraw) {
  uint8_t* s   = pDraw->pPixels;
  uint16_t* p  = pDraw->pPalette;
  int       y  = pDraw->iY + pDraw->y;
  uint16_t  lineBuffer[160];
  int xOff = (128 - pDraw->iWidth)  / 2;
  int yOff = (160 - pDraw->iHeight) / 2;
  if (xOff < 0) xOff = 0;
  if (yOff < 0) yOff = 0;
  for (int x = 0; x < pDraw->iWidth; x++) {
    uint16_t color = p[s[x]];
    lineBuffer[x] = (color >> 8) | (color << 8); // byte swap
  }
  tft.pushImage(xOff, yOff + y, pDraw->iWidth, 1, lineBuffer);
}

// ---- Load macros from SPIFFS --------------------------------
void loadMacros() {
  if (!SPIFFS.exists("/macros.json")) return;
  File f = SPIFFS.open("/macros.json", "r");
  if (!f) return;
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok) { f.close(); return; }
  f.close();
  JsonArray arr = doc.as<JsonArray>();
  for (int i = 0; i < 9 && i < (int)arr.size(); i++) {
    JsonObject obj = arr[i];
    macros[i].type     = obj["type"]     | 0;
    macros[i].modifier = obj["modifier"] | 0;
    macros[i].keycode  = obj["keycode"]  | 0;
    macros[i].mediaCode= obj["mediaCode"]| 0;
    strlcpy(macros[i].text,  obj["text"]  | "", sizeof(macros[i].text));
    strlcpy(macros[i].label, obj["label"] | "", sizeof(macros[i].label));
  }
}

// ---- Save macros to SPIFFS ----------------------------------
void saveMacros() {
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < 9; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["type"]      = macros[i].type;
    obj["modifier"]  = macros[i].modifier;
    obj["keycode"]   = macros[i].keycode;
    obj["mediaCode"] = macros[i].mediaCode;
    obj["text"]      = macros[i].text;
    obj["label"]     = macros[i].label;
  }
  File f = SPIFFS.open("/macros.json", "w");
  serializeJson(doc, f);
  f.close();
}

// ---- Draw key labels on TFT ---------------------------------
void drawKeyLabels() {
  if (gifPlaying) return;
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int i = 0; i < 9; i++) {
    int col = i % 3;
    int row = i / 3;
    int x = 4 + col * 42;
    int y = 4 + row * 52;
    tft.drawRect(x, y, 38, 48, TFT_CYAN);
    tft.setCursor(x + 3, y + 4);
    tft.print(i + 1);
    tft.setCursor(x + 3, y + 18);
    // Truncate label to fit
    char buf[8];
    strlcpy(buf, macros[i].label, sizeof(buf));
    tft.print(buf);
  }
}

// ---- Fire macro for key index ------------------------------
void fireMacro(int idx) {
  if (idx < 0 || idx >= 9) return;
  MacroEntry& m = macros[idx];
  switch (m.type) {
    case 0: // Key combo
      Keyboard.press(m.keycode);
      delay(20);
      Keyboard.releaseAll();
      break;
    case 1: // Text
      Keyboard.print(m.text);
      break;
    case 2: // Media
      Consumer.press(m.mediaCode);
      delay(20);
      Consumer.release();
      break;
    default:
      break;
  }
}

// ---- Encoder ISR -------------------------------------------
void IRAM_ATTR encoderISR() {
  static int lastCLK = HIGH;
  int clk = digitalRead(ENC_CLK);
  if (clk != lastCLK && clk == LOW) {
    int dt = digitalRead(ENC_DT);
    if (dt != clk) encPos++;
    else           encPos--;
  }
  lastCLK = clk;
}

// ---- Web UI HTML -------------------------------------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MacroPad Konfigurator</title>
<style>
  body{font-family:sans-serif;background:#1a1a2e;color:#eee;max-width:700px;margin:auto;padding:16px}
  h1{color:#00d4ff;text-align:center}
  .grid{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:20px}
  .btn-card{background:#16213e;border:1px solid #0f3460;border-radius:8px;padding:12px}
  .btn-card h3{margin:0 0 8px;color:#00d4ff;font-size:14px}
  label{font-size:12px;color:#aaa}
  input,select{width:100%;box-sizing:border-box;margin:3px 0 8px;padding:5px;background:#0f3460;border:1px solid #00d4ff;color:#fff;border-radius:4px}
  .section{background:#16213e;border:1px solid #0f3460;border-radius:8px;padding:14px;margin-bottom:16px}
  button{background:#00d4ff;color:#000;border:none;padding:8px 18px;border-radius:5px;cursor:pointer;font-weight:bold}
  button:hover{background:#00b8d9}
  #gifPreview{width:128px;height:160px;border:2px solid #00d4ff;display:block;margin:auto;background:#000;border-radius:4px}
  .gif-list span{display:inline-block;background:#0f3460;border-radius:4px;padding:3px 8px;margin:3px;cursor:pointer;font-size:13px}
  .gif-list span:hover{background:#00d4ff;color:#000}
  #status{color:#0f0;text-align:center;margin:8px 0}
</style>
</head>
<body>
<h1>🎮 MacroPad Konfigurator</h1>
<div id="status"></div>

<div class="section">
  <h2>Tasten belegen</h2>
  <div class="grid" id="keyGrid"></div>
  <button onclick="saveMacros()">💾 Speichern</button>
</div>

<div class="section">
  <h2>GIF hochladen &amp; verwalten</h2>
  <input type="file" id="gifFile" accept=".gif">
  <button onclick="uploadGif()">📤 Hochladen</button>
  <p style="font-size:12px;color:#aaa">Max. 200 KB · 128×160 px empfohlen</p>
  <div class="gif-list" id="gifList"></div>
  <p style="margin-top:10px;font-size:13px">Aktiv: <b id="activeGif">—</b>
  <button onclick="stopGif()" style="margin-left:10px;font-size:12px;padding:4px 10px">⏹ Stopp</button></p>
</div>

<script>
const TYPE_LABELS = ['Taste','Text','Medien'];
const MEDIA_OPTS = [
  {v:0,l:'—'},
  {v:0xCD,l:'Play/Pause'},
  {v:0xB5,l:'Nächster Track'},
  {v:0xB6,l:'Vorheriger Track'},
  {v:0xE9,l:'Lauter'},
  {v:0xEA,l:'Leiser'},
  {v:0xE2,l:'Mute'}
];

let macroData = [];

async function init() {
  const r = await fetch('/api/macros');
  macroData = await r.json();
  buildGrid();
  loadGifList();
}

function buildGrid() {
  const g = document.getElementById('keyGrid');
  g.innerHTML = '';
  for (let i=0;i<9;i++) {
    const m = macroData[i] || {type:0,modifier:0,keycode:0,mediaCode:0,text:'',label:''};
    g.innerHTML += `
    <div class="btn-card">
      <h3>Taste ${i+1}</h3>
      <label>Typ</label>
      <select id="type${i}" onchange="typeChange(${i})">
        <option value="0" ${m.type==0?'selected':''}>Taste (HID)</option>
        <option value="1" ${m.type==1?'selected':''}>Text tippen</option>
        <option value="2" ${m.type==2?'selected':''}>Medientaste</option>
      </select>
      <div id="keyFields${i}" style="display:${m.type==0?'block':'none'}">
        <label>Modifier (hex)</label>
        <input id="mod${i}" placeholder="z.B. 0x02 (Shift)" value="${m.modifier}">
        <label>Keycode (hex)</label>
        <input id="kc${i}" placeholder="z.B. 0x04 = A" value="${m.keycode}">
      </div>
      <div id="textField${i}" style="display:${m.type==1?'block':'none'}">
        <label>Text</label>
        <input id="txt${i}" value="${m.text}" maxlength="63">
      </div>
      <div id="mediaField${i}" style="display:${m.type==2?'block':'none'}">
        <label>Medienaktion</label>
        <select id="med${i}">
          ${MEDIA_OPTS.map(o=>`<option value="${o.v}" ${m.mediaCode==o.v?'selected':''}>${o.l}</option>`).join('')}
        </select>
      </div>
      <label>Label (Display)</label>
      <input id="lbl${i}" value="${m.label}" maxlength="7">
    </div>`;
  }
}

function typeChange(i) {
  const t = parseInt(document.getElementById('type'+i).value);
  document.getElementById('keyFields'+i).style.display = t==0?'block':'none';
  document.getElementById('textField'+i).style.display = t==1?'block':'none';
  document.getElementById('mediaField'+i).style.display= t==2?'block':'none';
}

async function saveMacros() {
  const data = [];
  for (let i=0;i<9;i++) {
    const t = parseInt(document.getElementById('type'+i).value);
    data.push({
      type:      t,
      modifier:  parseInt(document.getElementById('mod'+i)?.value||'0'),
      keycode:   parseInt(document.getElementById('kc'+i)?.value||'0'),
      text:      document.getElementById('txt'+i)?.value||'',
      mediaCode: parseInt(document.getElementById('med'+i)?.value||'0'),
      label:     document.getElementById('lbl'+i).value
    });
  }
  const r = await fetch('/api/macros',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});
  document.getElementById('status').textContent = r.ok ? '✅ Gespeichert!' : '❌ Fehler';
  setTimeout(()=>document.getElementById('status').textContent='',2500);
}

async function loadGifList() {
  const r = await fetch('/api/gifs');
  const list = await r.json();
  const el = document.getElementById('gifList');
  el.innerHTML = list.map(g=>`<span onclick="playGif('${g}')">${g}</span>`).join('');
}

async function uploadGif() {
  const f = document.getElementById('gifFile').files[0];
  if (!f) return;
  if (f.size > 204800) { alert('GIF zu groß! Max. 200 KB'); return; }
  const fd = new FormData();
  fd.append('gif', f, f.name);
  const r = await fetch('/api/upload', {method:'POST', body:fd});
  document.getElementById('status').textContent = r.ok ? '✅ GIF hochgeladen!' : '❌ Upload fehlgeschlagen';
  setTimeout(()=>document.getElementById('status').textContent='',2500);
  loadGifList();
}

async function playGif(name) {
  await fetch('/api/gif/play?name='+encodeURIComponent(name));
  document.getElementById('activeGif').textContent = name;
}

async function stopGif() {
  await fetch('/api/gif/stop');
  document.getElementById('activeGif').textContent = '—';
}

init();
</script>
</body>
</html>
)rawliteral";

// ---- Setup --------------------------------------------------
void setup() {
  Serial.begin(115200);

  // TFT Init
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 60);
  tft.print("MacroPad");
  tft.setTextSize(1);
  tft.setCursor(10, 90);
  tft.print("Booting...");

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    tft.setCursor(10, 105);
    tft.setTextColor(TFT_RED);
    tft.print("SPIFFS ERR");
  }

  // Load macros (defaults if none)
  memset(macros, 0, sizeof(macros));
  const char* defaultLabels[9] = {"Copy","Paste","Cut","Undo","Redo","Save","SS","Play","Mute"};
  for (int i = 0; i < 9; i++) strlcpy(macros[i].label, defaultLabels[i], sizeof(macros[i].label));
  loadMacros();

  // Encoder
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);

  // Buttons
  for (int i = 0; i < 9; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    lastBtnState[i] = HIGH;
  }

  // USB HID
  Keyboard.begin();
  Consumer.begin();
  USB.begin();

  // WiFi AP
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();

  // Web server routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/macros", HTTP_GET, [](AsyncWebServerRequest* req) {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < 9; i++) {
      JsonObject o = arr.createNestedObject();
      o["type"]      = macros[i].type;
      o["modifier"]  = macros[i].modifier;
      o["keycode"]   = macros[i].keycode;
      o["mediaCode"] = macros[i].mediaCode;
      o["text"]      = macros[i].text;
      o["label"]     = macros[i].label;
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/macros", HTTP_POST, [](AsyncWebServerRequest* req) {}, nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<2048> doc;
      if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
        req->send(400, "text/plain", "Bad JSON");
        return;
      }
      JsonArray arr = doc.as<JsonArray>();
      for (int i = 0; i < 9 && i < (int)arr.size(); i++) {
        JsonObject obj = arr[i];
        macros[i].type      = obj["type"]      | 0;
        macros[i].modifier  = obj["modifier"]  | 0;
        macros[i].keycode   = obj["keycode"]   | 0;
        macros[i].mediaCode = obj["mediaCode"] | 0;
        strlcpy(macros[i].text,  obj["text"]  | "", sizeof(macros[i].text));
        strlcpy(macros[i].label, obj["label"] | "", sizeof(macros[i].label));
      }
      saveMacros();
      drawKeyLabels();
      req->send(200, "text/plain", "OK");
    });

  server.on("/api/gifs", HTTP_GET, [](AsyncWebServerRequest* req) {
    String json = "[";
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    bool first = true;
    while (file) {
      String fname = String(file.name());
      if (fname.endsWith(".gif")) {
        if (!first) json += ",";
        json += "\"" + fname + "\"";
        first = false;
      }
      file = root.openNextFile();
    }
    json += "]";
    req->send(200, "application/json", json);
  });

  server.on("/api/upload", HTTP_POST, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "OK");
  }, [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
    static File uploadFile;
    if (!index) {
      String path = "/" + filename;
      uploadFile = SPIFFS.open(path, "w");
    }
    if (uploadFile) uploadFile.write(data, len);
    if (final && uploadFile) uploadFile.close();
  });

  server.on("/api/gif/play", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("name")) { req->send(400, "text/plain", "Missing name"); return; }
    String name = req->getParam("name")->value();
    if (!name.startsWith("/")) name = "/" + name;
    if (!SPIFFS.exists(name)) { req->send(404, "text/plain", "Not found"); return; }
    gifPlaying = true;
    currentGifName = name;
    req->send(200, "text/plain", "OK");
  });

  server.on("/api/gif/stop", HTTP_GET, [](AsyncWebServerRequest* req) {
    gifPlaying = false;
    currentGifName = "";
    drawKeyLabels();
    req->send(200, "text/plain", "OK");
  });

  server.begin();

  // Show IP on TFT
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 50);
  tft.print("MacroPad");
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 80);
  tft.print("WiFi: ");
  tft.println(AP_SSID);
  tft.setCursor(8, 95);
  tft.print("IP: ");
  tft.println(ip);
  tft.setCursor(8, 115);
  tft.print("PW: ");
  tft.println(AP_PASS);
  delay(3000);

  gif.begin(BIG_ENDIAN_PIXELS);
  drawKeyLabels();
}

// ---- GIF playback -------------------------------------------
void handleGif() {
  if (!gifPlaying || currentGifName.isEmpty()) return;

  // Load GIF buffer if not yet loaded
  if (gifBuf == nullptr) {
    File f = SPIFFS.open(currentGifName, "r");
    if (!f) { gifPlaying = false; return; }
    gifBufLen = f.size();
    gifBuf = (uint8_t*)ps_malloc(gifBufLen);
    if (!gifBuf) {
      // No PSRAM: try regular malloc
      gifBuf = (uint8_t*)malloc(gifBufLen);
    }
    if (!gifBuf) { f.close(); gifPlaying = false; return; }
    f.read(gifBuf, gifBufLen);
    f.close();
    if (gif.open(gifBuf, gifBufLen, gifDraw)) {
      tft.fillScreen(TFT_BLACK);
    } else {
      free(gifBuf);
      gifBuf = nullptr;
      gifPlaying = false;
      return;
    }
  }

  if (!gif.playFrame(true, nullptr)) {
    gif.reset(); // loop
  }
}

void stopGifPlayback() {
  if (gifBuf) { free(gifBuf); gifBuf = nullptr; }
  gif.close();
  gifBufLen = 0;
}

// ---- Loop ---------------------------------------------------
void loop() {
  // ---- Encoder volume control ----
  int pos = encPos;
  if (pos != lastEncPos) {
    int delta = pos - lastEncPos;
    lastEncPos = pos;
    if (delta > 0) {
      for (int i = 0; i < abs(delta); i++) {
        Consumer.press(0xE9); // Volume Up
        delay(10);
        Consumer.release();
      }
    } else {
      for (int i = 0; i < abs(delta); i++) {
        Consumer.press(0xEA); // Volume Down
        delay(10);
        Consumer.release();
      }
    }
  }

  // ---- Encoder button (Mute) ----
  bool swState = digitalRead(ENC_SW);
  if (swState == LOW && lastEncSW == HIGH) {
    if (millis() - lastEncDebounce > 50) {
      Consumer.press(0xE2); // Mute
      delay(20);
      Consumer.release();
      lastEncDebounce = millis();
    }
  }
  lastEncSW = swState;

  // ---- Macro buttons ----
  for (int i = 0; i < 9; i++) {
    bool state = digitalRead(BTN_PINS[i]);
    if (state == LOW && lastBtnState[i] == HIGH) {
      if (millis() - lastBtnDebounce[i] > 50) {
        fireMacro(i);
        lastBtnDebounce[i] = millis();
      }
    }
    lastBtnState[i] = state;
  }

  // ---- GIF playback ----
  if (gifPlaying) {
    handleGif();
  } else if (gifBuf != nullptr) {
    stopGifPlayback();
    drawKeyLabels();
  }

  delay(5);
}