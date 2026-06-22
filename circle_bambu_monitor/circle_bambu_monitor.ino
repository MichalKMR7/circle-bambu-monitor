#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <lvgl.h>
#include <time.h>
#include <stdlib.h>

using namespace websockets;

/**********************************************************
 * FIRMWARE VERSION
 **********************************************************/
#define FW_VERSION "0.11.15"
#define FW_DISPLAY_VERSION "0.11.15"

/**********************************************************
 * DISPLAY PIN CONFIGURATION
 **********************************************************/
#define TFT_CS   7
#define TFT_DC   2
#define TFT_RST  3
#define TFT_SCLK 4
#define TFT_MOSI 6

SPIClass spi(FSPI);
Adafruit_GC9A01A tft(&spi, TFT_DC, TFT_CS, TFT_RST);

static lv_display_t *disp;
static lv_color_t draw_buf[240 * 40];

/**********************************************************
 * WEB SERVER / STORAGE / WEBSOCKET OBJECTS
 **********************************************************/
WebServer server(80);
Preferences prefs;
WebsocketsClient wsClient;

/**********************************************************
 * CONFIGURATION STRUCTURE
 *
 * Ukládá:
 * - WiFi údaje
 * - Home Assistant URL
 * - Long Lived Token
 * - entity tiskárny
 **********************************************************/
struct Config {
  String wifiSsid;
  String wifiPass;
  String haUrl;
  String haToken;
  String otaPass;
  String tzName;
  String tzPosix;

  String entityPhase;
  String entityStatus;
  String entityProgress;
  String entityFilament;
  String entityFinishTime;
  String entityNozzleTemp;
  String entityBedTemp;
  String entityCurrentLayer;
  String entityTotalLayers;
  String entityFilamentWeight;
};

/**********************************************************
 * PRINTER DATA STRUCTURE
 *
 * Aktuální hodnoty z Home Assistantu.
 * Plní se jednorázově přes REST při startu
 * a potom živě přes WebSocket.
 **********************************************************/
struct PrinterData {
  String phase = "";
  String status = "";
  String filament = "";
  String finishTime = "";

  float progress = NAN;
  float nozzleTemp = NAN;
  float bedTemp = NAN;
  float currentLayer = NAN;
  float totalLayers = NAN;
  float filamentWeight = NAN;
};

Config cfg;
PrinterData data;

/**********************************************************
 * ACCESS POINT CONFIGURATION
 *
 * AP se spustí pouze pokud se zařízení nepřipojí
 * k uložené domácí WiFi.
 **********************************************************/
const char* apSsid = "CBM";
const char* apPass = "circlebambu";

/**********************************************************
 * TIMERS AND STATE FLAGS
 **********************************************************/
unsigned long lastTick = 0;
unsigned long lastUiUpdate = 0;
unsigned long lastWsReconnectAttempt = 0;

const unsigned long uiUpdateInterval = 200;
const unsigned long wsReconnectInterval = 5000;
const unsigned long initialStateTotalTimeout = 7000;
const int initialStateRequestTimeout = 900;

const char* defaultTzName = "UTC+1 - Czech Republic, Germany, France, Italy";
const char* defaultTzPosix = "CET-1CEST,M3.5.0,M10.5.0/3";

bool portalMode = false;
bool wsAuthenticated = false;
bool wsSubscribed = false;
bool firstHaReadDone = false;
bool uiDirty = true;
bool haAuthInvalid = false;
bool wsConnectFailed = false;
bool otaReady = false;
bool otaActive = false;

int wsNextId = 1;
int wsSubscribeId = 0;
int otaProgress = 0;

/**********************************************************
 * FINISH / PROGRESS STATE
 **********************************************************/
unsigned long finishDisplayUntilMs = 0;
bool finishSeen = false;
bool printWasActive = false;

float displayedArcValue = 0;
bool arcSmoothWasActive = false;

static lv_point_precise_t nozzleIconPoints[] = {
  {2, 1}, {12, 1}, {10, 6}, {8, 9}, {7, 13}, {6, 9}, {4, 6}, {2, 1}
};

static lv_point_precise_t bedIconPoints[] = {
  {1, 3}, {15, 3}, {15, 8}, {1, 8}, {1, 3}, {4, 8}, {4, 11}, {12, 11}, {12, 8}
};

/**********************************************************
 * LVGL OBJECTS
 **********************************************************/
lv_obj_t *progress_arc;
lv_obj_t *status_label;
lv_obj_t *center_label;
lv_obj_t *preparing_label;
lv_obj_t *bottom_label;
lv_obj_t *temps_label;
lv_obj_t *temp_row;
lv_obj_t *nozzle_icon;
lv_obj_t *bed_icon;
lv_obj_t *nozzle_temp_label;
lv_obj_t *bed_temp_label;
lv_obj_t *detail_label;

void serviceDisplayDuringSetup(uint16_t waitMs = 2);
void showBootMessage(String title, String detail = "");
void normalizeTimeZoneConfig();

/**********************************************************
 * TIME HELPER
 *
 * Vrací aktuální čas v sekundách.
 * Pokud ještě není synchronizovaný NTP čas,
 * použije millis().
 **********************************************************/
uint32_t currentSeconds() {
  time_t realNow = time(nullptr);
  return realNow > 100000 ? (uint32_t)realNow : millis() / 1000;
}

bool finishDisplayActive() {
  return finishDisplayUntilMs > 0 && (long)(finishDisplayUntilMs - millis()) >= 0;
}

/**********************************************************
 * LVGL DISPLAY FLUSH CALLBACK
 *
 * Přenáší vykreslený LVGL buffer na GC9A01A displej.
 **********************************************************/
void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
  int32_t w = area->x2 - area->x1 + 1;
  int32_t h = area->y2 - area->y1 + 1;

  tft.drawRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(display);
}

/**********************************************************
 * CONFIG LOAD
 *
 * Načte konfiguraci z NVS flash paměti ESP32.
 * Data přežijí restart i nahrání nového firmware,
 * pokud se nesmaže celá flash.
 **********************************************************/
void loadConfig() {
  prefs.begin("cbm", true);

  cfg.wifiSsid = prefs.getString("wifi_ssid", "");
  cfg.wifiPass = prefs.getString("wifi_pass", "");
  cfg.haUrl = prefs.getString("ha_url", "");
  cfg.haToken = prefs.getString("ha_token", "");
  cfg.otaPass = prefs.getString("ota_pass", "");
  cfg.tzName = prefs.getString("tz_name", defaultTzName);
  cfg.tzPosix = prefs.getString("tz_posix", defaultTzPosix);

  cfg.entityPhase = prefs.getString("ent_phase", "");
  cfg.entityStatus = prefs.getString("ent_status", "");
  cfg.entityProgress = prefs.getString("ent_progress", "");
  cfg.entityFilament = prefs.getString("ent_filament", "");
  cfg.entityFinishTime = prefs.getString("ent_finish", "");
  cfg.entityNozzleTemp = prefs.getString("ent_nozzle", "");
  cfg.entityBedTemp = prefs.getString("ent_bed", "");
  cfg.entityCurrentLayer = prefs.getString("ent_layer", "");
  cfg.entityTotalLayers = prefs.getString("ent_layers", "");
  cfg.entityFilamentWeight = prefs.getString("ent_weight", "");

  prefs.end();

  normalizeTimeZoneConfig();
}

/**********************************************************
 * CONFIG SAVE
 *
 * Uloží konfiguraci do NVS flash paměti ESP32.
 **********************************************************/
void saveConfig() {
  prefs.begin("cbm", false);

  prefs.putString("wifi_ssid", cfg.wifiSsid);
  prefs.putString("wifi_pass", cfg.wifiPass);
  prefs.putString("ha_url", cfg.haUrl);
  prefs.putString("ha_token", cfg.haToken);
  prefs.putString("ota_pass", cfg.otaPass);
  prefs.putString("tz_name", cfg.tzName);
  prefs.putString("tz_posix", cfg.tzPosix);

  prefs.putString("ent_phase", cfg.entityPhase);
  prefs.putString("ent_status", cfg.entityStatus);
  prefs.putString("ent_progress", cfg.entityProgress);
  prefs.putString("ent_filament", cfg.entityFilament);
  prefs.putString("ent_finish", cfg.entityFinishTime);
  prefs.putString("ent_nozzle", cfg.entityNozzleTemp);
  prefs.putString("ent_bed", cfg.entityBedTemp);
  prefs.putString("ent_layer", cfg.entityCurrentLayer);
  prefs.putString("ent_layers", cfg.entityTotalLayers);
  prefs.putString("ent_weight", cfg.entityFilamentWeight);

  prefs.end();
}

/**********************************************************
 * WEB CONFIGURATION PAGE
 *
 * Hlavní konfigurační stránka zařízení.
 **********************************************************/
String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

String jsonEscape(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  value.replace("\t", "\\t");
  return value;
}

String normalizedUrl(String rawUrl) {
  rawUrl.trim();

  if (rawUrl.endsWith("/")) {
    rawUrl.remove(rawUrl.length() - 1);
  }

  return rawUrl;
}

String posixTzForName(String name) {
  if (name == "UTC-10 - Hawaii") return "HST10";
  if (name == "UTC-9 - Alaska") return "AKST9AKDT,M3.2.0,M11.1.0";
  if (name == "UTC-8 - USA/Canada Pacific") return "PST8PDT,M3.2.0,M11.1.0";
  if (name == "UTC-7 - USA/Canada Mountain") return "MST7MDT,M3.2.0,M11.1.0";
  if (name == "UTC-7 - Arizona no DST") return "MST7";
  if (name == "UTC-6 - USA/Canada Central, Mexico") return "CST6CDT,M3.2.0,M11.1.0";
  if (name == "UTC-5 - USA/Canada Eastern, Caribbean") return "EST5EDT,M3.2.0,M11.1.0";
  if (name == "UTC-4 - Canada Atlantic, Caribbean") return "AST4ADT,M3.2.0,M11.1.0";
  if (name == "UTC-3:30 - Newfoundland") return "NST3:30NDT,M3.2.0,M11.1.0";
  if (name == "UTC-3 - Brazil, Argentina") return "<-03>3";
  if (name == "UTC-1 - Cape Verde, Azores") return "<-01>1";
  if (name == "UTC - United Kingdom, Ireland, Portugal") return "GMT0BST,M3.5.0/1,M10.5.0";
  if (name == "UTC - Iceland, Ghana, Senegal") return "UTC0";
  if (name == "UTC+1 - Czech Republic, Germany, France, Italy") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (name == "UTC+2 - Finland, Ukraine, Greece, Egypt, South Africa") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (name == "UTC+3 - Turkey, Saudi Arabia, Moscow") return "<+03>-3";
  if (name == "UTC+4 - UAE, Oman, Georgia") return "<+04>-4";
  if (name == "UTC+4:30 - Afghanistan") return "<+0430>-4:30";
  if (name == "UTC+5 - Pakistan, Kazakhstan") return "PKT-5";
  if (name == "UTC+5:30 - India, Sri Lanka") return "IST-5:30";
  if (name == "UTC+5:45 - Nepal") return "<+0545>-5:45";
  if (name == "UTC+6 - Bangladesh, Kazakhstan") return "<+06>-6";
  if (name == "UTC+6:30 - Myanmar") return "<+0630>-6:30";
  if (name == "UTC+7 - Thailand, Vietnam, Western Indonesia") return "<+07>-7";
  if (name == "UTC+8 - China, Singapore, Western Australia") return "CST-8";
  if (name == "UTC+9 - Japan, South Korea") return "JST-9";
  if (name == "UTC+9:30 - Central Australia") return "ACST-9:30ACDT,M10.1.0,M4.1.0/3";
  if (name == "UTC+10 - Eastern Australia") return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  if (name == "UTC+11 - Solomon Islands, New Caledonia") return "<+11>-11";
  if (name == "UTC+12 - New Zealand, Fiji") return "NZST-12NZDT,M9.5.0,M4.1.0/3";
  if (name == "UTC+13 - Samoa, Tonga") return "<+13>-13";
  if (name == "UTC+14 - Kiribati Line Islands") return "<+14>-14";
  if (name == "UTC") return "UTC0";
  if (name == "Custom") return "";

  if (name == "Europe/Prague") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (name == "Europe/London") return "GMT0BST,M3.5.0/1,M10.5.0";
  if (name == "Europe/Berlin") return "CET-1CEST,M3.5.0,M10.5.0/3";
  if (name == "America/New_York") return "EST5EDT,M3.2.0,M11.1.0";
  if (name == "America/Chicago") return "CST6CDT,M3.2.0,M11.1.0";
  if (name == "America/Denver") return "MST7MDT,M3.2.0,M11.1.0";
  if (name == "America/Los_Angeles") return "PST8PDT,M3.2.0,M11.1.0";
  if (name == "America/Phoenix") return "MST7";
  if (name == "Asia/Tokyo") return "JST-9";
  if (name == "Australia/Sydney") return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  if (name == "UTC") return "UTC0";
  return "";
}

