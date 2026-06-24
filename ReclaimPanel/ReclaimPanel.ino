/*
 * ReclaimPanel Firmware v1.8.0
 * Open source ESP32 driver for P20 triangle LED panels
 * https://github.com/thedrapercode/ReclaimPanel
 *
 * Developed by JD Labs - Draper Family Light Show, Kenton Ohio
 * MIT License
 *
 * PANEL SPECS (reverse engineered):
 * - 20x20 = 400 RGB triangles per panel, each = 1 RGB pixel (3 LEDs)
 * - 5 groups, each an 80-bit shift register chain
 * - CLK/LAT/OE shared across all 5 groups
 * - 80 clocks total per frame, ONE latch at end
 * - Serpentine bit pattern verified by hardware testing
 *
 * DDP PROTOCOL:
 * - Flags byte bit 0 = PUSH flag: last packet of frame, render now
 * - Flags byte bits 3-4 = QUERY/REPLY: reject these
 * - Bytes 4-7 = absolute channel offset (big-endian uint32)
 * - ESP32 subtracts (startChannel-1) to get local buffer position
 *
 * FPP SETUP:
 * - Output Type: DDP (not Raw Channel Numbers)
 * - Universe Size: 1200 per panel
 * - Start Channel: match xLights model start channel
 *
 * WIRING (Ribbon Pin -> ESP32 GPIO):
 * Pin 1  DR1 -> GPIO 25    Pin 2  DG1 -> GPIO 26    Pin 3  DB1 -> GPIO 27
 * Pin 4  DR2 -> GPIO 14    Pin 5  DG2 -> GPIO 33    Pin 6  DB2 -> GPIO 13
 * Pin 7  DR3 -> GPIO 23    Pin 8  DG3 -> GPIO 19    Pin 9  DB3 -> GPIO 5
 * Pin 10 DR4 -> GPIO 17    Pin 11 DG4 -> GPIO 16    Pin 12 DB4 -> GPIO 4
 * Pin 13 DR5 -> GPIO 2     Pin 14 DG5 -> GPIO 15    Pin 15 DB5 -> GPIO 18
 * Pin 16 GND -> GND        Pin 17 CLK -> GPIO 22    Pin 18 GND -> GND
 * Pin 19 LAT -> GPIO 21    Pin 20 OE  -> GPIO 3
 * CRITICAL: Also connect 5V supply GND to ESP32 GND
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>

// ============================================================
// PIN DEFINITIONS
// ============================================================
#define DR1 25
#define DG1 26
#define DB1 27
#define DR2 14
#define DG2 33
#define DB2 13
#define DR3 23
#define DG3 19
#define DB3  5
#define DR4 17
#define DG4 16
#define DB4  4
#define DR5  2
#define DG5 15
#define DB5 18
#define CLK 22
#define LAT 21
#define OE   3
#define TRIGGER_PIN 34

// ============================================================
// CONSTANTS
// ============================================================
#define DDP_PORT              4048
#define DDP_HEADER_SIZE       10
#define DDP_FLAGS_PUSH        0x01
#define DDP_FLAGS_QUERY       0x08
#define DDP_FLAGS_REPLY       0x10
#define PANEL_WIDTH           20
#define PANEL_HEIGHT          20
#define PIXELS_PER_PANEL      (PANEL_WIDTH * PANEL_HEIGHT)   // 400
#define CHANNELS_PER_PANEL    (PIXELS_PER_PANEL * 3)         // 1200
#define MAX_PANELS            4
#define GROUPS                5
#define BITS_PER_GROUP        80
#define ROWS_PER_GROUP        4
#define MIN_RENDER_INTERVAL   15
#define BCM_BITS              4     // 4-bit BCM = 16 brightness levels per channel (4096 colors)
// BCM_BASE_US is now a runtime variable (bcmBaseUs) - adjustable from web UI
#define AP_SSID               "ReclaimPanel-Setup"
#define AP_PASSWORD           "reclaim1"
#define JUMPSCARE_MS          500
#define FW_VERSION            "1.8.0"

// ============================================================
// GLOBALS
// ============================================================
Preferences prefs;
WebServer   server(80);
WiFiUDP     udp;

// Settings
char  wifiSSID[64]    = "";
char  wifiPass[64]    = "";
char  staticIPStr[20] = "10.0.0.101";
char  gatewayStr[20]  = "10.0.0.1";
char  subnetStr[20]   = "255.255.255.0";
char  unitName[32]    = "ReclaimPanel";
int   numPanels       = 1;
int   startChannel    = 1;
int   brightness      = 100;
int   pwmLevels       = 8;
int   currentPWMLevel = 0;
bool  configured      = false;
bool  pwmMode         = true;
bool  flipRows        = false;
bool  flipCols        = false;
int   panelCols       = 1;          // columns in panel grid (1 = linear chain)
char  panelArrangement[20] = "0,1,2,3"; // panel index at each grid pos, left-right top-bottom from front

// Buffers
uint8_t  pixelBuffer[MAX_PANELS * PIXELS_PER_PANEL * 3];
uint8_t  ddpBuffer[4820];  // must hold 10-byte header + up to 4800 channels (4 panels in one packet)

// State
bool          apMode          = false;
bool          jumpScareActive = false;
bool          newFrameData    = false;
bool          chaseActive     = false;
int           chasePixel      = 0;
unsigned long jumpScareStart  = 0;
unsigned long lastDDPPacket   = 0;
unsigned long lastRender      = 0;
unsigned long chaseTimer      = 0;
unsigned long heartbeatTimer  = 0;
unsigned long sketchStartTime = 0;
unsigned long ddpSecTimer     = 0;
uint32_t      ddpPacketCount  = 0;
uint32_t      ddpPacketsPerSec= 0;
uint32_t      ddpCountLast    = 0;
uint32_t      renderCount     = 0;
uint32_t      rendersPerSec   = 0;
uint32_t      renderCountLast = 0;

#define LOG_LINES    20
#define LOG_LINE_MAX 72
char    logBuf[LOG_LINES][LOG_LINE_MAX];
uint8_t logHead  = 0;
uint8_t logCount = 0;
bool    debugLog = false;
int     bcmBaseUs = 500;   // shortest BCM bit-plane hold in microseconds; full cycle = 15x this value

const int redPins[]   = {DR1, DR2, DR3, DR4, DR5};
const int greenPins[] = {DG1, DG2, DG3, DG4, DG5};
const int bluePins[]  = {DB1, DB2, DB3, DB4, DB5};

// ============================================================
// BIT -> PIXEL LOOKUP TABLE
// combinedBitToPixel[panelFirmwareIdx][group][bit] = pixel index in combined xLights buffer
// ============================================================
uint16_t combinedBitToPixel[MAX_PANELS][GROUPS][BITS_PER_GROUP];

void bitToDeviceRC(int bit, int &row4, int &col20) {
  int block    = bit / 16;
  int localBit = bit % 16;
  int colLocal;
  if (localBit < 8) {
    int j    = localBit / 2;
    colLocal = 3 - j;
    row4     = 2 + (localBit % 2);
  } else {
    int j    = (localBit - 8) / 2;
    colLocal = j;
    row4     = 1 - (localBit % 2);
  }
  col20 = block * 4 + colLocal;
}

// Parse panelArrangement string "1,0,3,2" into array
void parseArrangement(uint8_t *arr) {
  int idx = 0;
  char buf[20];
  strncpy(buf, panelArrangement, sizeof(buf)-1);
  char *tok = strtok(buf, ",");
  while (tok && idx < MAX_PANELS) { arr[idx++] = (uint8_t)atoi(tok); tok = strtok(NULL, ","); }
  while (idx < MAX_PANELS) arr[idx++] = idx; // fill remainder with defaults
}

void buildPixelMap() {
  int cols = (panelCols > 0) ? panelCols : 1;
  int combinedWidth = cols * PANEL_WIDTH;
  uint8_t arrangement[MAX_PANELS];
  parseArrangement(arrangement);

  for (int p = 0; p < numPanels; p++) {
    // Find which grid position this panel index occupies
    int gridPos = p; // default: linear
    for (int i = 0; i < numPanels; i++) {
      if (arrangement[i] == (uint8_t)p) { gridPos = i; break; }
    }
    int gRow = gridPos / cols;
    int gCol = gridPos % cols;

    for (int g = 0; g < GROUPS; g++) {
      for (int b = 0; b < BITS_PER_GROUP; b++) {
        int devRow4, devCol20;
        bitToDeviceRC(b, devRow4, devCol20);
        int panelRow = g * ROWS_PER_GROUP + devRow4;
        int panelCol = devCol20;
        if (flipRows) panelRow = PANEL_HEIGHT - 1 - panelRow;
        if (flipCols) panelCol = PANEL_WIDTH  - 1 - panelCol;
        int combinedRow = gRow * PANEL_HEIGHT + panelRow;
        int combinedCol = gCol * PANEL_WIDTH  + panelCol;
        // "Top Right, Horizontal" xLights model: col 0 = rightmost physical col
        int xlCol = (combinedWidth - 1) - combinedCol;
        combinedBitToPixel[p][g][b] = (uint16_t)(combinedRow * combinedWidth + xlCol);
      }
    }
  }
}

// ============================================================
// PANEL DRIVING
// ============================================================
void clockPulse() {
  digitalWrite(CLK, HIGH); delayMicroseconds(1);
  digitalWrite(CLK, LOW);  delayMicroseconds(1);
}

// No-delay clock for BCM - ~200ns per pulse vs ~2us. If you see pixel
// corruption, add delayMicroseconds(1) back and raise BCM_BASE_US to 3000.
void fastClockPulse() {
  digitalWrite(CLK, HIGH);
  digitalWrite(CLK, LOW);
}

void latch() {
  digitalWrite(OE, HIGH);
  digitalWrite(LAT, HIGH); delayMicroseconds(1);
  digitalWrite(LAT, LOW);  delayMicroseconds(1);
  digitalWrite(OE, LOW);
}

// Clock all panels in one pass (reverse order) then latch once.
// combinedBitToPixel handles routing to the correct position in the
// combined xLights buffer regardless of grid layout.
void writeAllPanelsBinary() {
  uint8_t threshold = (128 * brightness) / 100;
  if (threshold < 1) threshold = 1;
  for (int p = numPanels - 1; p >= 0; p--) {
    for (int b = 0; b < BITS_PER_GROUP; b++) {
      for (int g = 0; g < GROUPS; g++) {
        int px = (int)combinedBitToPixel[p][g][b] * 3;
        if (px + 2 >= MAX_PANELS * PIXELS_PER_PANEL * 3) continue;
        uint8_t r  = pixelBuffer[px];
        uint8_t gr = pixelBuffer[px + 1];
        uint8_t bl = pixelBuffer[px + 2];
        digitalWrite(redPins[g],   (r  >= threshold) ? HIGH : LOW);
        digitalWrite(greenPins[g], (gr >= threshold) ? HIGH : LOW);
        digitalWrite(bluePins[g],  (bl >= threshold) ? HIGH : LOW);
      }
      clockPulse();
    }
  }
  latch();
}

// BCM bit-plane write. bitPlane 0 = LSB (shortest hold), BCM_BITS-1 = MSB (longest hold).
// Uses top BCM_BITS bits of the brightness-scaled color value.
void writeAllPanelsBCM(uint8_t bitPlane) {
  uint8_t bitMask = 1 << (bitPlane + (8 - BCM_BITS));  // BCM_BITS=4: 0x10,0x20,0x40,0x80
  for (int p = numPanels - 1; p >= 0; p--) {
    for (int b = 0; b < BITS_PER_GROUP; b++) {
      for (int g = 0; g < GROUPS; g++) {
        int px = (int)combinedBitToPixel[p][g][b] * 3;
        if (px + 2 >= MAX_PANELS * PIXELS_PER_PANEL * 3) continue;
        uint8_t r  = (uint16_t)pixelBuffer[px]     * brightness / 100;
        uint8_t gr = (uint16_t)pixelBuffer[px + 1] * brightness / 100;
        uint8_t bl = (uint16_t)pixelBuffer[px + 2] * brightness / 100;
        digitalWrite(redPins[g],   (r  & bitMask) ? HIGH : LOW);
        digitalWrite(greenPins[g], (gr & bitMask) ? HIGH : LOW);
        digitalWrite(bluePins[g],  (bl & bitMask) ? HIGH : LOW);
      }
      fastClockPulse();
    }
  }
  latch();
}

void renderPanels() {
  writeAllPanelsBinary();
  lastRender = millis();
  renderCount++;
}

void setSolid(uint8_t r, uint8_t g, uint8_t b) {
  int total = numPanels * PIXELS_PER_PANEL * 3;
  for (int i = 0; i < total; i += 3) {
    pixelBuffer[i]     = r;
    pixelBuffer[i + 1] = g;
    pixelBuffer[i + 2] = b;
  }
  newFrameData = true;
}

void addLog(const char* msg) {
  if (!debugLog) return;
  snprintf(logBuf[logHead], LOG_LINE_MAX, "[%lus] %s", millis() / 1000, msg);
  logHead  = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
}

// ============================================================
// SETTINGS
// ============================================================
void loadSettings() {
  prefs.begin("rclmpanel", true);
  configured   = prefs.getBool("configured", false);
  prefs.getString("ssid",    wifiSSID,    sizeof(wifiSSID));
  prefs.getString("pass",    wifiPass,    sizeof(wifiPass));
  prefs.getString("ip",      staticIPStr, sizeof(staticIPStr));
  prefs.getString("gateway", gatewayStr,  sizeof(gatewayStr));
  prefs.getString("subnet",  subnetStr,   sizeof(subnetStr));
  prefs.getString("name",    unitName,    sizeof(unitName));
  numPanels    = prefs.getInt("panels",     1);
  startChannel = prefs.getInt("startch",    1);
  brightness   = prefs.getInt("brightness", 100);
  pwmLevels    = prefs.getInt("pwmlevels",  8);
  pwmMode      = prefs.getBool("pwm",       true);
  flipRows     = prefs.getBool("fliprows",  false);
  flipCols     = prefs.getBool("flipcols",  false);
  panelCols    = prefs.getInt("panelcols",  1);
  prefs.getString("arrangement", panelArrangement, sizeof(panelArrangement));
  if (strlen(panelArrangement) == 0) strcpy(panelArrangement, "0,1,2,3");
  debugLog     = prefs.getBool("debuglog",  false);
  bcmBaseUs    = prefs.getInt("bcmbaseus",  500);
  prefs.end();
}

void saveSettings() {
  prefs.begin("rclmpanel", false);
  prefs.putBool("configured", true);
  prefs.putString("ssid",    wifiSSID);
  prefs.putString("pass",    wifiPass);
  prefs.putString("ip",      staticIPStr);
  prefs.putString("gateway", gatewayStr);
  prefs.putString("subnet",  subnetStr);
  prefs.putString("name",    unitName);
  prefs.putInt("panels",     numPanels);
  prefs.putInt("startch",    startChannel);
  prefs.putInt("brightness", brightness);
  prefs.putInt("pwmlevels",  pwmLevels);
  prefs.putBool("pwm",       pwmMode);
  prefs.putBool("fliprows",    flipRows);
  prefs.putBool("flipcols",    flipCols);
  prefs.putInt("panelcols",    panelCols);
  prefs.putString("arrangement", panelArrangement);
  prefs.putBool("debuglog",      debugLog);
  prefs.putInt("bcmbaseus",      bcmBaseUs);
  prefs.end();
}

// ============================================================
// WEB INTERFACE
// ============================================================
const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>ReclaimPanel</title>
<style>
  *{box-sizing:border-box}
  body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:0;padding:16px}
  h1{color:#ff6600;margin-bottom:4px}
  h2{color:#aaa;font-size:13px;margin-top:0;font-weight:normal}
  h3{color:#ff6600;margin-top:20px;border-bottom:1px solid #333;padding-bottom:4px;font-size:15px}
  label{display:block;margin-top:10px;font-size:13px;color:#aaa}
  input[type=text],input[type=password],select{width:100%;padding:8px;margin-top:3px;background:#222;border:1px solid #444;border-radius:4px;color:#eee;font-size:14px}
  input[type=range]{width:100%;margin-top:6px}
  input[type=file]{color:#eee;margin-top:6px}
  .row{display:flex;align-items:center;margin-top:10px;gap:10px}
  .row label{margin:0}
  input[type=checkbox]{width:18px;height:18px;cursor:pointer}
  .btn{padding:9px 18px;margin:5px 3px 0 0;border:none;border-radius:4px;cursor:pointer;font-size:13px;font-weight:bold}
  .btn-save   {background:#ff6600;color:#fff;width:100%;margin-top:12px;padding:11px;font-size:15px}
  .btn-reboot {background:#884400;color:#fff}
  .btn-reset  {background:#880000;color:#fff}
  .btn-ota    {background:#004488;color:#fff;width:100%;margin-top:8px;padding:11px}
  .btn-red    {background:#cc0000;color:#fff}
  .btn-green  {background:#006600;color:#fff}
  .btn-blue   {background:#000099;color:#fff}
  .btn-cyan   {background:#007777;color:#fff}
  .btn-orange {background:#cc5500;color:#fff}
  .btn-mag    {background:#880088;color:#fff}
  .btn-white  {background:#888;color:#fff}
  .btn-off    {background:#333;color:#fff}
  .btn-scare  {background:#660066;color:#fff}
  .btn-chase  {background:#006666;color:#fff}
  .btn-log    {background:#222244;color:#8888ff;border:1px solid #444488;font-size:12px;padding:6px 12px}
  .card{background:#1a1a1a;border:1px solid #333;border-radius:6px;padding:14px;margin-top:12px}
  .status{font-size:13px;line-height:1.9}
  .status b{color:#888}
  .ok{color:#00cc00}
  .warn{color:#ffaa00}
  .err{color:#ff4444}
  .diag{background:#111;border:1px solid #2a2a4a;border-radius:4px;padding:10px;margin-top:8px;font-size:12px;color:#88a;font-family:monospace;line-height:1.9}
  .info{background:#1a2a1a;border:1px solid #2a4a2a;border-radius:4px;padding:10px;margin-top:8px;font-size:12px;color:#8a8;line-height:1.7}
  .logbox{background:#0a0a0a;border:1px solid #224;border-radius:4px;padding:10px;margin-top:8px;font-size:11px;color:#88a;font-family:monospace;line-height:1.7;min-height:60px;max-height:200px;overflow-y:auto;white-space:pre-wrap;word-break:break-all}
  .rssi-bar{display:inline-block;width:60px;height:10px;background:#333;border-radius:3px;vertical-align:middle;margin-left:4px;overflow:hidden}
  .rssi-fill{height:100%;border-radius:3px}
  .badge{display:inline-block;padding:1px 7px;border-radius:8px;font-size:11px;font-weight:bold;margin-left:4px}
  .pwm-on{background:#004400;color:#00cc00}
  .pwm-off{background:#444400;color:#cccc00}
  #ota-progress{display:none;margin-top:10px}
  .progress-bar{width:100%;background:#333;border-radius:4px;height:20px;overflow:hidden}
  .progress-fill{height:100%;background:#ff6600;width:0%;transition:width 0.3s;border-radius:4px}
  #ota-status{margin-top:6px;font-size:13px;color:#aaa}
  .tab-bar{display:flex;gap:4px;margin-top:12px;flex-wrap:wrap}
  .tab{padding:8px 14px;border:1px solid #444;border-radius:4px 4px 0 0;cursor:pointer;font-size:13px;background:#1a1a1a;color:#aaa}
  .tab.active{background:#ff6600;color:#fff;border-color:#ff6600}
  .tab-content{display:none;border:1px solid #333;border-radius:0 4px 4px 4px;padding:14px;background:#1a1a1a}
  .tab-content.active{display:block}
  .val{color:#ff6600;font-weight:bold;margin-left:6px}
  .stat-grid{display:grid;grid-template-columns:140px 1fr;gap:5px 12px;font-size:12px;margin-top:8px;align-items:center}
  .stat-label{color:#666}
  .stat-value{color:#aaf;font-family:monospace}
</style>
</head>
<body>
<h1 id='pageTitle'>ReclaimPanel</h1>
<h2>JD Labs - Draper Family Light Show &nbsp;|&nbsp; FW <span id='fwver'>-</span></h2>

<div class='card status'>
  <b>Unit:</b> <span id='uname' class='ok'>-</span> &nbsp;&nbsp;
  <b>Status:</b> <span id='connstat'>-</span> &nbsp;&nbsp;
  <b>IP:</b> <span id='ipaddr'>-</span><br>
  <b>Panels:</b> <span id='npanels'>-</span> &nbsp;&nbsp;
  <b>Start Ch:</b> <span id='startch'>-</span> &nbsp;&nbsp;
  <b>Total Ch:</b> <span id='totalch'>-</span><br>
  <b>Mode:</b> <span id='modespan'>-</span> &nbsp;&nbsp;
  <b>Brightness:</b> <span id='brtpct'>-</span>% &nbsp;&nbsp;
  <b>Renders:</b> <span id='renders'>-</span><br>
  <b>Uptime:</b> <span id='uptime'>-</span>
</div>

<div id='update-banner' style='display:none;background:#1a2800;border:1px solid #446600;border-radius:6px;padding:10px 14px;margin-top:10px;font-size:13px;color:#aacc44'>
  Update available: <b><span id='update-ver'></span></b> &nbsp;-&nbsp;
  <a href='https://github.com/thedrapercode/ReclaimPanel/releases/latest' target='_blank' style='color:#ff6600'>Download on GitHub</a>
</div>

<div class='tab-bar'>
  <div class='tab active' onclick='showTab("test",this)'>Test</div>
  <div class='tab' onclick='showTab("display",this)'>Display</div>
  <div class='tab' onclick='showTab("network",this)'>Network</div>
  <div class='tab' onclick='showTab("panels",this)'>Panels</div>
  <div class='tab' onclick='showTab("xlights",this)'>xLights</div>
  <div class='tab' onclick='showTab("diag",this)'>Diagnostics</div>
  <div class='tab' onclick='showTab("ota",this)'>OTA Update</div>
</div>

<!-- TEST TAB -->
<div id='tab-test' class='tab-content active'>
  <h3>Test Colors</h3>
  <button class='btn btn-red'    onclick='test("red")'>Red</button>
  <button class='btn btn-green'  onclick='test("green")'>Green</button>
  <button class='btn btn-blue'   onclick='test("blue")'>Blue</button>
  <button class='btn btn-cyan'   onclick='test("cyan")'>Cyan</button>
  <button class='btn btn-orange' onclick='test("orange")'>Orange</button>
  <button class='btn btn-mag'    onclick='test("magenta")'>Magenta</button>
  <button class='btn btn-white'  onclick='test("white")'>White</button>
  <button class='btn btn-off'    onclick='test("off")'>Off</button>
  <button class='btn btn-scare'  onclick='test("scare")'>Jump Scare</button>
  <button class='btn btn-chase'  onclick='toggleChase()' id='chase_btn'>Chase Test</button>
  <div class='info' style='margin-top:12px'>
    <b>Chase Test</b> animates a pixel walker driven entirely by the ESP32 - no FPP needed.
    If Chase works but FPP does not, the issue is in your FPP or xLights configuration, not this controller.
    Check the Diagnostics tab for live connection status and DDP packet count.
  </div>
</div>

<!-- DISPLAY TAB -->
<div id='tab-display' class='tab-content'>
  <h3>Brightness</h3>
  <label>Panel Brightness: <span class='val' id='brt_disp'>100</span>%</label>
  <input type='range' id='brt_slider' min='1' max='100' value='100'
    oninput="document.getElementById('brt_disp').textContent=this.value; setBrightness(this.value)"
    onchange='setBrightness(this.value)'>
  <h3>Color Mode</h3>
  <div class='row'>
    <input type='checkbox' id='pwm_chk' onchange='togglePWM()'>
    <label for='pwm_chk'>BCM color mode - enables full color mixing (cyan, orange, purple, pastels)</label>
  </div>
  <div class='info' style='margin-top:10px'>
    <b>BCM (Binary Code Modulation)</b> cycles 4 bit-planes at a set rate to produce
    16 brightness levels per channel and 4096 total colors. When off, binary mode renders
    on/off only (8 colors) but responds instantly to every DDP frame with no extra overhead.
  </div>
  <h3>BCM Cycle Speed</h3>
  <label>Bit-plane hold time: <span class='val' id='bcm_disp'>500</span> us
    &nbsp;|&nbsp; Refresh rate: <span class='val' id='bcm_hz'>133</span> Hz</label>
  <input type='range' id='bcm_slider' min='100' max='3000' step='50' value='500'
    oninput="updateBCMDisplay(this.value)"
    onchange="setBCMSpeed(this.value)">
  <div class='info' style='margin-top:6px;font-size:11px'>
    Lower = faster refresh, less flicker. Higher = slower, more stable if you see pixel artifacts.
    100 us = ~267 Hz &nbsp;|&nbsp; 500 us = ~133 Hz (default) &nbsp;|&nbsp; 1000 us = ~67 Hz &nbsp;|&nbsp; 3000 us = ~22 Hz.
    Hit Save to keep across reboots.
  </div>
  <h3>Orientation</h3>
  <div class='row'>
    <input type='checkbox' id='fliprows_chk' onchange='setOrientation()'>
    <label for='fliprows_chk'>Flip Rows (use if image is upside down)</label>
  </div>
  <div class='row'>
    <input type='checkbox' id='flipcols_chk' onchange='setOrientation()'>
    <label for='flipcols_chk'>Flip Columns (use if image is mirrored left-right)</label>
  </div>
</div>

<!-- NETWORK TAB -->
<div id='tab-network' class='tab-content'>
  <h3>Unit Identity</h3>
  <label>Unit Name</label>
  <input type='text' id='unitname_inp' placeholder='e.g. Stage Left Panels'>
  <h3>WiFi Settings</h3>
  <label>Network Name (SSID)</label>
  <input type='text' id='ssid_inp' placeholder='Your WiFi name' autocomplete='off'>
  <label>Password</label>
  <input type='password' id='pass_inp' placeholder='Your WiFi password' autocomplete='off'>
  <h3>IP Settings</h3>
  <label>Static IP Address</label>
  <input type='text' id='ip_inp' placeholder='10.0.0.101'>
  <label>Gateway</label>
  <input type='text' id='gw_inp' placeholder='10.0.0.1'>
  <label>Subnet Mask</label>
  <input type='text' id='sn_inp' placeholder='255.255.255.0'>
  <h3>Config Backup / Restore</h3>
  <div class='info' style='margin-bottom:8px'>Download saves all settings to a file. Restore uploads a backup and restarts. WiFi password is included - keep the file private.</div>
  <button class='btn btn-log' onclick='downloadConfig()'>Download Config</button>
  <label style='margin-top:14px'>Restore from backup file</label>
  <input type='file' id='restore_file' accept='.json'>
  <button class='btn btn-ota' onclick='restoreConfig()'>Restore and Restart</button>
</div>

<!-- PANELS TAB -->
<div id='tab-panels' class='tab-content'>
  <h3>Panel Count</h3>
  <select id='panels_sel' onchange='updatePanelGrid()'>
    <option value='1'>1 panel  (400 pixels / 1200 channels)</option>
    <option value='2'>2 panels (800 pixels / 2400 channels)</option>
    <option value='3'>3 panels (1200 pixels / 3600 channels)</option>
    <option value='4'>4 panels (1600 pixels / 4800 channels)</option>
  </select>
  <label>Start Channel (must match xLights and FPP exactly)</label>
  <input type='text' id='startch_inp' placeholder='e.g. 16998'>
  <h3>Panel Grid Layout</h3>
  <div class='info'>
    Set <b>Columns</b> to arrange panels as a grid instead of a linear chain.<br>
    Enter the panel index for each grid position in the arrangement field.<br>
    The controller remaps pixels so xLights sees one combined display.
  </div>
  <label>Columns in grid (1 = linear chain)</label>
  <select id='panelcols_sel' onchange='updatePanelGrid()'>
    <option value='1'>1 - linear chain (default)</option>
    <option value='2'>2 - two columns</option>
    <option value='3'>3 - three columns</option>
    <option value='4'>4 - four columns</option>
  </select>
  <label style='margin-top:12px'>Panel Arrangement (front view, left-to-right, top-to-bottom)<br>
  <span style='font-size:11px;color:#888'>Panel indices (0-based) for each grid position, comma-separated</span></label>
  <input type='text' id='arrangement_inp' placeholder='e.g. 1,0,3,2' oninput='renderGrid()'>
  <div id='panel_grid' style='margin-top:10px;font-family:monospace'></div>
  <div class='info' style='margin-top:10px'>
    <b>xLights model:</b> <span id='xl_dims'>20 x 20</span>, Top Right, Horizontal, Don't Zig Zag<br>
    <b>FPP Channel Count:</b> <span id='xl_ch'>1200</span>
  </div>
</div>

<!-- XLIGHTS TAB -->
<div id='tab-xlights' class='tab-content'>
  <div class='info'>
    <b>Controllers Tab:</b> Vendor WLED, Model WLED, Varient Generic ESP32, Protocol DDP<br>
    IP: this unit's IP &nbsp;|&nbsp; Start Channel: match Panels tab &nbsp;|&nbsp; Channel Count: 1200 per panel<br><br>
    <b>Layout - Matrix model:</b><br>
    Width 20, Height 20, String Count 20, Lights/String 1<br>
    String Type RGB, Start Location Top Right, Direction Horizontal, Don't Zig Zag checked<br><br>
    <b>FPP Connect:</b> Tools &gt; FPP Connect, UDP Out All, check Sequences and Media, Upload
  </div>
</div>

<!-- DIAGNOSTICS TAB -->
<div id='tab-diag' class='tab-content'>
  <h3>DDP and Show Status</h3>
  <div class='diag'>
    Last DDP packet: <span id='lastddp'>never</span><br>
    DDP packets/sec: <span id='pps'>0</span> &nbsp;|&nbsp;
    DDP frames/sec: <span id='fps'>0</span><br>
    Total packets received: <span id='totalpkt'>0</span><br>
    Total renders: <span id='renders2'>0</span> &nbsp;|&nbsp;
    Display refreshes/sec: <span id='rps'>0</span><br>
    FPP data active: <span id='fppactive' class='warn'>no</span>
  </div>
  <h3>ESP32 Hardware</h3>
  <div class='stat-grid'>
    <span class='stat-label'>Free Heap</span>      <span class='stat-value' id='heap'>-</span>
    <span class='stat-label'>CPU Speed</span>       <span class='stat-value' id='cpu_mhz'>-</span>
    <span class='stat-label'>Flash Size</span>      <span class='stat-value' id='flash_kb'>-</span>
    <span class='stat-label'>MAC Address</span>     <span class='stat-value' id='mac'>-</span>
    <span class='stat-label'>WiFi Channel</span>    <span class='stat-value' id='channel'>-</span>
    <span class='stat-label'>WiFi Signal</span>
    <span>
      <span class='stat-value' id='rssi'>-</span> dBm
      <span class='rssi-bar'><span class='rssi-fill' id='rssi_bar' style='width:0%'></span></span>
    </span>
  </div>
  <h3>Debug Log</h3>
  <div class='row' style='margin-bottom:8px'>
    <input type='checkbox' id='debuglog_chk' onchange='toggleDebugLog()'>
    <label for='debuglog_chk'>Enable debug log (captures DDP events and render activity)</label>
  </div>
  <button class='btn btn-log' onclick='refreshLog()'>Refresh Log</button>
  <button class='btn btn-log' onclick='clearLog()' style='margin-left:6px'>Clear Log</button>
  <div class='logbox' id='logbox'>Log disabled. Enable above to start capturing events.</div>
  <h3>Device Controls</h3>
  <button class='btn btn-reboot' onclick='doReboot()'>Reboot</button>
  <button class='btn btn-reset'  onclick='doReset()' style='margin-left:6px'>Factory Reset</button>
</div>

<!-- OTA TAB -->
<div id='tab-ota' class='tab-content'>
  <h3>OTA Firmware Update</h3>
  <div class='info'>Arduino IDE: Sketch &gt; Export Compiled Binary (.bin), then select and upload below.</div>
  <label style='margin-top:12px'>Select firmware .bin file</label>
  <input type='file' id='ota_file' accept='.bin'>
  <button class='btn btn-ota' onclick='startOTA()'>Upload Firmware</button>
  <div id='ota-progress'>
    <div class='progress-bar'><div class='progress-fill' id='ota-bar'></div></div>
    <div id='ota-status'>Uploading...</div>
  </div>
</div>

<button class='btn btn-save' onclick='saveConfig()'>Save &amp; Restart</button>

<script>
var firstLoad = true;
var statusTimer = null;
var chaseOn = false;

function showTab(name, el) {
  document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('active');});
  document.querySelectorAll('.tab-content').forEach(function(t){t.classList.remove('active');});
  document.getElementById('tab-'+name).classList.add('active');
  el.classList.add('active');
}

function rssiToPercent(rssi) {
  if (rssi >= -50) return 100;
  if (rssi <= -100) return 0;
  return Math.round((rssi + 100) * 2);
}

function loadStatus() {
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    document.getElementById('pageTitle').textContent = d.name || 'ReclaimPanel';
    document.getElementById('uname').textContent     = d.name || 'Unnamed';
    document.getElementById('fwver').textContent     = d.fwver || '-';
    document.getElementById('connstat').textContent  = d.connected ? 'Connected' : 'AP Mode';
    document.getElementById('connstat').className    = d.connected ? 'ok' : 'warn';
    document.getElementById('ipaddr').textContent    = d.ip;
    document.getElementById('npanels').textContent   = d.panels;
    document.getElementById('startch').textContent   = d.startch;
    document.getElementById('totalch').textContent   = (d.panels * 1200) + ' ch';
    document.getElementById('brtpct').textContent    = d.brightness;
    document.getElementById('uptime').textContent    = formatUptime(d.uptime);
    document.getElementById('renders').textContent   = d.renders;
    document.getElementById('renders2').textContent  = d.renders;
    document.getElementById('rps').textContent       = d.rps || '0';
    document.getElementById('lastddp').textContent   = d.lastddp > 0 ? (d.lastddp + 'ms ago') : 'never';
    document.getElementById('pps').textContent       = d.pps;
    var panels = d.panels || 1;
    var fps = (d.pps > 0) ? Math.round(d.pps / panels) : 0;
    document.getElementById('fps').textContent       = fps;
    document.getElementById('totalpkt').textContent  = d.totalpkt;
    document.getElementById('heap').textContent      = d.heap + ' bytes';
    document.getElementById('cpu_mhz').textContent   = (d.cpu_mhz || '-') + ' MHz';
    document.getElementById('flash_kb').textContent  = d.flash_kb ? (Math.round(d.flash_kb / 1024)) + ' MB' : '-';
    document.getElementById('mac').textContent       = d.mac || '-';
    document.getElementById('channel').textContent   = d.channel || '-';
    var rssi = d.rssi || -100;
    document.getElementById('rssi').textContent      = rssi;
    var pct = rssiToPercent(rssi);
    var bar = document.getElementById('rssi_bar');
    bar.style.width      = pct + '%';
    bar.style.background = pct > 60 ? '#00cc00' : pct > 30 ? '#ffaa00' : '#ff4444';
    var fppActive = d.lastddp > 0 && d.lastddp < 5000;
    document.getElementById('fppactive').textContent = fppActive ? 'YES' : 'no';
    document.getElementById('fppactive').className   = fppActive ? 'ok' : 'warn';
    document.getElementById('modespan').innerHTML    = d.pwm
      ? "<span class='badge pwm-on'>BCM ON</span>"
      : "<span class='badge pwm-off'>Binary</span>";
    if (firstLoad) {
      firstLoad = false;
      document.getElementById('ssid_inp').value        = d.ssid || '';
      document.getElementById('ip_inp').value          = d.static_ip || '';
      document.getElementById('gw_inp').value          = d.gateway || '';
      document.getElementById('sn_inp').value          = d.subnet || '';
      document.getElementById('panels_sel').value      = d.panels || '1';
      document.getElementById('startch_inp').value     = d.startch || '';
      document.getElementById('pwm_chk').checked       = d.pwm;
      document.getElementById('brt_slider').value      = d.brightness || '100';
      document.getElementById('brt_disp').textContent  = d.brightness || '100';
      document.getElementById('unitname_inp').value    = d.name || '';
      document.getElementById('fliprows_chk').checked  = d.fliprows;
      document.getElementById('flipcols_chk').checked  = d.flipcols;
      document.getElementById('panelcols_sel').value   = d.panelcols || '1';
      document.getElementById('arrangement_inp').value = d.arrangement || '0,1,2,3';
      document.getElementById('debuglog_chk').checked  = d.debug_log;
      var bcu = d.bcm_base_us || 500;
      document.getElementById('bcm_slider').value      = bcu;
      updateBCMDisplay(bcu);
      updatePanelGrid();
      checkForUpdate(d.fwver);
    }
  }).catch(function(){});
}

function updatePanelGrid() {
  var n    = parseInt(document.getElementById('panels_sel').value) || 1;
  var cols = parseInt(document.getElementById('panelcols_sel').value) || 1;
  var rows = Math.ceil(n / cols);
  document.getElementById('xl_dims').textContent = (cols * 20) + ' x ' + (rows * 20);
  document.getElementById('xl_ch').textContent   = (n * 1200);
  var arr = document.getElementById('arrangement_inp').value.trim();
  var parts = arr.split(',').map(function(x){return x.trim();}).filter(function(x){return x!='';});
  if (parts.length !== n) {
    var def = []; for(var i=0;i<n;i++) def.push(i);
    document.getElementById('arrangement_inp').value = def.join(',');
  }
  renderGrid();
}

function renderGrid() {
  var n    = parseInt(document.getElementById('panels_sel').value) || 1;
  var cols = parseInt(document.getElementById('panelcols_sel').value) || 1;
  var rows = Math.ceil(n / cols);
  var arr  = document.getElementById('arrangement_inp').value.split(',');
  var html = '<table style="border-collapse:collapse;margin-top:6px">';
  html += '<tr><td colspan="'+cols+'" style="color:#888;font-size:11px;padding-bottom:4px">FRONT VIEW</td></tr>';
  for(var r=0;r<rows;r++){
    html+='<tr>';
    for(var c=0;c<cols;c++){
      var pos = r*cols+c;
      var idx = arr[pos] !== undefined ? arr[pos].trim() : '?';
      html+='<td style="border:1px solid #446;padding:10px 18px;text-align:center;background:#1a2a1a;color:#8f8">';
      html+='idx '+idx+'<br><span style="font-size:10px;color:#666">ch '+(parseInt(idx)*1200+1)+'-'+((parseInt(idx)+1)*1200)+'</span>';
      html+='</td>';
    }
    html+='</tr>';
  }
  html+='</table>';
  document.getElementById('panel_grid').innerHTML=html;
}

function formatUptime(ms) {
  var s=Math.floor(ms/1000),m=Math.floor(s/60);s%=60;
  var h=Math.floor(m/60);m%=60;
  return h+'h '+m+'m '+s+'s';
}

function refreshLog() {
  fetch('/log').then(function(r){return r.text();}).then(function(t){
    document.getElementById('logbox').textContent = t || '(empty)';
  }).catch(function(){ document.getElementById('logbox').textContent = 'Error fetching log.'; });
}

function clearLog() {
  var on = document.getElementById('debuglog_chk').checked;
  fetch('/debuglog?v=' + (on ? '1' : '0'))
    .then(function(){ document.getElementById('logbox').textContent = '(cleared)'; });
}

function toggleDebugLog() {
  var on = document.getElementById('debuglog_chk').checked;
  fetch('/debuglog?v=' + (on ? '1' : '0'));
  document.getElementById('logbox').textContent = on ? 'Logging enabled. Events will appear here.' : 'Log disabled.';
}

function test(c)          { chaseOn=false; fetch('/chase?v=0'); document.getElementById('chase_btn').textContent='Chase Test'; fetch('/test?color='+c); }
function toggleChase()    { chaseOn=!chaseOn; fetch('/chase?v='+(chaseOn?'1':'0')); document.getElementById('chase_btn').textContent=chaseOn?'Stop Chase':'Chase Test'; }
function togglePWM()      { fetch('/pwm?v='+(document.getElementById('pwm_chk').checked?'1':'0')); }
function setBrightness(v) { fetch('/brightness?v='+v); }
function updateBCMDisplay(v) {
  document.getElementById('bcm_disp').textContent = v;
  document.getElementById('bcm_hz').textContent   = Math.round(1000000 / (v * 15));
}
function setBCMSpeed(v) { fetch('/bcmspeed?v='+v); }
function setOrientation() { fetch('/orientation?fliprows='+(document.getElementById('fliprows_chk').checked?'1':'0')+'&flipcols='+(document.getElementById('flipcols_chk').checked?'1':'0')); }
function doReboot()       { if(confirm('Reboot the panel controller?')) { fetch('/reboot'); setTimeout(function(){ window.location.reload(); },4000); } }
function doReset()        { if(confirm('Factory reset? All settings will be cleared.')) fetch('/reset'); }

function checkForUpdate(ver) {
  fetch('https://api.github.com/repos/thedrapercode/ReclaimPanel/releases/latest')
    .then(function(r){return r.json();})
    .then(function(d){
      var latest=(d.tag_name||'').replace(/^v/i,'');
      if(latest && latest!==ver){
        document.getElementById('update-ver').textContent=d.tag_name;
        document.getElementById('update-banner').style.display='block';
      }
    }).catch(function(){});
}

function downloadConfig(){ window.location.href='/config'; }

function restoreConfig(){
  var file=document.getElementById('restore_file').files[0];
  if(!file){alert('Select a config .json file first');return;}
  if(!confirm('Restore settings from '+file.name+'? The unit will restart.'))return;
  var reader=new FileReader();
  reader.onload=function(e){
    fetch('/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:e.target.result})
      .then(function(r){return r.text();})
      .then(function(){alert('Restored! Restarting...');})
      .catch(function(){alert('Restore may have succeeded - unit is restarting.');});
  };
  reader.readAsText(file);
}

function startOTA() {
  var file=document.getElementById('ota_file').files[0];
  if(!file){alert('Select a .bin file first');return;}
  if(!confirm('Upload '+file.name+'?'))return;
  document.getElementById('ota-progress').style.display='block';
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);document.getElementById('ota-bar').style.width=p+'%';document.getElementById('ota-status').textContent='Uploading: '+p+'%';}};
  xhr.onload=function(){if(xhr.status===200){document.getElementById('ota-status').textContent='Success! Restarting...';document.getElementById('ota-bar').style.background='#00cc00';setTimeout(function(){window.location.reload();},6000);}else{document.getElementById('ota-status').textContent='Failed: '+xhr.responseText;document.getElementById('ota-bar').style.background='#cc0000';}};
  xhr.onerror=function(){document.getElementById('ota-status').textContent='Upload error';document.getElementById('ota-bar').style.background='#cc0000';};
  var fd=new FormData();fd.append('firmware',file);
  xhr.open('POST','/ota');xhr.send(fd);
}

function saveConfig() {
  if(statusTimer){clearInterval(statusTimer);statusTimer=null;}
  var ssid=document.getElementById('ssid_inp').value.trim();
  var ip=document.getElementById('ip_inp').value.trim();
  if(!ssid){alert('Please enter WiFi network name.');statusTimer=setInterval(loadStatus,3000);return;}
  if(!ip){alert('Please enter static IP address.');statusTimer=setInterval(loadStatus,3000);return;}
  var data={
    ssid:ssid,
    pass:document.getElementById('pass_inp').value,
    ip:ip,
    gateway:document.getElementById('gw_inp').value.trim(),
    subnet:document.getElementById('sn_inp').value.trim(),
    name:document.getElementById('unitname_inp').value.trim(),
    panels:document.getElementById('panels_sel').value,
    startch:document.getElementById('startch_inp').value.trim(),
    pwm:document.getElementById('pwm_chk').checked?'1':'0',
    brightness:document.getElementById('brt_slider').value,
    fliprows:document.getElementById('fliprows_chk').checked?'1':'0',
    flipcols:document.getElementById('flipcols_chk').checked?'1':'0',
    panelcols:document.getElementById('panelcols_sel').value,
    arrangement:document.getElementById('arrangement_inp').value.trim(),
    bcmbaseus:document.getElementById('bcm_slider').value
  };
  fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
    .then(function(r){return r.text();}).then(function(){alert('Saved! Restarting...');})
    .catch(function(){alert('Save may have succeeded - unit is restarting.');});
}

loadStatus();
statusTimer=setInterval(loadStatus,3000);
</script>
</body>
</html>
)rawhtml";

// ============================================================
// WEB HANDLERS
// ============================================================
void handleRoot() { server.send(200, "text/html", HTML_PAGE); }

void handleStatus() {
  unsigned long now     = millis();
  unsigned long lastddp = (lastDDPPacket > 0) ? (now - lastDDPPacket) : 0;
  String json = "{";
  json += "\"connected\":"   + String(WiFi.status()==WL_CONNECTED?"true":"false") + ",";
  json += "\"ip\":\""        + (apMode?String("192.168.4.1"):WiFi.localIP().toString()) + "\",";
  json += "\"ssid\":\""      + String(wifiSSID)    + "\",";
  json += "\"static_ip\":\"" + String(staticIPStr) + "\",";
  json += "\"gateway\":\""   + String(gatewayStr)  + "\",";
  json += "\"subnet\":\""    + String(subnetStr)   + "\",";
  json += "\"name\":\""      + String(unitName)    + "\",";
  json += "\"fwver\":\""     + String(FW_VERSION)  + "\",";
  json += "\"panels\":"      + String(numPanels)   + ",";
  json += "\"startch\":"     + String(startChannel)+ ",";
  json += "\"brightness\":"  + String(brightness)  + ",";
  json += "\"pwmlevels\":"   + String(pwmLevels)   + ",";
  json += "\"pwm\":"         + String(pwmMode?"true":"false") + ",";
  json += "\"fliprows\":"    + String(flipRows?"true":"false") + ",";
  json += "\"flipcols\":"      + String(flipCols?"true":"false") + ",";
  json += "\"panelcols\":"     + String(panelCols) + ",";
  json += "\"arrangement\":\"" + String(panelArrangement) + "\",";
  json += "\"uptime\":"        + String(now - sketchStartTime) + ",";
  json += "\"lastddp\":"     + String(lastddp) + ",";
  json += "\"pps\":"         + String(ddpPacketsPerSec) + ",";
  json += "\"totalpkt\":"    + String(ddpPacketCount) + ",";
  json += "\"renders\":"     + String(renderCount) + ",";
  json += "\"heap\":"        + String(ESP.getFreeHeap()) + ",";
  json += "\"rssi\":"        + String(WiFi.RSSI()) + ",";
  json += "\"rps\":"         + String(rendersPerSec) + ",";
  json += "\"mac\":\""       + WiFi.macAddress() + "\",";
  json += "\"channel\":"     + String(WiFi.channel()) + ",";
  json += "\"cpu_mhz\":"     + String(ESP.getCpuFreqMHz()) + ",";
  json += "\"flash_kb\":"    + String(ESP.getFlashChipSize() / 1024) + ",";
  json += "\"debug_log\":"   + String(debugLog ? "true" : "false") + ",";
  json += "\"bcm_base_us\":" + String(bcmBaseUs);
  json += "}";
  server.send(200, "application/json", json);
}

void handleTest() {
  String c = server.arg("color");
  chaseActive = false;
  if      (c=="red")     setSolid(255,0,0);
  else if (c=="green")   setSolid(0,255,0);
  else if (c=="blue")    setSolid(0,0,255);
  else if (c=="cyan")    setSolid(0,255,255);
  else if (c=="orange")  setSolid(255,100,0);
  else if (c=="magenta") setSolid(255,0,255);
  else if (c=="white")   setSolid(255,255,255);
  else if (c=="off")     setSolid(0,0,0);
  else if (c=="scare")   { jumpScareActive=true; jumpScareStart=millis(); setSolid(255,255,255); }
  server.send(200,"text/plain","ok");
}

void handleChase() {
  chaseActive = (server.arg("v") == "1");
  if (!chaseActive) setSolid(0,0,0);
  server.send(200,"text/plain","ok");
}

void handlePWM() {
  pwmMode = (server.arg("v")=="1");
  prefs.begin("rclmpanel",false); prefs.putBool("pwm",pwmMode); prefs.end();
  server.send(200,"text/plain","ok");
}

void handlePWMLevels() {
  pwmLevels = constrain(server.arg("v").toInt(), 4, 16);
  prefs.begin("rclmpanel",false); prefs.putInt("pwmlevels",pwmLevels); prefs.end();
  server.send(200,"text/plain","ok");
}

void handleBrightness() {
  brightness = constrain(server.arg("v").toInt(), 1, 100);
  // No flash write here - Save button persists brightness to avoid flash wear on every drag
  server.send(200,"text/plain","ok");
}

void handleOrientation() {
  flipRows = (server.arg("fliprows")=="1");
  flipCols = (server.arg("flipcols")=="1");
  prefs.begin("rclmpanel",false);
  prefs.putBool("fliprows",flipRows);
  prefs.putBool("flipcols",flipCols);
  prefs.end();
  buildPixelMap();
  server.send(200,"text/plain","ok");
}

void handleReboot() {
  server.send(200,"text/plain","rebooting");
  delay(500); ESP.restart();
}

void handleReset() {
  prefs.begin("rclmpanel",false); prefs.clear(); prefs.end();
  server.send(200,"text/plain","reset");
  delay(500); ESP.restart();
}

void handleLog() {
  String out = "";
  if (logCount == 0) { server.send(200, "text/plain", "Log empty"); return; }
  int start = (logCount < LOG_LINES) ? 0 : logHead;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_LINES;
    out += String(logBuf[idx]) + "\n";
  }
  server.send(200, "text/plain", out);
}

void handleDebugLog() {
  debugLog = (server.arg("v") == "1");
  if (!debugLog) { logHead = 0; logCount = 0; }
  server.send(200, "text/plain", "ok");
}

void handleBCMSpeed() {
  bcmBaseUs = constrain(server.arg("v").toInt(), 100, 5000);
  server.send(200, "text/plain", "ok");
}

void handleOTA() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    setSolid(0,0,80);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) setSolid(0,80,0);
    else { Update.printError(Serial); setSolid(80,0,0); }
  }
}

void handleOTADone() {
  if (Update.hasError()) server.send(500,"text/plain","Update failed");
  else { server.send(200,"text/plain","Update successful - restarting"); delay(1000); ESP.restart(); }
}

void handleSave() {
  if (!server.hasArg("plain")) { server.send(400,"text/plain","no data"); return; }
  String body = server.arg("plain");
  auto extractStr = [&](String key) -> String {
    String search = "\""+key+"\":\"";
    int idx = body.indexOf(search);
    if (idx < 0) return "";
    idx += search.length();
    return body.substring(idx, body.indexOf("\"",idx));
  };
  // Only overwrite WiFi credentials if non-empty
  String newSSID = extractStr("ssid");
  String newPass = extractStr("pass");
  if (newSSID.length() > 0) newSSID.toCharArray(wifiSSID, sizeof(wifiSSID));
  if (newPass.length() > 0) newPass.toCharArray(wifiPass, sizeof(wifiPass));
  extractStr("ip").toCharArray(staticIPStr,     sizeof(staticIPStr));
  extractStr("gateway").toCharArray(gatewayStr, sizeof(gatewayStr));
  extractStr("subnet").toCharArray(subnetStr,   sizeof(subnetStr));
  extractStr("name").toCharArray(unitName,      sizeof(unitName));
  startChannel = extractStr("startch").toInt();
  numPanels    = extractStr("panels").toInt();
  brightness   = extractStr("brightness").toInt();
  pwmMode      = (extractStr("pwm") == "1");
  flipRows     = (extractStr("fliprows") == "1");
  flipCols     = (extractStr("flipcols") == "1");
  panelCols    = extractStr("panelcols").toInt();
  String arr   = extractStr("arrangement");
  if (arr.length() > 0) arr.toCharArray(panelArrangement, sizeof(panelArrangement));
  if (numPanels < 1 || numPanels > 4) numPanels = 1;
  if (panelCols < 1 || panelCols > 4) panelCols = 1;
  brightness = constrain(brightness, 1, 100);
  bcmBaseUs  = constrain(extractStr("bcmbaseus").toInt(), 100, 3000);
  if (bcmBaseUs == 0) bcmBaseUs = 500;
  saveSettings();
  server.send(200,"text/plain","saved");
  delay(1000); ESP.restart();
}

void handleConfig() {
  String json = "{";
  json += "\"name\":\""        + String(unitName)       + "\",";
  json += "\"ssid\":\""        + String(wifiSSID)       + "\",";
  json += "\"ip\":\""          + String(staticIPStr)    + "\",";
  json += "\"gateway\":\""     + String(gatewayStr)     + "\",";
  json += "\"subnet\":\""      + String(subnetStr)      + "\",";
  json += "\"panels\":\""      + String(numPanels)      + "\",";
  json += "\"startch\":\""     + String(startChannel)   + "\",";
  json += "\"brightness\":\""  + String(brightness)     + "\",";
  json += "\"pwm\":\""         + String(pwmMode  ? "1" : "0") + "\",";
  json += "\"fliprows\":\""    + String(flipRows  ? "1" : "0") + "\",";
  json += "\"flipcols\":\""    + String(flipCols  ? "1" : "0") + "\",";
  json += "\"panelcols\":\""   + String(panelCols)      + "\",";
  json += "\"arrangement\":\"" + String(panelArrangement) + "\",";
  json += "\"bcmbaseus\":\""   + String(bcmBaseUs)      + "\"";
  json += "}";
  server.sendHeader("Content-Disposition", "attachment; filename=\"reclampanel-config.json\"");
  server.send(200, "application/json", json);
}

void handleRestore() {
  if (!server.hasArg("plain")) { server.send(400,"text/plain","no data"); return; }
  String body = server.arg("plain");
  auto extractStr = [&](String key) -> String {
    String search = "\""+key+"\":\"";
    int idx = body.indexOf(search);
    if (idx < 0) return "";
    idx += search.length();
    return body.substring(idx, body.indexOf("\"",idx));
  };
  String newSSID = extractStr("ssid");
  if (newSSID.length() > 0) newSSID.toCharArray(wifiSSID, sizeof(wifiSSID));
  extractStr("ip").toCharArray(staticIPStr,     sizeof(staticIPStr));
  extractStr("gateway").toCharArray(gatewayStr, sizeof(gatewayStr));
  extractStr("subnet").toCharArray(subnetStr,   sizeof(subnetStr));
  extractStr("name").toCharArray(unitName,      sizeof(unitName));
  startChannel = extractStr("startch").toInt();
  numPanels    = extractStr("panels").toInt();
  brightness   = extractStr("brightness").toInt();
  pwmMode      = (extractStr("pwm")      == "1");
  flipRows     = (extractStr("fliprows") == "1");
  flipCols     = (extractStr("flipcols") == "1");
  panelCols    = extractStr("panelcols").toInt();
  String arr   = extractStr("arrangement");
  if (arr.length() > 0) arr.toCharArray(panelArrangement, sizeof(panelArrangement));
  if (numPanels < 1 || numPanels > 4) numPanels = 1;
  if (panelCols < 1 || panelCols > 4) panelCols = 1;
  brightness = constrain(brightness, 1, 100);
  bcmBaseUs  = constrain(extractStr("bcmbaseus").toInt(), 100, 3000);
  if (bcmBaseUs == 0) bcmBaseUs = 500;
  saveSettings();
  server.send(200,"text/plain","restored");
  delay(1000); ESP.restart();
}

void setupWebServer() {
  server.on("/",            handleRoot);
  server.on("/status",      handleStatus);
  server.on("/test",        handleTest);
  server.on("/chase",       handleChase);
  server.on("/pwm",         handlePWM);
  server.on("/pwmlevels",   handlePWMLevels);
  server.on("/brightness",  handleBrightness);
  server.on("/orientation", handleOrientation);
  server.on("/reboot",      handleReboot);
  server.on("/reset",       handleReset);
  server.on("/log",         handleLog);
  server.on("/debuglog",    handleDebugLog);
  server.on("/bcmspeed",    handleBCMSpeed);
  server.on("/config",      handleConfig);
  server.on("/restore",     HTTP_POST, handleRestore);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/ota",         HTTP_POST, handleOTADone, handleOTA);
  server.begin();
}

// ============================================================
// DDP
// ============================================================
void processDDP(int len) {
  if (len < DDP_HEADER_SIZE) return;
  uint8_t  flags   = ddpBuffer[0];
  if (flags & (DDP_FLAGS_QUERY | DDP_FLAGS_REPLY)) return;
  uint32_t offset  = ((uint32_t)ddpBuffer[4]<<24)|((uint32_t)ddpBuffer[5]<<16)|
                     ((uint32_t)ddpBuffer[6]<<8) | ddpBuffer[7];
  uint16_t dataLen = ((uint16_t)ddpBuffer[8]<<8) | ddpBuffer[9];
  int totalBytes   = numPanels * CHANNELS_PER_PANEL;

  // Subtract start channel offset so FPP channel 15799 maps to pixelBuffer[0]
  uint32_t localOffset = (offset >= (uint32_t)(startChannel - 1))
                         ? offset - (uint32_t)(startChannel - 1)
                         : offset;

  if ((int)localOffset >= totalBytes) return;  // silently drop out-of-range packets

  for (int i = 0; i < (int)dataLen && (int)(localOffset+i) < totalBytes && (DDP_HEADER_SIZE+i) < len; i++)
    pixelBuffer[localOffset+i] = ddpBuffer[DDP_HEADER_SIZE+i];

  lastDDPPacket = millis();
  ddpPacketCount++;

  if (flags & DDP_FLAGS_PUSH) newFrameData = true;
}

// ============================================================
// WIFI
// ============================================================
void setupWiFi() {
  if (!configured || strlen(wifiSSID)==0) {
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    apMode = true;
    setSolid(0,0,80);
    return;
  }
  IPAddress sip, gw, sn;
  sip.fromString(staticIPStr); gw.fromString(gatewayStr); sn.fromString(subnetStr);
  WiFi.config(sip, gw, sn);
  WiFi.begin(wifiSSID, wifiPass);
  int attempts=0;
  while (WiFi.status()!=WL_CONNECTED && attempts<40) {
    setSolid(80,0,0); delay(250); setSolid(0,0,0); delay(250);
    attempts++;
  }
  if (WiFi.status()==WL_CONNECTED) {
    for (int i=0;i<3;i++){setSolid(0,80,0);delay(200);setSolid(0,0,0);delay(200);}
    apMode=false;
    char mdnsName[32];
    strncpy(mdnsName, unitName, sizeof(mdnsName)-1);
    mdnsName[sizeof(mdnsName)-1] = 0;
    for (int i=0; mdnsName[i]; i++) {
      if (isalnum((unsigned char)mdnsName[i])) mdnsName[i] = tolower((unsigned char)mdnsName[i]);
      else mdnsName[i] = '-';
    }
    if (MDNS.begin(mdnsName)) MDNS.addService("http", "tcp", 80);
  } else {
    WiFi.softAP(AP_SSID, AP_PASSWORD); apMode=true;
    for (int i=0;i<3;i++){setSolid(0,0,80);delay(200);setSolid(0,0,0);delay(200);}
  }
}

// ============================================================
// SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  sketchStartTime = millis();
  const int allPins[]={DR1,DG1,DB1,DR2,DG2,DB2,DR3,DG3,DB3,DR4,DG4,DB4,DR5,DG5,DB5,CLK,LAT,OE};
  for (int i=0;i<18;i++){pinMode(allPins[i],OUTPUT);digitalWrite(allPins[i],LOW);}
  digitalWrite(OE,LOW);
  pinMode(TRIGGER_PIN,INPUT);
  memset(pixelBuffer,0,sizeof(pixelBuffer));
  loadSettings();
  buildPixelMap();
  setupWiFi();
  setupWebServer();
  if (!apMode) {
    udp.begin(DDP_PORT);
    Serial.printf("ReclaimPanel %s ready\n", FW_VERSION);
    Serial.printf("Unit: %s  IP: %s\n", unitName, WiFi.localIP().toString().c_str());
    Serial.printf("Panels: %d  StartCh: %d  Channels: %d\n", numPanels, startChannel, numPanels*CHANNELS_PER_PANEL);
    Serial.printf("Mode: %s  Brightness: %d  Heap: %d\n", pwmMode?"PWM":"Binary", brightness, ESP.getFreeHeap());
  }
}

void loop() {
  // Drain ALL pending UDP packets first - before server.handleClient() can block.
  // With 4 panels FPP sends 4 packets per frame; reading only one per loop iteration
  // caused the socket buffer to overflow and drop packets 2-4 (including push flag).
  if (!apMode) {
    int ps;
    while ((ps = udp.parsePacket()) > 0) {
      int len = udp.read(ddpBuffer, sizeof(ddpBuffer));
      if (len > 0) processDDP(len);
    }
  }

  server.handleClient();

  // Heartbeat every second - proves loop() is running and not frozen
  if (millis() - heartbeatTimer >= 1000) {
    heartbeatTimer   = millis();
    ddpPacketsPerSec = ddpPacketCount - ddpCountLast;
    ddpCountLast     = ddpPacketCount;
    rendersPerSec    = renderCount - renderCountLast;
    renderCountLast  = renderCount;
    ddpSecTimer      = millis();
    Serial.printf("ALIVE t=%lus renders=%u rps=%u pps=%u heap=%u ddp_age=%lums\n",
      millis()/1000, renderCount, rendersPerSec, ddpPacketsPerSec, ESP.getFreeHeap(),
      lastDDPPacket > 0 ? (millis()-lastDDPPacket) : 0);
  }

  // Jump scare
  if (digitalRead(TRIGGER_PIN)==HIGH && !jumpScareActive) {
    jumpScareActive=true; jumpScareStart=millis(); setSolid(255,255,255);
  }
  if (jumpScareActive && (millis()-jumpScareStart>JUMPSCARE_MS)) {
    jumpScareActive=false; setSolid(0,0,0);
  }

  // Chase effect - pixel walker across all panels
  if (chaseActive && (millis()-chaseTimer >= 100)) {
    chaseTimer = millis();
    int totalPixels = numPanels * PIXELS_PER_PANEL;
    memset(pixelBuffer, 0, totalPixels * 3);
    for (int i=0;i<3;i++) {
      int px = ((chasePixel+i) % totalPixels) * 3;
      pixelBuffer[px] = 255; // red
    }
    chasePixel = (chasePixel+1) % totalPixels;
    newFrameData = true;
  }

  // BCM: cycle bit planes continuously - no frame gate needed, pixelBuffer is read each cycle
  if (pwmMode) {
    static uint8_t  bcmBit     = 0;
    static uint32_t bcmHoldEnd = 0;
    if ((int32_t)(micros() - bcmHoldEnd) >= 0) {
      writeAllPanelsBCM(bcmBit);
      bcmHoldEnd = micros() + ((uint32_t)bcmBaseUs << bcmBit);
      bcmBit = (bcmBit + 1) % BCM_BITS;
      lastRender = millis();
      renderCount++;
    }
  }

  // Binary mode: render only when new DDP frame arrives
  if (!pwmMode && newFrameData && (millis()-lastRender >= MIN_RENDER_INTERVAL)) {
    newFrameData = false;
    renderPanels();
  }

  yield();
}
