/*
  WebServer.cpp - diagnostics web UI

  Copyright (C) 2026 @steadramon

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "WebServer.h"

#include <Arduino.h>
#include <Update.h>
#include <Version.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "../CrashDump/CrashDump.h"
#include "../Display/Display.h"
#include "../MontePi/MontePi.h"

// Build-time gzip blobs; fall back to the raw R"..." bodies on extract miss.
#include "INDEX_JS.gz.h"
#include "STYLE_CSS.gz.h"
#include "../Poller/Poller.h"
#include "../Stations/Stations.h"
#include "../UdpRx/UdpRx.h"
#include "../Ui/Ui.h"
#include "../board.h"

namespace WebSrv {

// Falls back to the static GADGET_VERSION if scripts/git_rev.py didn't fire
// (clean checkout without git, CI export, etc).
#ifndef GIT_VERSION
#define GIT_VERSION GADGET_VERSION
#endif
#define EG_CACHE_BUST "?v=" GIT_VERSION

namespace {

constexpr const char * kVersion = GIT_VERSION;

WebServer s_http(80);
bool      s_inited = false;

void send_text(int code, const char * mime, const char * body) {
  s_http.send(code, mime, body);
}

// Serves a PROGMEM gzip blob with Content-Encoding: gzip + long Cache-Control.
void send_gzip_p(int code, const char * mime, const uint8_t * blob, size_t len) {
  s_http.sendHeader("Content-Encoding", "gzip");
  s_http.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  s_http.setContentLength(len);
  s_http.send(code, mime, "");
  s_http.sendContent_P(reinterpret_cast<const char *>(blob), len);
}

// Shared dark theme + chrome for /, /update, /crash. Gzipped at build
// time (scripts/gzip_assets.py). The raw body is compiled in only as a
// fallback when the script extract-misses. Served from /style.css.
#if !EG_GZ_STYLE_CSS
static const char STYLE_CSS[] PROGMEM = R"CSS(
:root{--bg:#212529;--page:#1a1d20;--fg:#dee2e6;--muted:#adb5bd;
--border:#373b3e;--accent:#06c;--card:#2b3035;--ok:#5a5;--warn:#ffbb22;--err:#c0392b}
*{box-sizing:border-box}
html{background:var(--page);min-height:100%}
body{font:16px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
max-width:780px;margin:1.5em auto;padding:1.5em 1em;color:var(--fg);
background:var(--bg);border-radius:8px;box-shadow:0 1px 3px #0000001a}
@media(max-width:640px){body{margin:0 auto;border-radius:0;box-shadow:none}}
body.narrow{max-width:520px}
h1{margin:0 0 1em;font-size:1.65em;font-weight:700;display:flex;
align-items:center;gap:.5em}
h1 svg{width:1.4em;height:1.4em;flex:0 0 auto}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
table{width:100%;border-collapse:collapse;margin:1em 0}
th,td{text-align:left;padding:.5em .6em;border-bottom:1px solid var(--border)}
th{font-weight:500;color:var(--muted);width:9em}
.mn{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.tag.ok{color:var(--ok)}.tag.warn{color:var(--warn)}.tag.err{color:var(--err)}
.ok{color:#5fc06a}.bad{color:#ff5555}
.menu a{display:inline-block;background:var(--accent);color:#fff;padding:.5em 1em;
border-radius:6px;margin-top:1em;font-weight:500}
.back{display:inline-block;margin-top:1em;background:var(--accent);color:#fff;
padding:.5em 1em;border-radius:6px;font-weight:500;text-decoration:none}
.back:hover{opacity:.9;text-decoration:none}
.brand{margin-top:2em;font-size:.85em;color:var(--muted)}
.brand a{color:var(--fg);font-weight:600;text-decoration:none}
.brand a:hover{text-decoration:underline}
.warn{background:#4a1a1a;color:#ffaaaa;padding:.75em 1em;border-radius:6px;
margin:1em 0;font-size:.95em}
pre{background:var(--card);border:1px solid var(--border);border-radius:6px;
padding:.75em 1em;overflow:auto;font:13px/1.4 ui-monospace,monospace}
input[type=file]{display:block;width:100%;max-width:100%;padding:.6em;margin:1em 0;
background:var(--card);color:var(--fg);border:1px solid var(--border);
border-radius:6px;font-size:1em;overflow:hidden;text-overflow:ellipsis;
white-space:nowrap}
button{padding:.6em 1em;background:var(--accent);color:#fff;border:0;
border-radius:6px;font-size:1em;font-weight:500;cursor:pointer}
button:disabled{background:var(--border);cursor:not-allowed}
button.danger{background:#4a1a1a;color:#ffaaaa}
form.primary button{display:block;width:100%;padding:.75em;font-size:1.05em;font-weight:600}
progress{display:block;width:100%;margin-top:1em;height:20px}
#st{margin-top:.5em;font-family:ui-monospace,monospace;color:var(--muted)}
)CSS";
#endif  // !EG_GZ_STYLE_CSS

// Polled cells get IDs exposed via /stats.json. The JS body is parsed
// out by scripts/gzip_assets.py and emitted as INDEX_JS_GZ; the raw body
// is only compiled in when the script extract-misses. Served from /app.js.
#if !EG_GZ_INDEX_JS
static const char INDEX_JS[] PROGMEM = R"JS(
var $=function(i){return document.getElementById(i)};
var els={up:$('tup'),heap:$('theap'),min:$('thmin'),big:$('thbig'),
cnt:$('tcount'),hl:$('thealth'),pi:$('tpi'),rate:$('trate')};
var lP=null,lU=null,lT=null;
function fu(s){if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m';
if(s<86400){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);
return h+'h '+(m<10?'0':'')+m+'m';}
var d=Math.floor(s/86400),h=Math.floor((s%86400)/3600);
return d+'d '+(h<10?'0':'')+h+'h';}
var P='3.141592653589793';
function poll(){fetch('/stats.json').then(function(r){return r.json()}).then(function(d){
els.up.textContent=fu(d.uptime_s);
els.heap.textContent=d.heap.free.toLocaleString()+' B';
els.min.textContent=d.heap.min.toLocaleString()+' B';
els.big.textContent=d.heap.big.toLocaleString()+' B';
els.cnt.textContent=d.stations+' / '+d.max+' ('+d.udp_count+' via UDP)';
els.hl.innerHTML=d.faults?'<span class="tag err">FAULT</span>':
(d.stations?'<span class="tag ok">healthy</span>':'<span class="tag warn">scanning</span>');
if(d.pi&&d.pi.t>0){
var est=(4*d.pi.i/d.pi.t).toFixed(8),m=0;
while(m<est.length&&m<P.length&&est[m]===P[m])m++;
els.pi.innerHTML='<span style="color:#5fc06a">'+est.substring(0,m)+
'</span><span style="color:#ff5555">'+est.substring(m)+'</span>';
els.pi.title=d.pi.d.toLocaleString()+' clicks / '+d.pi.t.toLocaleString()+' samples';
}else els.pi.textContent='waiting for clicks...';
var now=Date.now();
if(lP!==null){var dt=(now-lT)/1000;
els.rate.textContent=((d.polls-lP)/dt).toFixed(1)+' poll/s, '+
((d.udp-lU)/dt).toFixed(1)+' udp/s';}
lP=d.polls;lU=d.udp;lT=now;
}).catch(function(){});}
poll();setInterval(function(){if(!document.hidden)poll()},3000);
)JS";
#endif  // !EG_GZ_INDEX_JS

void format_uptime(uint32_t s, char * buf, size_t cap) {
  if      (s < 60)    snprintf(buf, cap, "%us", (unsigned)s);
  else if (s < 3600)  snprintf(buf, cap, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(buf, cap, "%uh %02um",
                                (unsigned)(s / 3600), (unsigned)((s % 3600) / 60));
  else                snprintf(buf, cap, "%ud %02uh",
                                (unsigned)(s / 86400), (unsigned)((s % 86400) / 3600));
}

// From parent FAVICON_PATHS.
constexpr const char * kTrefoilSvg =
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 512 512'>"
  "<circle cx='256' cy='256' r='256' fill='#666'/>"
  "<circle cx='256' cy='256' r='220' fill='#FB2'/>"
  "<path fill='#555' d='M256 286a30 30 0 1 0 0-60 30 30 0 0 0 0 60zm28-82 62-109"
  "a182 182 0 0 0-182 1l63 109a57 57 0 0 1 57-1zm155 51H313c0 21-11 39-28 49l64"
  " 108c54-32 90-90 90-157zM163 412l64-108a57 57 0 0 1-28-49H73c0 67 36 125 90 "
  "157z'/></svg>";

void handle_favicon() {
  s_http.send(200, "image/svg+xml", kTrefoilSvg);
}

void handle_index() {
  size_t station_count = 0;
  size_t udp_count = 0;
  size_t fault_count = 0;
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    station_count = Stations::g_count;
    for (size_t i = 0; i < Stations::g_count; i++) {
      const auto & s = Stations::g_stations[i];
      if (Stations::is_udp_active(s)) udp_count++;
      if (Stations::is_faulted(s))    fault_count++;
    }
    xSemaphoreGive(Stations::g_mux);
  }
  const IPAddress ip = WiFi.localIP();

  // Chunked stream; page exceeds any sensible stack buffer.
  s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_http.send(200, "text/html; charset=utf-8", "");

  s_http.sendContent_P(PSTR(
    "<!doctype html><html lang=en><head>"
    "<meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>ESPGeiger Gadget</title>"
    "<link rel=icon type=image/svg+xml href=/favicon.svg>"
    "<link rel=stylesheet href=/style.css" EG_CACHE_BUST ">"
    "</head><body>"
    "<h1>"
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 512 512'>"
    "<circle cx='256' cy='256' r='256' fill='#666'/>"
    "<circle cx='256' cy='256' r='220' fill='#FB2'/>"
    "<path fill='#555' d='M256 286a30 30 0 1 0 0-60 30 30 0 0 0 0 60zm28-82 62-109"
    "a182 182 0 0 0-182 1l63 109a57 57 0 0 1 57-1zm155 51H313c0 21-11 39-28 49l64"
    " 108c54-32 90-90 90-157zM163 412l64-108a57 57 0 0 1-28-49H73c0 67 36 125 90 "
    "157z'/></svg>"
    "ESPGeiger Gadget</h1>"));

  // JS at the bottom updates these cells from /stats.json.
  char row[160];
  s_http.sendContent_P(PSTR("<table id=top>"));

  snprintf(row, sizeof(row),
    "<tr><th>IP</th><td class=mn>%u.%u.%u.%u</td></tr>",
    ip[0], ip[1], ip[2], ip[3]);
  s_http.sendContent(row);

  snprintf(row, sizeof(row),
    "<tr><th>Build</th><td class=mn>%s %s</td></tr>",
    __DATE__, __TIME__);
  s_http.sendContent(row);

  snprintf(row, sizeof(row),
    "<tr><th>Stations</th><td class=mn id=tcount>%u / %u (%u via UDP)</td></tr>",
    (unsigned)station_count, (unsigned)Stations::g_max, (unsigned)udp_count);
  s_http.sendContent(row);

  const char * health =
    (fault_count > 0) ? "<span class='tag err'>FAULT</span>" :
    (station_count == 0) ? "<span class='tag warn'>scanning</span>" :
                           "<span class='tag ok'>healthy</span>";
  snprintf(row, sizeof(row),
    "<tr><th>Health</th><td id=thealth>%s</td></tr>", health);
  s_http.sendContent(row);

  char up_buf[24];
  format_uptime((uint32_t)(millis() / 1000UL), up_buf, sizeof(up_buf));
  snprintf(row, sizeof(row),
    "<tr><th>Uptime</th><td class=mn id=tup>%s</td></tr>", up_buf);
  s_http.sendContent(row);
  s_http.sendContent_P(PSTR("<tr><th>&pi;</th><td class=mn id=tpi>-</td></tr>"));
  snprintf(row, sizeof(row),
    "<tr><th>Heap free</th><td class=mn id=theap>%lu B</td></tr>",
    (unsigned long)ESP.getFreeHeap());
  s_http.sendContent(row);

  s_http.sendContent_P(PSTR(
    "<tr><th>Heap min</th><td class=mn id=thmin>-</td></tr>"
    "<tr><th>Largest block</th><td class=mn id=thbig>-</td></tr>"
    "<tr><th>Polls / UDP</th><td class=mn id=trate>-</td></tr>"
    "</table>"
    "<p class=menu><a href=/shot>Screenshot (BMP)</a> "
    "<a href=/update>Update firmware</a></p>"
    "<p class=brand>"
    "<a href='https://github.com/steadramon/ESPGeiger-Gadget'>"
    "ESPGeiger Gadget</a> &middot; "));
  s_http.sendContent(kVersion);
  s_http.sendContent_P(PSTR("</p>"));

  s_http.sendContent_P(PSTR(
    "<script src=/app.js" EG_CACHE_BUST " defer></script>"
    "</body></html>"));
  s_http.sendContent("");
}

void handle_stats_json() {
  size_t station_count = 0, udp_count = 0, fault_count = 0;
  if (xSemaphoreTake(Stations::g_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    station_count = Stations::g_count;
    for (size_t i = 0; i < Stations::g_count; i++) {
      const auto & s = Stations::g_stations[i];
      if (Stations::is_udp_active(s)) udp_count++;
      if (Stations::is_faulted(s))    fault_count++;
    }
    xSemaphoreGive(Stations::g_mux);
  }

  multi_heap_info_t hi;
  heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  char body[320];
  snprintf(body, sizeof(body),
    "{\"uptime_s\":%lu,"
    "\"heap\":{\"free\":%u,\"min\":%u,\"big\":%u},"
    "\"polls\":%lu,\"udp\":%lu,"
    "\"stations\":%u,\"max\":%u,\"udp_count\":%u,\"faults\":%u,"
    "\"pi\":{\"d\":%llu,\"t\":%llu,\"i\":%llu}}",
    (unsigned long)(millis() / 1000UL),
    (unsigned)hi.total_free_bytes,
    (unsigned)hi.minimum_free_bytes,
    (unsigned)hi.largest_free_block,
    (unsigned long)Poller::g_poll_count,
    (unsigned long)UdpRx::g_pkt_count,
    (unsigned)station_count,
    (unsigned)Stations::g_max,
    (unsigned)udp_count,
    (unsigned)fault_count,
    (unsigned long long)MontePi::g_decays,
    (unsigned long long)MontePi::g_darts,
    (unsigned long long)MontePi::g_inside);
  s_http.send(200, "application/json", body);
}

// /shot: 16-bit BMP V3 with BI_BITFIELDS + R5G6B5 masks. Native LVGL
// format, no per-pixel conversion.

constexpr int32_t kW          = Board::SCREEN_W;
constexpr int32_t kH          = Board::SCREEN_H;
constexpr uint32_t kRowBytes  = (uint32_t)kW * 2;          // 480
constexpr uint32_t kPixelData = (uint32_t)kH * kRowBytes;  // 153 600
constexpr uint32_t kHeaderSz  = 14 + 40 + 12;              // 66
constexpr uint32_t kFileSize  = kHeaderSz + kPixelData;    // 153 666

void put_u16_le(uint8_t * p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put_u32_le(uint8_t * p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

WiFiClient * s_shot_client = nullptr;

void shot_flush_observer(int x1, int y1, int x2, int y2, const uint8_t * px_map) {
  if (!s_shot_client) return;
  const int w = x2 - x1 + 1;
  const int h = y2 - y1 + 1;
  // force_full_refresh always invalidates the whole screen, so tiles
  // span the full width and rows line up.
  if (w != kW) return;
  s_shot_client->write(px_map, (size_t)kRowBytes * h);
}

void handle_shot() {
  uint8_t hdr[kHeaderSz] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  put_u32_le(hdr + 2,  kFileSize);
  put_u32_le(hdr + 10, kHeaderSz);
  put_u32_le(hdr + 14, 40);
  put_u32_le(hdr + 18, (uint32_t)kW);
  put_u32_le(hdr + 22, (uint32_t)(-(int32_t)kH));   // negative -> top-down
  put_u16_le(hdr + 26, 1);
  put_u16_le(hdr + 28, 16);
  put_u32_le(hdr + 30, 3);                          // BI_BITFIELDS
  put_u32_le(hdr + 34, kPixelData);
  put_u32_le(hdr + 54, 0xF800);
  put_u32_le(hdr + 58, 0x07E0);
  put_u32_le(hdr + 62, 0x001F);

  WiFiClient client = s_http.client();
  s_http.setContentLength(kFileSize);
  s_http.send(200, "image/bmp", "");
  client.write(hdr, sizeof(hdr));

  s_shot_client = &client;
  Display::install_flush_observer(shot_flush_observer);
  Display::force_full_refresh();
  Display::clear_flush_observer();
  s_shot_client = nullptr;
}

// Test endpoint: fakes a 3-of-5 majority down.
void handle_test_radmon() {
  Poller::g_radmon_alarm_down    = 3;
  Poller::g_radmon_alarm_known   = 5;
  Poller::g_radmon_alarm_idx     = -1;
  Poller::g_radmon_alarm_pending = true;
  send_text(200, "text/plain", "alarm fired\n");
}

// /update: GET form + POST multipart upload via Arduino Update.
// Bad uploads auto-revert via the rollback marker in main.cpp.
void handle_update_form() {
  s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_http.send(200, "text/html; charset=utf-8", "");
  s_http.sendContent_P(PSTR(
    "<!doctype html><html lang=en><head>"
    "<meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Update - ESPGeiger Gadget</title>"
    "<link rel=stylesheet href=/style.css" EG_CACHE_BUST ">"
    "</head><body class=narrow>"
    "<h1>Firmware update</h1>"
    "<div class=warn><b>Be careful.</b> Only flash a .bin built for this "
    "gadget. The current firmware will reboot when done. If the new "
    "firmware crashes within 30 s of boot, the gadget reverts to the "
    "current one automatically.</div>"
    "<form id=f class=primary><input type=file name=firmware accept=.bin required>"
    "<button id=b>Flash</button></form>"
    "<progress id=p value=0 max=100 hidden></progress>"
    "<div id=st></div>"
    "<a href=/ class=back>&larr; back</a>"
    "<script>"
    "var f=document.getElementById('f'),b=document.getElementById('b'),"
    "p=document.getElementById('p'),st=document.getElementById('st');"
    "f.onsubmit=function(e){e.preventDefault();"
    "var fd=new FormData(f);"
    "if(!fd.get('firmware').size){st.textContent='pick a .bin first';return;}"
    "var x=new XMLHttpRequest();"
    "x.upload.onprogress=function(ev){"
    "if(ev.lengthComputable){p.value=Math.round(ev.loaded/ev.total*100);"
    "st.textContent='uploading '+Math.round(ev.loaded/1024)+"
    "'/'+Math.round(ev.total/1024)+' KB ('+p.value+'%)';}};"
    "x.onload=function(){"
    "if(x.status==200){p.value=100;"
    "st.textContent='success - rebooting, reloading in 20 s...';"
    "setTimeout(function(){location.href='/'},20000);}"
    "else{st.textContent='failed: '+x.responseText;b.disabled=false;}};"
    "x.onerror=function(){st.textContent='upload error (gadget may still be flashing - "
    "wait 20 s then reload)';};"
    "x.open('POST','/update');b.disabled=true;p.hidden=false;"
    "st.textContent='starting upload...';x.send(fd);"
    "};"
    "</script></body></html>"));
  s_http.sendContent("");
}

void handle_update_upload() {
  HTTPUpload & upload = s_http.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] upload start: %s\n", upload.filename.c_str());
    Ui::show_update_modal();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Update.printError(Serial);
      Ui::update_modal_set_status("begin failed");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[ota] success, %u bytes\n", (unsigned)upload.totalSize);
      Ui::update_modal_set_status("flash ok, rebooting");
    } else {
      Update.printError(Serial);
      Ui::update_modal_set_status("flash failed");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("[ota] upload aborted");
    Ui::update_modal_set_status("upload aborted");
  }
}

void handle_update_done() {
  if (Update.hasError()) {
    char buf[80];
    snprintf(buf, sizeof(buf), "update failed (error %u)",
             (unsigned)Update.getError());
    send_text(500, "text/plain", buf);
  } else {
    send_text(200, "text/plain", "ok, rebooting\n");
    delay(500);   // let the TCP flush before reboot
    ESP.restart();
  }
}

void handle_not_found() {
  send_text(404, "text/plain", "not found");
}

}  // anonymous namespace

void handle_app_js() {
#if EG_GZ_INDEX_JS
  send_gzip_p(200, "application/javascript", INDEX_JS_GZ, INDEX_JS_GZ_LEN);
#else
  s_http.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  s_http.send_P(200, "application/javascript", INDEX_JS);
#endif
}

void handle_style_css() {
#if EG_GZ_STYLE_CSS
  send_gzip_p(200, "text/css", STYLE_CSS_GZ, STYLE_CSS_GZ_LEN);
#else
  s_http.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  s_http.send_P(200, "text/css", STYLE_CSS);
#endif
}

void handle_crash() {
  const CrashDump::Info & ci = CrashDump::info();
  s_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_http.send(200, "text/html; charset=utf-8", "");
  s_http.sendContent_P(PSTR(
    "<!doctype html><html lang=en><head>"
    "<meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>Last crash - ESPGeiger Gadget</title>"
    "<link rel=stylesheet href=/style.css" EG_CACHE_BUST ">"
    "</head><body>"
    "<h1>Last crash</h1>"));

  // Send each row separately so they can't blow a single stack buffer.
  char row[128];
  snprintf(row, sizeof(row),
    "<table><tr><th>Reset reason</th><td class=\"mn %s\">%s</td></tr>",
    (ci.reset_reason == ESP_RST_POWERON || ci.reset_reason == ESP_RST_SW ||
     ci.reset_reason == ESP_RST_EXT) ? "ok" : "bad",
    CrashDump::reset_reason_str());
  s_http.sendContent(row);

  if (ci.has_coredump) {
    snprintf(row, sizeof(row),
      "<tr><th>Task</th><td class=mn>%s</td></tr>", ci.task);
    s_http.sendContent(row);
    snprintf(row, sizeof(row),
      "<tr><th>PC</th><td class=mn>0x%08lx</td></tr>",
      (unsigned long)ci.exc_pc);
    s_http.sendContent(row);
    snprintf(row, sizeof(row),
      "<tr><th>Exc cause</th><td class=mn>%lu</td></tr>",
      (unsigned long)ci.exc_cause);
    s_http.sendContent(row);
    snprintf(row, sizeof(row),
      "<tr><th>Fault addr</th><td class=mn>0x%08lx</td></tr></table>",
      (unsigned long)ci.exc_vaddr);
    s_http.sendContent(row);

    if (ci.bt_depth > 0) {
      s_http.sendContent_P(PSTR("<p>Backtrace:</p><pre>"));
      for (uint8_t i = 0; i < ci.bt_depth; i++) {
        snprintf(row, sizeof(row), "#%-2u 0x%08lx\n",
                 (unsigned)i, (unsigned long)ci.bt[i]);
        s_http.sendContent(row);
      }
      if (ci.bt_corrupted) {
        s_http.sendContent_P(PSTR("(corrupted)\n"));
      }
      s_http.sendContent_P(PSTR("</pre>"));
    }
    s_http.sendContent_P(PSTR(
      "<form method=POST action=/crash/clear>"
      "<button class=danger>Erase coredump</button></form>"));
  } else {
    s_http.sendContent_P(PSTR(
      "</table><p style='color:var(--muted)'>"
      "No coredump on flash. Either the last shutdown was clean, or "
      "this firmware has never panicked.</p>"));
  }

  s_http.sendContent_P(PSTR("<a href=/ class=back>&larr; back</a></body></html>"));
  s_http.sendContent("");
}

void handle_crash_clear() {
  const esp_err_t rc = CrashDump::erase();
  if (rc != ESP_OK) {
    Serial.printf("[crash] erase failed: %d\n", (int)rc);
  }
  s_http.sendHeader("Location", "/crash");
  s_http.send(303, "text/plain", "");
}

void init() {
  if (s_inited) return;
  s_http.on("/",            handle_index);
  s_http.on("/favicon.svg", handle_favicon);
  s_http.on("/shot",        handle_shot);
  s_http.on("/stats.json",  handle_stats_json);
  s_http.on("/app.js",      handle_app_js);
  s_http.on("/style.css",   handle_style_css);
  s_http.on("/test/radmon", handle_test_radmon);
  s_http.on("/update",      HTTP_GET,  handle_update_form);
  s_http.on("/update",      HTTP_POST, handle_update_done, handle_update_upload);
  s_http.on("/crash",       HTTP_GET,  handle_crash);
  s_http.on("/crash/clear", HTTP_POST, handle_crash_clear);
  s_http.onNotFound(handle_not_found);
  s_http.begin();
  s_inited = true;
  Serial.printf("[web] listening on http://%s/\n",
                WiFi.localIP().toString().c_str());
}

void loop() {
  if (!s_inited) return;
  s_http.handleClient();
}

}  // namespace WebSrv