void normalizeTimeZoneConfig() {
  cfg.tzName.trim();
  cfg.tzPosix.trim();

  if (cfg.tzName == "Europe/Prague" || cfg.tzName == "Europe/Berlin") {
    cfg.tzName = "UTC+1 - Czech Republic, Germany, France, Italy";
  } else if (cfg.tzName == "Europe/London") {
    cfg.tzName = "UTC - United Kingdom, Ireland, Portugal";
  } else if (cfg.tzName == "America/New_York") {
    cfg.tzName = "UTC-5 - USA/Canada Eastern, Caribbean";
  } else if (cfg.tzName == "America/Chicago") {
    cfg.tzName = "UTC-6 - USA/Canada Central, Mexico";
  } else if (cfg.tzName == "America/Denver") {
    cfg.tzName = "UTC-7 - USA/Canada Mountain";
  } else if (cfg.tzName == "America/Los_Angeles") {
    cfg.tzName = "UTC-8 - USA/Canada Pacific";
  } else if (cfg.tzName == "America/Phoenix") {
    cfg.tzName = "UTC-7 - Arizona no DST";
  } else if (cfg.tzName == "Asia/Tokyo") {
    cfg.tzName = "UTC+9 - Japan, South Korea";
  } else if (cfg.tzName == "Australia/Sydney") {
    cfg.tzName = "UTC+10 - Eastern Australia";
  }

  if (cfg.tzName.isEmpty()) {
    cfg.tzName = defaultTzName;
  }

  if (cfg.tzName != "Custom") {
    String mapped = posixTzForName(cfg.tzName);
    if (!mapped.isEmpty()) {
      cfg.tzPosix = mapped;
    } else {
      cfg.tzName = defaultTzName;
      cfg.tzPosix = defaultTzPosix;
    }
  }

  if (cfg.tzPosix.isEmpty()) {
    cfg.tzName = defaultTzName;
    cfg.tzPosix = defaultTzPosix;
  }
}

String buildRestStatesUrlFor(String haUrl) {
  String url = normalizedUrl(haUrl);
  url += "/api/states";
  return url;
}

String htmlPage() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Circle Bambu Monitor</title>
<style>
body{background:#111;color:white;font-family:-apple-system,BlinkMacSystemFont,sans-serif;padding:20px;}
.card{max-width:550px;margin:auto;background:#1c1c1e;padding:20px;border-radius:18px;}
input,select{width:100%;padding:12px;margin-top:5px;margin-bottom:10px;border-radius:10px;border:none;background:#2c2c2e;color:white;box-sizing:border-box;}
button{width:100%;padding:14px;border:none;border-radius:12px;background:#00AE42;color:white;font-size:18px;font-weight:bold;margin-top:10px;}
button.secondary{background:#3a3a3c;font-size:15px;padding:11px;}
a{color:#00AE42;text-decoration:none;}
a.danger{color:#ff453a;}
label{display:block;margin-top:10px;}
.small{color:#aaa;font-size:14px;line-height:1.4;}
.status{color:#aaa;font-size:13px;min-height:18px;margin-top:6px;}
.suggestions{display:none;background:#2c2c2e;border:1px solid #444;border-radius:10px;margin-top:-4px;margin-bottom:10px;overflow:hidden;}
.suggestion{padding:10px 12px;border-bottom:1px solid #3a3a3c;cursor:pointer;word-break:break-word;}
.suggestion:last-child{border-bottom:none;}
.suggestion:hover{background:#3a3a3c;}
.suggestion small{display:block;color:#aaa;margin-top:3px;}
.links{margin-top:18px;display:flex;gap:14px;flex-wrap:wrap;}
</style>
</head>
<body>
<div class="card">
<h2>Circle Bambu Monitor</h2>
<p class="small">Firmware: %FW_DISPLAY_VERSION%</p>

<form action="/save" method="POST">

<label>WiFi SSID</label>
<input id="wifi_ssid" name="wifi_ssid" value="%WIFI_SSID%">
<div id="wifi_suggestions" class="suggestions"></div>
<button class="secondary" type="button" onclick="scanWifi()">Scan WiFi</button>
<div id="wifi_status" class="status"></div>

<label>WiFi Password</label>
<input id="wifi_pass" name="wifi_pass" type="password" value="%WIFI_PASS%">
<button class="secondary" type="button" onclick="togglePassword('wifi_pass',this)">Show password</button>

<label>Home Assistant URL</label>
<input name="ha_url" value="%HA_URL%">

<label>Long Lived Token</label>
<input id="ha_token" name="ha_token" type="password" value="%HA_TOKEN%">
<button class="secondary" type="button" onclick="togglePassword('ha_token',this)">Show token</button>

<label>OTA Password</label>
<input id="ota_pass" name="ota_pass" type="password" value="%OTA_PASS%">
<button class="secondary" type="button" onclick="togglePassword('ota_pass',this)">Show password</button>

<label>Time Zone</label>
<input id="tz_name" name="tz_name" value="%TZ_NAME%" autocomplete="off">
<div id="tz_suggestions" class="suggestions"></div>

<label>POSIX Time Zone</label>
<input id="tz_posix" name="tz_posix" value="%TZ_POSIX%">
<p class="small">Used for clock and finish time conversion. Choose Custom to edit manually.</p>

%ENTITY_SECTION%

<button type="submit">Save and Restart</button>
</form>

<div class="links">
<a href="/diagnostics">Diagnostics</a>
<a href="/restart">Restart device</a>
<a class="danger" href="/factory-reset">Factory reset</a>
</div>

</div>

<script>
function togglePassword(id,button){
  const input=document.getElementById(id);
  if(input.type==='password'){
    input.type='text';
    button.textContent='Hide';
  }else{
    input.type='password';
    button.textContent=id==='ha_token'?'Show token':'Show password';
  }
}

%ENTITY_SCRIPT%

const tzOptions=[
  {label:'UTC-10 - Hawaii',region:'Americas / Pacific',keywords:'hawaii honolulu pacific',posix:'HST10'},
  {label:'UTC-9 - Alaska',region:'Americas',keywords:'alaska anchorage',posix:'AKST9AKDT,M3.2.0,M11.1.0'},
  {label:'UTC-8 - USA/Canada Pacific',region:'Americas',keywords:'usa canada pacific california washington oregon british columbia vancouver los angeles seattle',posix:'PST8PDT,M3.2.0,M11.1.0'},
  {label:'UTC-7 - USA/Canada Mountain',region:'Americas',keywords:'usa canada mountain colorado alberta denver calgary',posix:'MST7MDT,M3.2.0,M11.1.0'},
  {label:'UTC-7 - Arizona no DST',region:'Americas',keywords:'arizona phoenix no daylight saving',posix:'MST7'},
  {label:'UTC-6 - USA/Canada Central, Mexico',region:'Americas',keywords:'usa canada mexico central texas illinois manitoba chicago mexico city',posix:'CST6CDT,M3.2.0,M11.1.0'},
  {label:'UTC-5 - USA/Canada Eastern, Caribbean',region:'Americas',keywords:'usa canada eastern florida new york ontario quebec caribbean cuba jamaica',posix:'EST5EDT,M3.2.0,M11.1.0'},
  {label:'UTC-4 - Canada Atlantic, Caribbean',region:'Americas',keywords:'atlantic canada nova scotia new brunswick puerto rico dominican republic caribbean',posix:'AST4ADT,M3.2.0,M11.1.0'},
  {label:'UTC-3:30 - Newfoundland',region:'Americas',keywords:'newfoundland canada st johns',posix:'NST3:30NDT,M3.2.0,M11.1.0'},
  {label:'UTC-3 - Brazil, Argentina',region:'Americas',keywords:'brazil argentina uruguay chile greenland buenos aires sao paulo rio',posix:'<-03>3'},
  {label:'UTC-1 - Cape Verde, Azores',region:'Atlantic',keywords:'cape verde azores cabo verde',posix:'<-01>1'},
  {label:'UTC - United Kingdom, Ireland, Portugal',region:'Europe / Atlantic',keywords:'uk united kingdom britain ireland portugal london dublin lisbon daylight saving',posix:'GMT0BST,M3.5.0/1,M10.5.0'},
  {label:'UTC - Iceland, Ghana, Senegal',region:'Europe / Africa',keywords:'iceland ghana senegal morocco no daylight saving',posix:'UTC0'},
  {label:'UTC+1 - Czech Republic, Germany, France, Italy',region:'Europe',keywords:'czech republic germany france italy spain netherlands belgium austria poland central europe',posix:'CET-1CEST,M3.5.0,M10.5.0/3'},
  {label:'UTC+2 - Finland, Ukraine, Greece, Egypt, South Africa',region:'Europe / Africa',keywords:'finland ukraine greece baltics egypt south africa eastern europe',posix:'EET-2EEST,M3.5.0/3,M10.5.0/4'},
  {label:'UTC+3 - Turkey, Saudi Arabia, Moscow',region:'Europe / Middle East',keywords:'turkey saudi arabia qatar kuwait iraq moscow russia east africa',posix:'<+03>-3'},
  {label:'UTC+4 - UAE, Oman, Georgia',region:'Middle East / Asia',keywords:'uae united arab emirates dubai oman georgia armenia azerbaijan',posix:'<+04>-4'},
  {label:'UTC+4:30 - Afghanistan',region:'Asia',keywords:'afghanistan kabul',posix:'<+0430>-4:30'},
  {label:'UTC+5 - Pakistan, Kazakhstan',region:'Asia',keywords:'pakistan kazakhstan uzbekistan tajikistan',posix:'PKT-5'},
  {label:'UTC+5:30 - India, Sri Lanka',region:'Asia',keywords:'india sri lanka delhi mumbai kolkata colombo',posix:'IST-5:30'},
  {label:'UTC+5:45 - Nepal',region:'Asia',keywords:'nepal kathmandu',posix:'<+0545>-5:45'},
  {label:'UTC+6 - Bangladesh, Kazakhstan',region:'Asia',keywords:'bangladesh kazakhstan bhutan dhaka almaty',posix:'<+06>-6'},
  {label:'UTC+6:30 - Myanmar',region:'Asia',keywords:'myanmar yangon burma',posix:'<+0630>-6:30'},
  {label:'UTC+7 - Thailand, Vietnam, Western Indonesia',region:'Asia',keywords:'thailand vietnam cambodia laos western indonesia bangkok jakarta',posix:'<+07>-7'},
  {label:'UTC+8 - China, Singapore, Western Australia',region:'Asia / Australia',keywords:'china singapore malaysia philippines taiwan hong kong western australia perth beijing',posix:'CST-8'},
  {label:'UTC+9 - Japan, South Korea',region:'Asia',keywords:'japan korea south korea tokyo seoul',posix:'JST-9'},
  {label:'UTC+9:30 - Central Australia',region:'Australia',keywords:'central australia adelaide darwin south australia northern territory',posix:'ACST-9:30ACDT,M10.1.0,M4.1.0/3'},
  {label:'UTC+10 - Eastern Australia',region:'Australia / Pacific',keywords:'eastern australia sydney melbourne brisbane tasmania',posix:'AEST-10AEDT,M10.1.0,M4.1.0/3'},
  {label:'UTC+11 - Solomon Islands, New Caledonia',region:'Pacific',keywords:'solomon islands new caledonia vanuatu',posix:'<+11>-11'},
  {label:'UTC+12 - New Zealand, Fiji',region:'Pacific',keywords:'new zealand fiji auckland wellington',posix:'NZST-12NZDT,M9.5.0,M4.1.0/3'},
  {label:'UTC+13 - Samoa, Tonga',region:'Pacific',keywords:'samoa tonga',posix:'<+13>-13'},
  {label:'UTC+14 - Kiribati Line Islands',region:'Pacific',keywords:'kiribati line islands kiritimati',posix:'<+14>-14'},
  {label:'UTC',region:'Universal',keywords:'utc gmt zulu universal',posix:'UTC0'},
  {label:'Custom',region:'Advanced',keywords:'manual custom posix',posix:''}
];

function setTimeZone(option){
  const name=document.getElementById('tz_name');
  const posix=document.getElementById('tz_posix');
  name.value=option.label;
  posix.readOnly=option.label!=='Custom';
  if(option.label!=='Custom'){
    posix.value=option.posix;
  }
  document.getElementById('tz_suggestions').style.display='none';
}

function renderTimeZoneSuggestions(){
  const input=document.getElementById('tz_name');
  const box=document.getElementById('tz_suggestions');
  const q=input.value.trim().toLowerCase();
  const matches=tzOptions.filter(o=>{
    const text=(o.label+' '+o.region+' '+o.keywords+' '+o.posix).toLowerCase();
    return !q||text.includes(q);
  }).slice(0,35);
  box.innerHTML='';

  matches.forEach(option=>{
    const row=document.createElement('div');
    row.className='suggestion';
    row.textContent=option.label;

    const small=document.createElement('small');
    small.textContent=option.label==='Custom'?'Manual POSIX time zone':option.region+' | '+option.posix;
    row.appendChild(small);

    row.addEventListener('mousedown',ev=>{
      ev.preventDefault();
      setTimeZone(option);
    });

    box.appendChild(row);
  });

  box.style.display=matches.length?'block':'none';
}

function setupTimeZoneAutocomplete(){
  const input=document.getElementById('tz_name');
  const posix=document.getElementById('tz_posix');
  const selected=tzOptions.find(o=>o.label===input.value);

  if(selected){
    posix.readOnly=selected.label!=='Custom';
    if(selected.label!=='Custom'){
      posix.value=selected.posix;
    }
  }else{
    posix.readOnly=false;
  }

  input.addEventListener('focus',renderTimeZoneSuggestions);
  input.addEventListener('input',renderTimeZoneSuggestions);
}

async function scanWifi(){
  const status=document.getElementById('wifi_status');
  status.textContent='Scanning...';
  renderWifiSuggestions([]);
  try{
    const res=await fetch('/wifi-scan');
    const networks=await res.json();
    renderWifiSuggestions(networks);
    status.textContent=networks.length+' networks found';
  }catch(e){
    status.textContent='WiFi scan failed';
  }
}

function renderWifiSuggestions(networks){
  const box=document.getElementById('wifi_suggestions');
  const input=document.getElementById('wifi_ssid');
  box.innerHTML='';

  if(networks.length===0){
    box.style.display='none';
    return;
  }

  networks.forEach(n=>{
    const row=document.createElement('div');
    row.className='suggestion';
    row.textContent=n.ssid;

    const small=document.createElement('small');
    small.textContent=n.rssi+' dBm'+(n.secure?' - secured':' - open');
    row.appendChild(small);

    row.addEventListener('mousedown',ev=>{
      ev.preventDefault();
      input.value=n.ssid;
      box.style.display='none';
      document.getElementById('wifi_status').textContent='Selected '+n.ssid;
    });

    box.appendChild(row);
  });

  box.style.display='block';
}

document.addEventListener('click',ev=>{
  const wifiBox=document.getElementById('wifi_suggestions');
  const wifiInput=document.getElementById('wifi_ssid');
  if(ev.target!==wifiInput&&!wifiBox.contains(ev.target)){
    wifiBox.style.display='none';
  }

  const tzBox=document.getElementById('tz_suggestions');
  const tzInput=document.getElementById('tz_name');
  if(ev.target!==tzInput&&!tzBox.contains(ev.target)){
    tzBox.style.display='none';
  }
});

window.addEventListener('load',setupTimeZoneAutocomplete);
</script>
</body>
</html>
)rawliteral";

  String entitySection = "";
  String entityScript = "";
  bool hasAnyEntityConfig = !cfg.entityPhase.isEmpty() ||
                            !cfg.entityStatus.isEmpty() ||
                            !cfg.entityProgress.isEmpty() ||
                            !cfg.entityFilament.isEmpty() ||
                            !cfg.entityFinishTime.isEmpty() ||
                            !cfg.entityNozzleTemp.isEmpty() ||
                            !cfg.entityBedTemp.isEmpty() ||
                            !cfg.entityCurrentLayer.isEmpty() ||
                            !cfg.entityTotalLayers.isEmpty() ||
                            !cfg.entityFilamentWeight.isEmpty();
  bool haReadyForEntityConfig = wsAuthenticated ||
                                wsSubscribed ||
                                firstHaReadDone ||
                                hasAnyEntityConfig;
  bool showEntityConfig = !portalMode &&
                          WiFi.status() == WL_CONNECTED &&
                          !cfg.haUrl.isEmpty() &&
                          !cfg.haToken.isEmpty() &&
                          haReadyForEntityConfig;

  if (showEntityConfig) {
    entitySection = R"rawliteral(
<div id="ha_status" class="status"></div>
<div id="entity_suggestions" class="suggestions"></div>

<label>Phase Entity</label>
<input class="entity-input" name="ent_phase" value="%ENT_PHASE%">

<label>Print Status Entity</label>
<input class="entity-input" name="ent_status" value="%ENT_STATUS%">

<label>Progress Entity</label>
<input class="entity-input" name="ent_progress" value="%ENT_PROGRESS%">

<label>Filament Entity</label>
<input class="entity-input" name="ent_filament" value="%ENT_FILAMENT%">

<label>Finish Time Entity</label>
<input class="entity-input" name="ent_finish" value="%ENT_FINISH%">

<label>Nozzle Temp Entity</label>
<input class="entity-input" name="ent_nozzle" value="%ENT_NOZZLE%">

<label>Bed Temp Entity</label>
<input class="entity-input" name="ent_bed" value="%ENT_BED%">

<label>Current Layer Entity</label>
<input class="entity-input" name="ent_layer" value="%ENT_LAYER%">

<label>Total Layers Entity</label>
<input class="entity-input" name="ent_layers" value="%ENT_LAYERS%">

<label>Filament Weight Entity</label>
<input class="entity-input" name="ent_weight" value="%ENT_WEIGHT%">
)rawliteral";

    entityScript = R"rawliteral(
let entitySearchTimer=null;
let activeEntityInput=null;

function renderEntitySuggestions(entities){
  const box=document.getElementById('entity_suggestions');
  box.innerHTML='';

  if(!activeEntityInput||entities.length===0){
    box.style.display='none';
    return;
  }

  entities.forEach(e=>{
    const row=document.createElement('div');
    row.className='suggestion';
    row.textContent=e.entity_id;

    if(e.name){
      const small=document.createElement('small');
      small.textContent=e.name;
      row.appendChild(small);
    }

    row.addEventListener('mousedown',ev=>{
      ev.preventDefault();
      activeEntityInput.value=e.entity_id;
      box.style.display='none';
      document.getElementById('ha_status').textContent='Selected '+e.entity_id;
    });

    box.appendChild(row);
  });

  activeEntityInput.insertAdjacentElement('afterend',box);
  box.style.display='block';
}

async function searchHaEntities(q){
  const status=document.getElementById('ha_status');
  const haUrl=document.querySelector('[name=ha_url]').value;
  const haToken=document.querySelector('[name=ha_token]').value;

  if(q.length<2){
    renderEntitySuggestions([]);
    return;
  }

  status.textContent='Searching "'+q+'"...';

  try{
    const params=new URLSearchParams({q:q,ha_url:haUrl,ha_token:haToken});
    const res=await fetch('/ha-entity-search?'+params.toString());
    if(!res.ok){
      const msg=await res.text();
      throw new Error(msg||('HTTP '+res.status));
    }
    const entities=await res.json();
    renderEntitySuggestions(entities);
    status.textContent=entities.length?entities.length+' matches':'No matches';
  }catch(e){
    status.textContent='HA search failed: '+e.message;
  }
}

function setupEntityAutocomplete(){
  document.querySelectorAll('.entity-input').forEach(input=>{
    input.addEventListener('focus',()=>{
      activeEntityInput=input;
    });
    input.addEventListener('input',()=>{
      activeEntityInput=input;
      const q=input.value.trim();
      clearTimeout(entitySearchTimer);
      entitySearchTimer=setTimeout(()=>searchHaEntities(q),350);
    });
  });

  document.addEventListener('click',ev=>{
    const entityBox=document.getElementById('entity_suggestions');
    if(ev.target!==activeEntityInput&&!entityBox.contains(ev.target)){
      entityBox.style.display='none';
    }

    const wifiBox=document.getElementById('wifi_suggestions');
    const wifiInput=document.getElementById('wifi_ssid');
    if(ev.target!==wifiInput&&!wifiBox.contains(ev.target)){
      wifiBox.style.display='none';
    }
  });
}

window.addEventListener('load',setupEntityAutocomplete);
)rawliteral";
  } else {
    entitySection = "<p class='small'>Save WiFi and Home Assistant settings first. Entity setup will appear after the device reconnects.</p>";
  }

  page.replace("%ENTITY_SECTION%", entitySection);
  page.replace("%ENTITY_SCRIPT%", entityScript);

  page.replace("%FW_VERSION%", FW_VERSION);
  page.replace("%FW_DISPLAY_VERSION%", FW_DISPLAY_VERSION);

  page.replace("%WIFI_SSID%", htmlEscape(cfg.wifiSsid));
  page.replace("%WIFI_PASS%", htmlEscape(cfg.wifiPass));
  page.replace("%HA_URL%", htmlEscape(cfg.haUrl));
  page.replace("%HA_TOKEN%", htmlEscape(cfg.haToken));
  page.replace("%OTA_PASS%", htmlEscape(cfg.otaPass));
  page.replace("%TZ_NAME%", htmlEscape(cfg.tzName));
  page.replace("%TZ_POSIX%", htmlEscape(cfg.tzPosix));

  page.replace("%ENT_PHASE%", htmlEscape(cfg.entityPhase));
  page.replace("%ENT_STATUS%", htmlEscape(cfg.entityStatus));
  page.replace("%ENT_PROGRESS%", htmlEscape(cfg.entityProgress));
  page.replace("%ENT_FILAMENT%", htmlEscape(cfg.entityFilament));
  page.replace("%ENT_FINISH%", htmlEscape(cfg.entityFinishTime));
  page.replace("%ENT_NOZZLE%", htmlEscape(cfg.entityNozzleTemp));
  page.replace("%ENT_BED%", htmlEscape(cfg.entityBedTemp));
  page.replace("%ENT_LAYER%", htmlEscape(cfg.entityCurrentLayer));
  page.replace("%ENT_LAYERS%", htmlEscape(cfg.entityTotalLayers));
  page.replace("%ENT_WEIGHT%", htmlEscape(cfg.entityFilamentWeight));

  return page;
}

/**********************************************************
 * DIAGNOSTICS PAGE
 *
 * Jednoduchá diagnostická stránka:
 * - IP adresa
 * - WiFi RSSI
 * - stav WebSocketu
 * - aktuální data tiskárny
 **********************************************************/
String diagnosticsPage() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Diagnostics</title>
<style>
body{background:#111;color:white;font-family:-apple-system,BlinkMacSystemFont,sans-serif;padding:20px;}
.card{max-width:550px;margin:auto;background:#1c1c1e;padding:20px;border-radius:18px;}
.row{display:flex;justify-content:space-between;border-bottom:1px solid #333;padding:9px 0;gap:16px;}
.key{color:#aaa;}
.value{text-align:right;word-break:break-word;}
a{color:#00AE42;text-decoration:none;}
</style>
</head>
<body>
<div class="card">
<h2>Diagnostics</h2>
%ROWS%
<br>
<a href="/">Back</a>
</div>
</body>
</html>
)rawliteral";

  String rows = "";

  auto addRow = [&](String key, String value) {
    rows += "<div class='row'><div class='key'>";
    rows += htmlEscape(key);
    rows += "</div><div class='value'>";
    rows += htmlEscape(value);
    rows += "</div></div>";
  };

  addRow("Firmware", FW_VERSION);
  addRow("Mode", portalMode ? "AP Portal" : "WiFi Client");
  addRow("WiFi", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  addRow("IP", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
  addRow("RSSI", WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "-");
  addRow("Free heap", String(ESP.getFreeHeap()));
  addRow("HA URL", cfg.haUrl);
  addRow("HA Auth Invalid", haAuthInvalid ? "Yes" : "No");
  addRow("Time Zone", cfg.tzName);
  addRow("POSIX TZ", cfg.tzPosix);
  addRow("WS Auth", wsAuthenticated ? "OK" : "No");
  addRow("WS Subscribed", wsSubscribed ? "OK" : "No");
  addRow("WS Connect Failed", wsConnectFailed ? "Yes" : "No");
  addRow("First HA read", firstHaReadDone ? "OK" : "No");
  addRow("OTA", otaReady ? (otaActive ? "Updating " + String(otaProgress) + "%" : "Ready") : "Disabled");
  addRow("OTA Password", cfg.otaPass.isEmpty() ? "Not set" : "Set");
  addRow("Phase", data.phase);
  addRow("Status", data.status);
  addRow("Progress", isnan(data.progress) ? "-" : String(data.progress, 1));
  addRow("Filament", data.filament);
  addRow("Nozzle", isnan(data.nozzleTemp) ? "-" : String(data.nozzleTemp, 1));
  addRow("Bed", isnan(data.bedTemp) ? "-" : String(data.bedTemp, 1));
  addRow("Layer", (isnan(data.currentLayer) || isnan(data.totalLayers)) ? "-" : String((int)data.currentLayer) + " / " + String((int)data.totalLayers));
  addRow("Weight", isnan(data.filamentWeight) ? "-" : String(data.filamentWeight, 2));

  page.replace("%ROWS%", rows);

  return page;
}

/**********************************************************
 * WEB SERVER HANDLERS
 **********************************************************/
void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleDiagnostics() {
  server.send(200, "text/html", diagnosticsPage());
}

void handleWifiScan() {
  int count = WiFi.scanNetworks(false, true);
  String json = "[";

  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";

    json += "{\"ssid\":\"";
    json += jsonEscape(WiFi.SSID(i));
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI(i));
    json += ",\"secure\":";
    json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "false" : "true";
    json += "}";
  }

  json += "]";
  WiFi.scanDelete();

  server.send(200, "application/json", json);
}

bool entityLooksUseful(String entityId, String friendlyName) {
  String text = entityId + " " + friendlyName;
  text.toLowerCase();

  bool configuredEntity = entityId == cfg.entityPhase ||
                          entityId == cfg.entityStatus ||
                          entityId == cfg.entityProgress ||
                          entityId == cfg.entityFilament ||
                          entityId == cfg.entityFinishTime ||
                          entityId == cfg.entityNozzleTemp ||
                          entityId == cfg.entityBedTemp ||
                          entityId == cfg.entityCurrentLayer ||
                          entityId == cfg.entityTotalLayers ||
                          entityId == cfg.entityFilamentWeight;

  return configuredEntity ||
         entityId.startsWith("sensor.") ||
         entityId.startsWith("binary_sensor.") ||
         entityId.startsWith("select.") ||
         entityId.startsWith("number.") ||
         entityId.startsWith("text.") ||
         entityId.startsWith("switch.") ||
         text.indexOf("bambu") >= 0 ||
         text.indexOf("ams") >= 0 ||
         text.indexOf("print") >= 0 ||
         text.indexOf("printer") >= 0 ||
         text.indexOf("nozzle") >= 0 ||
         text.indexOf("bed") >= 0 ||
         text.indexOf("plate") >= 0 ||
         text.indexOf("chamber") >= 0 ||
         text.indexOf("temperature") >= 0 ||
         text.indexOf("filament") >= 0 ||
         text.indexOf("layer") >= 0 ||
         text.indexOf("progress") >= 0 ||
         text.indexOf("finish") >= 0 ||
         text.indexOf("remaining") >= 0 ||
         text.indexOf("weight") >= 0 ||
         text.indexOf("phase") >= 0 ||
         text.indexOf("status") >= 0;
}

void handleHaEntities() {
  String haUrl = server.hasArg("ha_url") ? server.arg("ha_url") : cfg.haUrl;
  String haToken = server.hasArg("ha_token") ? server.arg("ha_token") : cfg.haToken;

  haUrl.trim();
  haToken.trim();

  if (haUrl.isEmpty() || haToken.isEmpty()) {
    server.send(400, "text/plain", "Missing HA URL or token");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    server.send(409, "text/plain", "ESP is not connected to WiFi, so it cannot reach Home Assistant");
    return;
  }

  HTTPClient http;

  http.begin(buildRestStatesUrlFor(haUrl));
  http.useHTTP10(true);
  http.setReuse(false);
  http.setTimeout(12000);
  http.addHeader("Authorization", "Bearer " + haToken);
  http.addHeader("Content-Type", "application/json");

  int code = http.GET();

  if (code != 200) {
    Serial.print("HA entities error ");
    Serial.println(code);
    http.end();
    server.send(502, "text/plain", "Home Assistant returned HTTP " + String(code));
    return;
  }

  DynamicJsonDocument filter(384);
  filter[0]["entity_id"] = true;
  filter[0]["attributes"]["friendly_name"] = true;

  DynamicJsonDocument doc(49152);
  DeserializationError error = deserializeJson(
    doc,
    http.getStream(),
    DeserializationOption::Filter(filter)
  );

  http.end();

  if (error) {
    Serial.print("HA entities JSON error: ");
    Serial.println(error.c_str());
    Serial.print("Free heap: ");
    Serial.println(ESP.getFreeHeap());
    server.send(500, "text/plain", "HA JSON parse failed: " + String(error.c_str()) + ". Try again; if it repeats, HA /api/states is probably too large for this ESP32-C3.");
    return;
  }

  String json = "[";
  int added = 0;

  for (JsonObject entity : doc.as<JsonArray>()) {
    String entityId = entity["entity_id"] | "";
    String friendlyName = entity["attributes"]["friendly_name"] | "";

    if (entityId.isEmpty()) continue;
    if (!entityLooksUseful(entityId, friendlyName)) continue;

    if (added > 0) json += ",";

    json += "{\"entity_id\":\"";
    json += jsonEscape(entityId);
    json += "\",\"name\":\"";
    json += jsonEscape(friendlyName);
    json += "\"}";

    added++;

    if (added >= 160) break;
  }

  json += "]";
  server.send(200, "application/json", json);
}

void handleHaEntitySearch() {
  String query = server.arg("q");
  String haUrl = server.hasArg("ha_url") ? server.arg("ha_url") : cfg.haUrl;
  String haToken = server.hasArg("ha_token") ? server.arg("ha_token") : cfg.haToken;

  query.trim();
  query.toLowerCase();
  haUrl.trim();
  haToken.trim();

  if (query.length() < 2) {
    server.send(200, "application/json", "[]");
    return;
  }

  if (haUrl.isEmpty() || haToken.isEmpty()) {
    server.send(400, "text/plain", "Missing HA URL or token");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    server.send(409, "text/plain", "ESP is not connected to WiFi, so it cannot reach Home Assistant");
    return;
  }

  HTTPClient http;

  http.begin(buildRestStatesUrlFor(haUrl));
  http.useHTTP10(true);
  http.setReuse(false);
  http.setTimeout(12000);
  http.addHeader("Authorization", "Bearer " + haToken);
  http.addHeader("Content-Type", "application/json");

  int code = http.GET();

  if (code != 200) {
    Serial.print("HA entity search error ");
    Serial.println(code);
    http.end();
    server.send(502, "text/plain", "Home Assistant returned HTTP " + String(code));
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  String json = "[";
  int added = 0;
  bool inString = false;
  bool escape = false;
  bool nextStringIsEntityId = false;
  String token = "";

  while (http.connected() && added < 60) {
    while (stream->available() && added < 60) {
      char c = stream->read();

      if (inString) {
        token += c;

        if (escape) {
          escape = false;
        } else if (c == '\\') {
          escape = true;
        } else if (c == '"') {
          inString = false;
          String content = token.substring(1, token.length() - 1);

          if (nextStringIsEntityId) {
            String lowerEntityId = content;
            lowerEntityId.toLowerCase();

            if (lowerEntityId.indexOf(query) >= 0) {
              if (added > 0) json += ",";

              json += "{\"entity_id\":\"";
              json += jsonEscape(content);
              json += "\",\"name\":\"\"}";

              added++;
            }
            nextStringIsEntityId = false;
          } else if (content == "entity_id") {
            nextStringIsEntityId = true;
          }

          token = "";
        }

        if (token.length() > 180) {
          token = "";
          inString = false;
          escape = false;
        }
      } else if (c == '"') {
        inString = true;
        escape = false;
        token = "\"";
      }
    }

    if (!stream->available()) {
      delay(1);
    }
  }

  http.end();

  json += "]";
  server.send(200, "application/json", json);
}

String restartReturnPage(String title, String message, String guidance) {
  String guidanceBlock = "";
  if (guidance.length() > 0) {
    guidanceBlock = "<p class=\"small\">" + htmlEscape(guidance) + "</p>";
  }

  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>%TITLE%</title>
<style>
body{background:#111;color:white;font-family:-apple-system,BlinkMacSystemFont,sans-serif;padding:20px;}
.card{max-width:550px;margin:auto;background:#1c1c1e;padding:20px;border-radius:18px;}
.small{color:#aaa;font-size:14px;line-height:1.4;}
a{color:#00AE42;text-decoration:none;}
</style>
</head>
<body>
<div class="card">
<h2>%TITLE%</h2>
<p>%MESSAGE%</p>
%GUIDANCE_BLOCK%
<p class="small">If the device keeps the same network address, this page will try to reopen the configuration automatically.</p>
<p><a href="/">Try opening configuration</a></p>
</div>
<script>
function tryOpenConfig(){
  fetch('/',{cache:'no-store'})
    .then(r=>{
      if(r.ok){
        window.location.href='/';
      }else{
        setTimeout(tryOpenConfig,1000);
      }
    })
    .catch(()=>setTimeout(tryOpenConfig,1000));
}
setTimeout(tryOpenConfig,3000);
</script>
</body>
</html>
)rawliteral";

  page.replace("%TITLE%", htmlEscape(title));
  page.replace("%MESSAGE%", htmlEscape(message));
  page.replace("%GUIDANCE_BLOCK%", guidanceBlock);
  return page;
}

void handleRestart() {
  server.send(200, "text/html", restartReturnPage(
    "Restarting...",
    "Restarting device...",
    ""
  ));
  delay(800);
  ESP.restart();
}

void handleFactoryReset() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Factory Reset</title>
<style>
body{background:#111;color:white;font-family:-apple-system,BlinkMacSystemFont,sans-serif;padding:20px;}
.card{max-width:550px;margin:auto;background:#1c1c1e;padding:20px;border-radius:18px;}
.warn{color:#ff453a;font-weight:bold;}
button{width:100%;padding:14px;border:none;border-radius:12px;background:#ff453a;color:white;font-size:18px;font-weight:bold;margin-top:10px;}
a{display:block;color:#00AE42;text-decoration:none;margin-top:18px;}
</style>
</head>
<body>
<div class="card">
<h2>Factory Reset</h2>
<p class="warn">This will erase WiFi, Home Assistant, OTA, time zone, and entity configuration.</p>
<p>The device will restart and open the setup portal if it cannot connect to WiFi.</p>
<form action="/factory-reset-confirm" method="POST">
<button type="submit">Confirm Factory Reset</button>
</form>
<a href="/">Cancel and go back</a>
</div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", page);
}

void handleFactoryResetConfirm() {
  prefs.begin("cbm", false);
  prefs.clear();
  prefs.end();

  server.send(200, "text/html", "<h2>Factory reset done.</h2><p>Restarting...</p>");
  delay(1200);
  ESP.restart();
}

void handleSave() {
  cfg.wifiSsid = server.arg("wifi_ssid");
  cfg.wifiPass = server.arg("wifi_pass");
  cfg.haUrl = server.arg("ha_url");
  cfg.haToken = server.arg("ha_token");
  cfg.otaPass = server.arg("ota_pass");
  cfg.tzName = server.arg("tz_name");
  cfg.tzPosix = server.arg("tz_posix");
  normalizeTimeZoneConfig();

  if (server.hasArg("ent_phase")) cfg.entityPhase = server.arg("ent_phase");
  if (server.hasArg("ent_status")) cfg.entityStatus = server.arg("ent_status");
  if (server.hasArg("ent_progress")) cfg.entityProgress = server.arg("ent_progress");
  if (server.hasArg("ent_filament")) cfg.entityFilament = server.arg("ent_filament");
  if (server.hasArg("ent_finish")) cfg.entityFinishTime = server.arg("ent_finish");
  if (server.hasArg("ent_nozzle")) cfg.entityNozzleTemp = server.arg("ent_nozzle");
  if (server.hasArg("ent_bed")) cfg.entityBedTemp = server.arg("ent_bed");
  if (server.hasArg("ent_layer")) cfg.entityCurrentLayer = server.arg("ent_layer");
  if (server.hasArg("ent_layers")) cfg.entityTotalLayers = server.arg("ent_layers");
  if (server.hasArg("ent_weight")) cfg.entityFilamentWeight = server.arg("ent_weight");

  saveConfig();

  server.send(200, "text/html", restartReturnPage(
    "Saved.",
    "Restarting device...",
    "If this was the first setup, reconnect to your normal WiFi network, find this device in your router or network device list, then open its IP address to finish configuration."
  ));
  delay(1500);
  ESP.restart();
}

/**********************************************************
 * WEB SERVER SETUP
 **********************************************************/
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/diagnostics", handleDiagnostics);
  server.on("/wifi-scan", handleWifiScan);
  server.on("/ha-entities", handleHaEntities);
  server.on("/ha-entity-search", handleHaEntitySearch);
  server.on("/restart", handleRestart);
  server.on("/factory-reset", HTTP_GET, handleFactoryReset);
  server.on("/factory-reset-confirm", HTTP_POST, handleFactoryResetConfirm);
  server.begin();

  Serial.println("Web server started");
}

/**********************************************************
 * CONFIGURATION PORTAL
 *
 * Spustí vlastní hotspot pouze pokud není možné
 * připojit se k uložené WiFi síti.
 **********************************************************/
void startPortal() {
  portalMode = true;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid, apPass);

  setupWebServer();

  Serial.println("Portal started");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

/**********************************************************
 * WIFI CONNECTION
 *
 * Připojení k domácí WiFi.
 * Pokud se připojení podaří, web server běží
 * na lokální IP adrese zařízení.
 **********************************************************/
bool connectWifi() {
  if (cfg.wifiSsid.isEmpty()) return false;

  showBootMessage("WiFi", "Connecting");

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());

  Serial.print("Connecting to WiFi");

  unsigned long start = millis();
  unsigned long lastDot = 0;

  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    serviceDisplayDuringSetup(20);

    if (millis() - lastDot >= 500) {
      lastDot = millis();
      Serial.print(".");
    }
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    portalMode = false;

    Serial.println("WiFi connected.");
    Serial.print("STA IP: ");
    Serial.println(WiFi.localIP());

    setupWebServer();
    showBootMessage("WiFi", WiFi.localIP().toString());

    return true;
  }

  return false;
}

/**********************************************************
 * HOME ASSISTANT URL HELPERS
 **********************************************************/
String normalizedHaUrl() {
  String url = cfg.haUrl;
  url.trim();

  if (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }

  return url;
}

String buildRestUrl(String entityId) {
  String url = normalizedHaUrl();
  url += "/api/states/";
  url += entityId;
  return url;
}

String buildRestStatesUrl() {
  String url = normalizedHaUrl();
  url += "/api/states";
  return url;
}

String buildWsUrl() {
  String url = normalizedHaUrl();

  if (url.startsWith("https://")) {
    url.replace("https://", "wss://");
  } else if (url.startsWith("http://")) {
    url.replace("http://", "ws://");
  } else {
    url = "ws://" + url;
  }

  url += "/api/websocket";
  return url;
}

/**********************************************************
 * DATA CONVERSION HELPERS
 **********************************************************/
float stateToFloat(String value) {
  value.trim();
  value.toLowerCase();

  if (value == "" || value == "unknown" || value == "unavailable" || value == "none") {
    return NAN;
  }

  char *endPtr = nullptr;
  float parsed = strtof(value.c_str(), &endPtr);

  if (endPtr == value.c_str()) {
    return NAN;
  }

  while (*endPtr == ' ' || *endPtr == '\t') {
    endPtr++;
  }

  if (*endPtr != '\0') {
    return NAN;
  }

  return parsed;
}

bool isKnownEntity(String entityId) {
  return entityId == cfg.entityPhase ||
         entityId == cfg.entityStatus ||
         entityId == cfg.entityProgress ||
         entityId == cfg.entityFilament ||
         entityId == cfg.entityFinishTime ||
         entityId == cfg.entityNozzleTemp ||
         entityId == cfg.entityBedTemp ||
         entityId == cfg.entityCurrentLayer ||
         entityId == cfg.entityTotalLayers ||
         entityId == cfg.entityFilamentWeight;
}

/**********************************************************
 * PRINTER DATA UPDATE
 *
 * Aktualizuje interní data podle entity_id.
 **********************************************************/
void updatePrinterData(String entityId, String state) {
  if (entityId == cfg.entityPhase) {
    data.phase = state;
  } else if (entityId == cfg.entityStatus) {
    data.status = state;
  } else if (entityId == cfg.entityProgress) {
    data.progress = stateToFloat(state);
  } else if (entityId == cfg.entityFilament) {
    data.filament = state;
  } else if (entityId == cfg.entityFinishTime) {
    data.finishTime = state;
  } else if (entityId == cfg.entityNozzleTemp) {
    data.nozzleTemp = stateToFloat(state);
  } else if (entityId == cfg.entityBedTemp) {
    data.bedTemp = stateToFloat(state);
  } else if (entityId == cfg.entityCurrentLayer) {
    data.currentLayer = stateToFloat(state);
  } else if (entityId == cfg.entityTotalLayers) {
    data.totalLayers = stateToFloat(state);
  } else if (entityId == cfg.entityFilamentWeight) {
    data.filamentWeight = stateToFloat(state);
  }
}

void serviceUiDuringRest() {
  server.handleClient();

  if (otaReady) {
    ArduinoOTA.handle();
  }

  serviceDisplayDuringSetup();
}

/**********************************************************
 * HOME ASSISTANT REST API
 *
 * Používá se jen při startu nebo reconnectu WebSocketu,
 * aby zařízení nezůstalo viset na starém stavu.
 **********************************************************/
bool readOneInitialState(String entityId) {
  if (entityId.isEmpty()) return false;

  HTTPClient http;

  http.begin(buildRestUrl(entityId));
  http.useHTTP10(true);
  http.setReuse(false);
  http.setConnectTimeout(initialStateRequestTimeout);
  http.setTimeout(initialStateRequestTimeout);
  http.addHeader("Authorization", "Bearer " + cfg.haToken);
  http.addHeader("Content-Type", "application/json");

  int code = http.GET();

  if (code != 200) {
    Serial.print("REST state error ");
    Serial.print(code);
    Serial.print(" for ");
    Serial.println(entityId);
    http.end();
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, http.getStream());

  http.end();

  if (error) {
    Serial.print("REST state JSON error for ");
    Serial.print(entityId);
    Serial.print(": ");
    Serial.println(error.c_str());
    return false;
  }

  String state = doc["state"] | "";

  if (state.length() == 0) {
    return false;
  }

  updatePrinterData(entityId, state);

  Serial.print("REST state: ");
  Serial.print(entityId);
  Serial.print(" = ");
  Serial.println(state);

  return true;
}

void readInitialStates() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (cfg.haUrl.isEmpty() || cfg.haToken.isEmpty()) return;

  Serial.println("Reading initial HA states...");

  int loaded = 0;
  unsigned long start = millis();

  const int totalEntities = 10;
  String entities[totalEntities] = {
    cfg.entityPhase,
    cfg.entityStatus,
    cfg.entityProgress,
    cfg.entityFilament,
    cfg.entityFinishTime,
    cfg.entityNozzleTemp,
    cfg.entityBedTemp,
    cfg.entityCurrentLayer,
    cfg.entityTotalLayers,
    cfg.entityFilamentWeight
  };

  for (int i = 0; i < totalEntities; i++) {
    if (millis() - start > initialStateTotalTimeout) {
      Serial.println("Initial HA states timeout, continuing boot.");
      showBootMessage("HA", "REST timeout");
      serviceUiDuringRest();
      break;
    }

    showBootMessage("HA", "Reading " + String(i + 1) + "/" + String(totalEntities));
    loaded += readOneInitialState(entities[i]) ? 1 : 0;
    serviceUiDuringRest();
  }

  if (loaded > 0) {
    firstHaReadDone = true;
  }

  uiDirty = true;

  Serial.print("Initial states loaded: ");
  Serial.println(loaded);
}

/**********************************************************
 * HOME ASSISTANT WEBSOCKET API
 *
 * Živé změny stavů z Home Assistantu.
 * Po připojení se přihlásí přes Long Lived Token
 * a odebírá state_changed eventy.
 **********************************************************/
void subscribeStateChanged() {
  int id = wsNextId++;
  wsSubscribeId = id;
  wsSubscribed = false;

  String msg = "{\"id\":";
  msg += String(id);
  msg += ",\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}";

  Serial.println("Subscribing to state_changed...");
  wsClient.send(msg);
}

void handleWsStateChanged(String msg) {
  DynamicJsonDocument doc(8192);

  DeserializationError error = deserializeJson(doc, msg);

  if (error) {
    Serial.print("WS JSON error: ");
    Serial.println(error.c_str());
    return;
  }

  String type = doc["type"] | "";
  if (type != "event") return;

  String eventType = doc["event"]["event_type"] | "";
  if (eventType != "state_changed") return;

  String entityId = doc["event"]["data"]["entity_id"] | "";
  String state = doc["event"]["data"]["new_state"]["state"] | "";

  if (entityId == "" || state == "") return;

  if (!isKnownEntity(entityId)) return;

  updatePrinterData(entityId, state);
  firstHaReadDone = true;
  uiDirty = true;

  Serial.print("WS update: ");
  Serial.print(entityId);
  Serial.print(" = ");
  Serial.println(state);
}

void onWsMessage(WebsocketsMessage message) {
  String msg = message.data();

  if (msg.indexOf("\"type\":\"auth_required\"") >= 0) {
    Serial.println("WS auth_required");

    String auth = "{\"type\":\"auth\",\"access_token\":\"";
    auth += cfg.haToken;
    auth += "\"}";

    wsClient.send(auth);
    return;
  }

  if (msg.indexOf("\"type\":\"auth_ok\"") >= 0) {
    Serial.println("WS AUTH OK");
    wsAuthenticated = true;
    haAuthInvalid = false;
    subscribeStateChanged();
    uiDirty = true;
    return;
  }

  if (msg.indexOf("\"type\":\"auth_invalid\"") >= 0) {
    Serial.println("WS AUTH INVALID");
    wsAuthenticated = false;
    wsSubscribed = false;
    haAuthInvalid = true;
    uiDirty = true;
    return;
  }

  if (msg.indexOf("\"type\":\"result\"") >= 0) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, msg);

    if (error) {
      Serial.print("WS result JSON error: ");
      Serial.println(error.c_str());
      return;
    }

    int id = doc["id"] | 0;
    bool success = doc["success"] | false;

    if (id == wsSubscribeId) {
      wsSubscribed = success;

      if (success) {
        Serial.println("WS SUBSCRIBE OK");
      } else {
        Serial.println("WS SUBSCRIBE FAILED");
      }

      uiDirty = true;
    }

    if (!success && doc["error"]["message"].is<const char*>()) {
      Serial.print("WS result error: ");
      Serial.println(doc["error"]["message"].as<const char*>());
    }

    return;
  }

  if (msg.indexOf("\"event_type\":\"state_changed\"") >= 0) {
    handleWsStateChanged(msg);
    return;
  }
}

void onWsEvent(WebsocketsEvent event, String eventData) {
  if (event == WebsocketsEvent::ConnectionOpened) {
    Serial.println("WebSocket connected");
    wsConnectFailed = false;
    uiDirty = true;
  }

  if (event == WebsocketsEvent::ConnectionClosed) {
    Serial.println("WebSocket disconnected");
    wsAuthenticated = false;
    wsSubscribed = false;
    wsConnectFailed = true;
    uiDirty = true;
  }

  if (event == WebsocketsEvent::GotPing) {
    Serial.println("WS Ping");
  }

  if (event == WebsocketsEvent::GotPong) {
    Serial.println("WS Pong");
  }
}

bool connectWebSocket() {
  if (cfg.haUrl.isEmpty() || cfg.haToken.isEmpty()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = buildWsUrl();

  Serial.print("Connecting WebSocket: ");
  Serial.println(url);

  wsClient.onMessage(onWsMessage);
  wsClient.onEvent(onWsEvent);

  bool ok = wsClient.connect(url);

  if (ok) {
    Serial.println("WS connect OK");
    wsConnectFailed = false;
    return true;
  }

  Serial.println("WS connect failed");
  wsConnectFailed = true;
  return false;
}

/**********************************************************
 * PRINTER STATE HELPERS
 **********************************************************/
bool statusPaused() {
  return data.status == "pause" ||
         data.status == "Pause" ||
         data.status == "paused" ||
         data.status == "Paused";
}

bool objectDetectionPhase() {
  return data.phase == "heatbed_surface_foreign_object_detection" ||
         data.phase == "heatbed_underside_foreign_object_detection";
}

bool objectDetection() {
  return objectDetectionPhase() && statusPaused();
}

bool isOfflineState() {
  return data.phase == "offline" ||
         data.phase == "Offline" ||
         data.phase == "unavailable";
}

bool isIdleOrOffline() {
  return data.phase == "idle" ||
         data.phase == "Idle" ||
         isOfflineState();
}

bool isFinishRaw() {
  bool finishStatus = data.status == "finish" || data.status == "Finish";

  if (!finishStatus) return false;
  if (isnan(data.progress)) return false;
  if (data.progress < 99.5) return false;

  return true;
}

bool isPrinting() {
  return (data.phase == "printing" ||
          data.phase == "Printing" ||
          statusPaused()) && !objectDetection();
}

float realProgress() {
  if (isnan(data.progress)) return 0;

  float progress = data.progress;

  if (progress < 0) progress = 0;
  if (progress > 100) progress = 100;

  return progress;
}

/**********************************************************
 * DATE / TIME TEXT HELPERS
 **********************************************************/
String currentTimeText() {
  time_t now = time(nullptr);

  if (now < 100000) return "--:--";

  struct tm* t = localtime(&now);

  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", t->tm_hour, t->tm_min);
  return String(buffer);
}

String currentDateText() {
  time_t now = time(nullptr);

  if (now < 100000) return "";

  struct tm* t = localtime(&now);

  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%02d.%02d.%04d", t->tm_mday, t->tm_mon + 1, t->tm_year + 1900);
  return String(buffer);
}

long daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

bool isDigitAt(String value, int index) {
  return index >= 0 &&
         index < (int)value.length() &&
         value[index] >= '0' &&
         value[index] <= '9';
}

int twoDigitsAt(String value, int index) {
  if (!isDigitAt(value, index) || !isDigitAt(value, index + 1)) return -1;
  return (value[index] - '0') * 10 + (value[index + 1] - '0');
}

int fourDigitsAt(String value, int index) {
  if (!isDigitAt(value, index) ||
      !isDigitAt(value, index + 1) ||
      !isDigitAt(value, index + 2) ||
      !isDigitAt(value, index + 3)) {
    return -1;
  }

  return (value[index] - '0') * 1000 +
         (value[index + 1] - '0') * 100 +
         (value[index + 2] - '0') * 10 +
         (value[index + 3] - '0');
}

String finishTimeText() {
  String value = data.finishTime;
  value.trim();

  if (value.isEmpty() || value == "unknown" || value == "unavailable") return "";

  if (value.length() == 5 &&
      isDigitAt(value, 0) &&
      isDigitAt(value, 1) &&
      value[2] == ':' &&
      isDigitAt(value, 3) &&
      isDigitAt(value, 4)) {
    return value;
  }

  if (value.length() >= 19 && value[4] == '-' && value[7] == '-' && value[10] == 'T') {
    int year = fourDigitsAt(value, 0);
    int month = twoDigitsAt(value, 5);
    int day = twoDigitsAt(value, 8);
    int hour = twoDigitsAt(value, 11);
    int minute = twoDigitsAt(value, 14);
    int second = twoDigitsAt(value, 17);

    if (year > 1970 && month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
        hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59) {
      int offsetSeconds = 0;

      if (value.length() >= 25 && (value[19] == '+' || value[19] == '-')) {
        int offsetHours = twoDigitsAt(value, 20);
        int offsetMinutes = twoDigitsAt(value, 23);

        if (offsetHours >= 0 && offsetMinutes >= 0) {
          offsetSeconds = offsetHours * 3600 + offsetMinutes * 60;
          if (value[19] == '-') offsetSeconds = -offsetSeconds;
        }
      }

      long days = daysFromCivil(year, (unsigned)month, (unsigned)day);
      time_t utc = (time_t)((long long)days * 86400LL + hour * 3600L + minute * 60L + second - offsetSeconds);
      struct tm* local = localtime(&utc);

      char buffer[16];
      snprintf(buffer, sizeof(buffer), "%02d:%02d", local->tm_hour, local->tm_min);
      return String(buffer);
    }
  }

  return value;
}

String filamentText() {
  if (data.filament == "Empty" ||
      data.filament == "unknown" ||
      data.filament == "unavailable" ||
      data.filament == "") {
    return "No filament";
  }

  return data.filament;
}

/**********************************************************
 * PREPARING PHASE TRANSLATIONS
 *
 * Převod interních Bambu Lab fází na čitelný text
 * pro displej.
 **********************************************************/
String preparingText() {
  if (isPrinting()) return "";
  if (isIdleOrOffline()) return "";

  if (objectDetectionPhase()) {
    if (statusPaused()) return "Object\nDetected";
    return "Object\nDetection";
  }

  String phase = data.phase;

  if (phase == "preparing_hotend") return "Preparing\nHotend";
  if (phase == "heating_hotend") return "Heating\nHotend";
  if (phase == "heated_temperature") return "Heated\nTemp";
  if (phase == "heatbed_preheating") return "Bed\nPreheating";
  if (phase == "heating_chamber") return "Heating\nChamber";
  if (phase == "waiting_chamber_temperature_equalize") return "Chamber\nTemp\nEqualize";
  if (phase == "preparing_ams") return "Preparing\nAMS";
  if (phase == "changing_filament") return "Changing\nFilament";
  if (phase == "filament_loading") return "Filament\nLoading";
  if (phase == "filament_unloading") return "Filament\nUnloading";
  if (phase == "check_material") return "Check\nMaterial";
  if (phase == "check_material_position") return "Check\nMaterial\nPosition";
  if (phase == "identifying_build_plate_type") return "Scanning\nBuildplate\nType";
  if (phase == "check_platform") return "Check\nPlatform";
  if (phase == "check_plaform") return "Check\nPlatform";
  if (phase == "check_absolute_accuracy_before_calibration") return "Check\nAccuracy";
  if (phase == "check_absolute_accuracy_after_calibration") return "Check\nAccuracy";
  if (phase == "absolute_accuracy_calibration") return "Accuracy\nCalibration";
  if (phase == "calibrate_nozzle_offset") return "Nozzle\nOffset\nCalibration";
  if (phase == "calibrating_extrusion_flow") return "Flow\nCalibration";
  if (phase == "calibrating_extrusion") return "Extrusion\nCalibration";
  if (phase == "calibrating_live_view_camera") return "Live View\nCamera\nCalibration";
  if (phase == "calibrating_camera_offset") return "Camera\nOffset\nCalibration";
  if (phase == "laser_calibration") return "Laser\nCalibration";
  if (phase == "calibrating_micro_lidar") return "MicroLidar\nCalibration";
  if (phase == "calibrating_blade_holder_position") return "Blade\nHolder\nCalibration";
  if (phase == "calibrating_cutter_model_offset") return "Cutter\nModel\nCalibration";
  if (phase == "calibrating_detection_nozzle_clumping") return "Nozzle\nCheck";
  if (phase == "cleaning_nozzle_tip") return "Cleaning\nNozzle\nTip";
  if (phase == "cooling_nozzle") return "Cooling\nNozzle";
  if (phase == "purifying_chamber_air") return "Purifying\nChamber\nAir";
  if (phase == "homing_toolhead") return "Homing\nToolhead";
  if (phase == "moving_toolhead_to_center_of_heatbed") return "Toolhead\nPositioning";
  if (phase == "moving_toolhead_above_purge_chute") return "Toolhead\nPositioning";
  if (phase == "bed_level_phase_1") return "Automatic\nBed\nLeveling";
  if (phase == "bed_level_phase_2") return "Automatic\nBed\nLeveling";
  if (phase == "bed_level_high_temperature") return "Bed Level\nHigh\nTemp";
  if (phase == "calibrating_motor_noise") return "Motor\nNoise\nCalibration";
  if (phase == "active_arc_fitting") return "Active\nArc\nFitting";
  if (phase == "measuring_surface") return "Measuring\nSurface";
  if (phase == "measuring_rotary_attachment") return "Measuring\nRotary\nAttachment";
  if (phase == "build_plate_alignment_detection") return "Plate\nAlignment\nDetection";
  if (phase == "inspecting_first_layer") return "First\nLayer\nInspect";
  if (phase == "scanning_bed_surface") return "Bed\nSurface\nScanning";
  if (phase == "check_quick_release") return "Check\nQuick\nRelease";
  if (phase == "pre_extrusion_before_printing") return "Pre\nExtrusion";
  if (phase == "heated_bedcooling") return "Bed\nCooling";
  if (phase == "check_birdeye_camera_position") return "Check\nBirdEye\nCamera";
  if (phase == "homing_blade_holder") return "Blade\nHolder\nHoming";
  if (phase == "auto_bed_leveling") return "Automatic\nBed\nLeveling";
  if (phase == "check_door_and_cover") return "Check\nDoor";
  if (phase == "hotend_type_detection") return "Hotend\nType\nDetection";
  if (phase == "waiting_for_heatbed_temperature") return "Waiting\nfor\nBed Temp";
  if (phase == "print_calibration_lines") return "Print\nCalibration\nLine";
  if (phase == "hotend_pick_place_test") return "Hotend\nTest";
  if (phase == "motor_noise_showoff") return "Motor\nNoise\nShowoff";
  if (phase == "paused_skipped_step") return "Paused\nSkipped\nStep";
  if (phase == "checking_extruder_temperature") return "Extruder\nTemp\nCheck";
  if (phase == "paused_nozzle_filament_covered_detected") return "Nozzle\nError";
  if (phase == "calibrate_birdeye_camera") return "Calibration\nBirdEye\nCamera";
  if (phase == "paused_low_fan_speed_heat_break") return "Fan\nError";
  if (phase == "cooling_chamber") return "Cooling\nChamber";
  if (phase == "sweeping_xy_mech_mode") return "XY\nVibration\nSweeping";
  if (phase == "thermal_preconditioning") return "Thermal\nPrecondition.";

  return "Preparing";
}

/**********************************************************
 * SERVICE STATUS HELPERS
 **********************************************************/
bool haConfigured() {
  return !cfg.haUrl.isEmpty() && !cfg.haToken.isEmpty();
}

bool hasPrinterData() {
  return data.phase.length() > 0 ||
         data.status.length() > 0 ||
         !isnan(data.progress);
}

String portalAccessText() {
  String text = apSsid;
  text += " WiFi\nPASS: ";
  text += apPass;
  text += "\nIP: ";
  text += WiFi.softAPIP().toString();
  return text;
}

String serviceStatusTitle() {
  if (otaActive) return "OTA Update";
  if (portalMode) return "Connect to";
  if (WiFi.status() != WL_CONNECTED) return "WiFi Lost";
  if (!haConfigured()) return "Setup HA";
  if (haAuthInvalid) return "HA Auth";
  if (!hasPrinterData() && wsConnectFailed) return "WS Reconnect";
  return "";
}

String serviceStatusDetail() {
  if (otaActive) return String(otaProgress) + "%";
  if (portalMode) return portalAccessText();
  if (WiFi.status() != WL_CONNECTED) return "Reconnecting";
  if (!haConfigured()) return "Open config";
  if (haAuthInvalid) return "Check token";
  if (!hasPrinterData() && wsConnectFailed) return "Retrying";
  return "";
}

bool serviceStatusActive() {
  return serviceStatusTitle().length() > 0;
}

/**********************************************************
 * STATUS TEXT
 **********************************************************/
String statusText() {
  String serviceTitle = serviceStatusTitle();
  if (portalMode) return "Setup";
  if (serviceTitle.length() > 0) return otaActive ? "OTA Update" : "Service";

  if (finishDisplayActive()) return "Finish";
  if (statusPaused() && !objectDetection()) return "Paused";
  if (isOfflineState()) return "Offline";
  if (data.phase == "idle" || data.phase == "Idle") return "Ready";
  if (isPrinting()) return "Printing";
  return "Preparing";
}

/**********************************************************
 * FINISH LOGIC
 *
 * Po dokončení zobrazí krátce obrazovku Finish.
 **********************************************************/
void updateFinishLogic() {
  if (!firstHaReadDone) return;

  bool printingNow = isPrinting();
  bool finishNow = isFinishRaw();

  if (printingNow) {
    printWasActive = true;
  }

  if (finishNow && printWasActive && !finishSeen) {
    finishDisplayUntilMs = millis() + 30000UL;
    finishSeen = true;
    printWasActive = false;
  }

  if (!finishNow) {
    finishSeen = false;
  }

  if (finishDisplayUntilMs > 0 && !finishDisplayActive()) {
    finishDisplayUntilMs = 0;
  }

}

/**********************************************************
 * DISPLAY LAYOUTS
 *
 * Samostatné pozice pro každou obrazovku.
 * Díky tomu lze ladit Ready / Printing / Preparing
 * bez rozbití ostatních obrazovek.
 **********************************************************/
void layoutClockScreen() {
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_align(center_label, LV_ALIGN_TOP_MID, 0, 82);
  lv_obj_align(bottom_label, LV_ALIGN_TOP_MID, 0, 145);
}

void layoutPrintingScreen() {
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 46);
  lv_obj_align(center_label, LV_ALIGN_TOP_MID, 0, 78);
  lv_obj_align(bottom_label, LV_ALIGN_TOP_MID, 0, 140);
  lv_obj_align(temp_row, LV_ALIGN_TOP_MID, 0, 164);
  lv_obj_align(detail_label, LV_ALIGN_TOP_MID, 0, 194);
}

void layoutPreparingScreen() {
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 46);
  lv_obj_align(preparing_label, LV_ALIGN_TOP_MID, 0, 82);
}

void layoutFinishScreen() {
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_align(center_label, LV_ALIGN_TOP_MID, 0, 82);
}

void layoutServiceScreen() {
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 46);
  lv_obj_align(preparing_label, LV_ALIGN_TOP_MID, 0, 86);
  lv_obj_align(bottom_label, LV_ALIGN_TOP_MID, 0, 156);
}

void layoutSetupScreen() {
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 42);
  lv_obj_align(preparing_label, LV_ALIGN_TOP_MID, 0, 78);
  lv_obj_align(bottom_label, LV_ALIGN_TOP_MID, 0, 128);
}

void setTempRowVisible(bool visible) {
  if (temp_row == nullptr) return;

  if (visible) {
    lv_obj_clear_flag(temp_row, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(temp_row, LV_OBJ_FLAG_HIDDEN);
  }
}

int animatedArcValue(int target, bool smooth) {
  if (!smooth) {
    displayedArcValue = target;
    arcSmoothWasActive = false;
    return target;
  }

  if (!arcSmoothWasActive) {
    displayedArcValue = target;
    arcSmoothWasActive = true;
    return target;
  }

  float diff = target - displayedArcValue;

  float diffAbs = diff < 0 ? -diff : diff;

  if (diffAbs < 0.35) {
    displayedArcValue = target;
  } else {
    float step = diff * 0.28;

    if (step > 0 && step < 0.6) step = 0.6;
    if (step < 0 && step > -0.6) step = -0.6;

    displayedArcValue += step;
  }

  if (displayedArcValue < 0) displayedArcValue = 0;
  if (displayedArcValue > 100) displayedArcValue = 100;

  return (int)round(displayedArcValue);
}

void serviceDisplayDuringSetup(uint16_t waitMs) {
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;
  lv_timer_handler();
  delay(waitMs);
}

void showBootMessage(String title, String detail) {
  if (status_label == nullptr) return;

  layoutServiceScreen();

  displayedArcValue = 0;
  arcSmoothWasActive = false;
  lv_arc_set_value(progress_arc, 0);
  lv_obj_set_style_arc_color(progress_arc, lv_color_hex(0x0A84FF), LV_PART_INDICATOR);

  lv_label_set_text(status_label, title.c_str());
  lv_label_set_text(center_label, "");
  lv_label_set_text(preparing_label, detail.c_str());
  lv_label_set_text(bottom_label, "");
  lv_label_set_text(temps_label, "");
  lv_label_set_text(detail_label, "");
  setTempRowVisible(false);

  lv_obj_add_flag(center_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(preparing_label, LV_OBJ_FLAG_HIDDEN);

  serviceDisplayDuringSetup(5);
}

/**********************************************************
 * LVGL UI SETUP
 *
 * Vytvoření všech prvků na displeji.
 **********************************************************/
void setupLvglUi() {
  lv_obj_t *screen = lv_screen_active();

  lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  progress_arc = lv_arc_create(screen);
  lv_obj_set_size(progress_arc, 210, 210);
  lv_obj_align(progress_arc, LV_ALIGN_CENTER, 0, 0);

  lv_arc_set_range(progress_arc, 0, 100);
  lv_arc_set_value(progress_arc, 0);
  lv_arc_set_bg_angles(progress_arc, 135, 45);
  lv_obj_remove_style(progress_arc, NULL, LV_PART_KNOB);

  lv_obj_set_style_arc_width(progress_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(progress_arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(progress_arc, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_obj_set_style_arc_color(progress_arc, lv_color_hex(0x00AE42), LV_PART_INDICATOR);

  status_label = lv_label_create(screen);
  lv_label_set_text(status_label, "Starting");
  lv_obj_set_width(status_label, 220);
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 50);

  center_label = lv_label_create(screen);
  lv_label_set_text(center_label, "--:--");
  lv_obj_set_width(center_label, 240);
  lv_obj_set_style_text_align(center_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(center_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(center_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_align(center_label, LV_ALIGN_TOP_MID, 0, 82);

  preparing_label = lv_label_create(screen);
  lv_label_set_text(preparing_label, "");
  lv_obj_set_width(preparing_label, 220);
  lv_obj_set_style_text_align(preparing_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(preparing_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(preparing_label, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(preparing_label, LV_ALIGN_TOP_MID, 0, 82);

  bottom_label = lv_label_create(screen);
  lv_label_set_text(bottom_label, "");
  lv_obj_set_width(bottom_label, 210);
  lv_obj_set_style_text_align(bottom_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(bottom_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(bottom_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(bottom_label, 2, LV_PART_MAIN);
  lv_obj_align(bottom_label, LV_ALIGN_TOP_MID, 0, 140);

  temps_label = lv_label_create(screen);
  lv_label_set_text(temps_label, "");
  lv_obj_set_width(temps_label, 220);
  lv_obj_set_style_text_align(temps_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(temps_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(temps_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_align(temps_label, LV_ALIGN_TOP_MID, 0, 164);
  lv_obj_add_flag(temps_label, LV_OBJ_FLAG_HIDDEN);

  temp_row = lv_obj_create(screen);
  lv_obj_remove_style_all(temp_row);
  lv_obj_set_size(temp_row, 160, 24);
  lv_obj_align(temp_row, LV_ALIGN_TOP_MID, 0, 164);
  lv_obj_add_flag(temp_row, LV_OBJ_FLAG_HIDDEN);

  nozzle_icon = lv_line_create(temp_row);
  lv_line_set_points(nozzle_icon, nozzleIconPoints, sizeof(nozzleIconPoints) / sizeof(nozzleIconPoints[0]));
  lv_obj_set_size(nozzle_icon, 16, 16);
  lv_obj_set_pos(nozzle_icon, 13, 4);
  lv_obj_set_style_line_width(nozzle_icon, 2, LV_PART_MAIN);
  lv_obj_set_style_line_color(nozzle_icon, lv_color_hex(0x00AE42), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(nozzle_icon, true, LV_PART_MAIN);

  nozzle_temp_label = lv_label_create(temp_row);
  lv_label_set_text(nozzle_temp_label, "--°");
  lv_obj_set_width(nozzle_temp_label, 48);
  lv_obj_set_pos(nozzle_temp_label, 33, 0);
  lv_obj_set_style_text_align(nozzle_temp_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_style_text_color(nozzle_temp_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(nozzle_temp_label, &lv_font_montserrat_20, LV_PART_MAIN);

  bed_icon = lv_line_create(temp_row);
  lv_line_set_points(bed_icon, bedIconPoints, sizeof(bedIconPoints) / sizeof(bedIconPoints[0]));
  lv_obj_set_size(bed_icon, 18, 14);
  lv_obj_set_pos(bed_icon, 86, 5);
  lv_obj_set_style_line_width(bed_icon, 2, LV_PART_MAIN);
  lv_obj_set_style_line_color(bed_icon, lv_color_hex(0x00AE42), LV_PART_MAIN);
  lv_obj_set_style_line_rounded(bed_icon, true, LV_PART_MAIN);

  bed_temp_label = lv_label_create(temp_row);
  lv_label_set_text(bed_temp_label, "--°");
  lv_obj_set_width(bed_temp_label, 48);
  lv_obj_set_pos(bed_temp_label, 108, 0);
  lv_obj_set_style_text_align(bed_temp_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_style_text_color(bed_temp_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(bed_temp_label, &lv_font_montserrat_20, LV_PART_MAIN);

  detail_label = lv_label_create(screen);
  lv_label_set_text(detail_label, "");
  lv_obj_set_width(detail_label, 220);
  lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_color(detail_label, lv_color_hex(0x00AE42), LV_PART_MAIN);
  lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_align(detail_label, LV_ALIGN_TOP_MID, 0, 194);
}

/**********************************************************
 * DISPLAY UPDATE
 *
 * Přepočítá aktuální obrazovku podle dat tiskárny.
 *
 * ARC barvy:
 * - zelená  = Ready / Printing
 * - modrá   = Preparing
 * - oranžová = Paused
 * - červená = Object detection / error
 * - šedá    = Offline
 **********************************************************/
void updateUi() {
  updateFinishLogic();

  uint32_t nowSec = currentSeconds();
  bool finishActive = finishDisplayActive();
  bool serviceActive = serviceStatusActive();
  String serviceTitle = serviceStatusTitle();

  int arcValue = 0;
  bool smoothArc = false;
  lv_color_t arcColor = lv_color_hex(0x00AE42);

  if (serviceActive) {
    if (otaActive) {
      arcValue = otaProgress;
      arcColor = lv_color_hex(0x00AE42);
    } else if (serviceTitle == "WS Reconnect") {
      int s = nowSec % 20;
      arcValue = s <= 10 ? s * 10 : (20 - s) * 10;
      arcColor = lv_color_hex(0x0A84FF);
    } else {
      arcValue = 100;
      arcColor = (nowSec % 2 == 0) ? lv_color_hex(0xF5272A) : lv_color_hex(0x303030);
    }

  } else if (finishActive) {
    arcValue = 100;
    arcColor = (nowSec % 2 == 0) ? lv_color_hex(0x00AE42) : lv_color_hex(0x303030);

  } else if (objectDetection()) {
    arcValue = 100;
    arcColor = (nowSec % 2 == 0) ? lv_color_hex(0xF5272A) : lv_color_hex(0x303030);

  } else if (statusPaused()) {
    arcValue = (int)realProgress();
    smoothArc = true;
    arcColor = (nowSec % 2 == 0) ? lv_color_hex(0xFF9500) : lv_color_hex(0x303030);

  } else if (isPrinting()) {
    arcValue = (int)realProgress();
    smoothArc = true;
    arcColor = lv_color_hex(0x00AE42);

  } else if (isOfflineState()) {
    arcValue = ((nowSec % 60) * 100) / 59;
    arcColor = lv_color_hex(0x8E8E93);

  } else if (isIdleOrOffline()) {
    arcValue = ((nowSec % 60) * 100) / 59;
    arcColor = lv_color_hex(0x00AE42);

  } else {
    int s = nowSec % 20;
    arcValue = s <= 10 ? s * 10 : (20 - s) * 10;
    arcColor = lv_color_hex(0x0A84FF);
  }

  lv_arc_set_value(progress_arc, animatedArcValue(arcValue, smoothArc));
  lv_obj_set_style_arc_color(progress_arc, arcColor, LV_PART_INDICATOR);

  lv_label_set_text(status_label, statusText().c_str());
  setTempRowVisible(false);

  if (serviceActive) {
    if (portalMode) {
      layoutSetupScreen();
    } else {
      layoutServiceScreen();
    }

    lv_label_set_text(center_label, "");
    lv_label_set_text(preparing_label, serviceTitle.c_str());
    lv_label_set_text(bottom_label, serviceStatusDetail().c_str());
    lv_label_set_text(temps_label, "");
    lv_label_set_text(detail_label, "");

    lv_obj_add_flag(center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(preparing_label, LV_OBJ_FLAG_HIDDEN);

  } else if (finishActive) {
    layoutFinishScreen();

    lv_label_set_text(center_label, "100%");
    lv_label_set_text(preparing_label, "");
    lv_label_set_text(bottom_label, "");
    lv_label_set_text(temps_label, "");
    lv_label_set_text(detail_label, "");

    lv_obj_clear_flag(center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preparing_label, LV_OBJ_FLAG_HIDDEN);

  } else if (isPrinting()) {
    layoutPrintingScreen();

    String p = String((int)round(realProgress())) + "%";
    lv_label_set_text(center_label, p.c_str());

    lv_label_set_text(preparing_label, "");
    lv_label_set_text(bottom_label, filamentText().c_str());

    String nozzle = isnan(data.nozzleTemp) ? "--°" : String((int)round(data.nozzleTemp)) + "°";
    String bed = isnan(data.bedTemp) ? "--°" : String((int)round(data.bedTemp)) + "°";
    lv_label_set_text(nozzle_temp_label, nozzle.c_str());
    lv_label_set_text(bed_temp_label, bed.c_str());
    setTempRowVisible(true);

    int screen = nowSec % 9;

    String finish = finishTimeText();

    if (screen < 3 && finish.length() > 0) {
      lv_label_set_text(detail_label, finish.c_str());
    } else if (screen < 6 && !isnan(data.currentLayer) && !isnan(data.totalLayers)) {
      String layer = String((int)data.currentLayer) + " / " + String((int)data.totalLayers);
      lv_label_set_text(detail_label, layer.c_str());
    } else if (!isnan(data.filamentWeight)) {
      String weight = String(data.filamentWeight, 0) + " g";
      lv_label_set_text(detail_label, weight.c_str());
    } else {
      lv_label_set_text(detail_label, "");
    }

    lv_obj_clear_flag(center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preparing_label, LV_OBJ_FLAG_HIDDEN);

  } else if (isIdleOrOffline()) {
    layoutClockScreen();

    lv_label_set_text(center_label, currentTimeText().c_str());
    lv_label_set_text(preparing_label, "");
    lv_label_set_text(bottom_label, currentDateText().c_str());
    lv_label_set_text(temps_label, "");
    lv_label_set_text(detail_label, "");

    lv_obj_clear_flag(center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preparing_label, LV_OBJ_FLAG_HIDDEN);

  } else {
    layoutPreparingScreen();

    lv_label_set_text(center_label, "");
    lv_label_set_text(preparing_label, preparingText().c_str());
    lv_label_set_text(bottom_label, "");
    lv_label_set_text(temps_label, "");
    lv_label_set_text(detail_label, "");

    lv_obj_add_flag(center_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(preparing_label, LV_OBJ_FLAG_HIDDEN);
  }
}

/**********************************************************
 * TIME SETUP
 **********************************************************/
void setupTime() {
  normalizeTimeZoneConfig();
  configTzTime(cfg.tzPosix.c_str(), "pool.ntp.org", "time.nist.gov");
}

/**********************************************************
 * OTA UPDATE SETUP
 **********************************************************/
void setupOta() {
  if (portalMode || WiFi.status() != WL_CONNECTED) return;

  ArduinoOTA.setHostname("circle-bambu-monitor");

  if (!cfg.otaPass.isEmpty()) {
    ArduinoOTA.setPassword(cfg.otaPass.c_str());
  }

  ArduinoOTA.onStart([]() {
    otaActive = true;
    otaProgress = 0;
    uiDirty = true;
    Serial.println("OTA start");
  });

  ArduinoOTA.onEnd([]() {
    otaProgress = 100;
    uiDirty = true;
    Serial.println("OTA end");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total > 0) {
      otaProgress = (progress * 100) / total;
      uiDirty = true;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaActive = false;
    uiDirty = true;

    Serial.print("OTA error: ");
    Serial.println((int)error);
  });

  ArduinoOTA.begin();
  otaReady = true;

  Serial.println("OTA ready");
}

/**********************************************************
 * DISPLAY SETUP
 **********************************************************/
void setupDisplay() {
  spi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(GC9A01A_BLACK);

  lv_init();

  disp = lv_display_create(240, 240);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(
    disp,
    draw_buf,
    NULL,
    sizeof(draw_buf),
    LV_DISPLAY_RENDER_MODE_PARTIAL
  );

  setupLvglUi();

  lastTick = millis();
  serviceDisplayDuringSetup(5);
}

/**********************************************************
 * SETUP
 **********************************************************/
void setup() {
  Serial.begin(115200);
  delay(800);

  setupDisplay();
  showBootMessage("Starting", "Loading config");
  loadConfig();

  bool wifiOk = connectWifi();

  if (!wifiOk) {
    startPortal();
    updateUi();
    serviceDisplayDuringSetup(5);
  }

  if (wifiOk) {
    showBootMessage("Time", "Syncing clock");
    setupTime();
    showBootMessage("OTA", "Starting");
    setupOta();

    if (haConfigured()) {
      showBootMessage("HA", "Reading data");
      readInitialStates();
      showBootMessage("HA", "WebSocket");
      connectWebSocket();
    } else {
      showBootMessage("Setup HA", "Open config");
    }
  }

  updateUi();
  serviceDisplayDuringSetup(5);
}

/**********************************************************
 * MAIN LOOP
 *
 * Důležité:
 * wsClient.poll() se volá vždy, pokud je WiFi připojená.
 * Tím se WebSocket nepřestane obsluhovat.
 *
 * Pokud WebSocket spadne:
 * - znovu se připojí
 * - načte aktuální stav přes REST
 * - vynutí refresh displeje
 **********************************************************/
void loop() {
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;

  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    if (otaReady) {
      ArduinoOTA.handle();
    }

    if (otaActive) {
      if (uiDirty || millis() - lastUiUpdate > uiUpdateInterval) {
        uiDirty = false;
        lastUiUpdate = millis();
        updateUi();
      }

      lv_timer_handler();
      delay(5);
      return;
    }

    wsClient.poll();
    if (!wsClient.available() && millis() - lastWsReconnectAttempt > wsReconnectInterval) {
      lastWsReconnectAttempt = millis();

      wsAuthenticated = false;
      wsSubscribed = false;

      if (connectWebSocket()) {
        readInitialStates();
      }

      uiDirty = true;
    }
  }

  if (uiDirty || millis() - lastUiUpdate > uiUpdateInterval) {
    uiDirty = false;
    lastUiUpdate = millis();
    updateUi();
  }

  lv_timer_handler();
  delay(5);
}
