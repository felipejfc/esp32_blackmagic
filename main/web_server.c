/*
 * Web UI for ESP32 Black Magic Probe
 * Provides HTTP server with WebSocket for viewing UART/RTT terminal and status
 * This is a read-only informational interface - all debug control is via GDB
 */

#include "general.h"
#include "platform.h"
#include "web_server.h"
#include "uart_passthrough.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "web_server";

static httpd_handle_t server = NULL;
static int ws_fd = -1;  // WebSocket file descriptor
static SemaphoreHandle_t ws_mutex = NULL;

// Forward declarations
extern unsigned short gdb_port;
extern bool gdb_if_is_connected(void);

// ============== Embedded Web UI ==============
static const char index_html[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Black Magic Probe</title>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0d1117;color:#c9d1d9;min-height:100vh}"
".container{max-width:1200px;margin:0 auto;padding:16px}"
"header{background:linear-gradient(135deg,#161b22 0%,#21262d 100%);border-bottom:1px solid #30363d;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}"
".logo{display:flex;align-items:center;gap:10px}"
".logo svg{width:28px;height:28px;fill:#58a6ff}"
".logo h1{font-size:1.1rem;font-weight:600;color:#f0f6fc}"
".status{display:flex;align-items:center;gap:8px;font-size:0.8rem}"
".status-dot{width:8px;height:8px;border-radius:50%;background:#3fb950}"
".status-dot.offline{background:#f85149}"
".card{background:#161b22;border:1px solid #30363d;border-radius:6px;overflow:hidden;margin-bottom:16px}"
".card-header{background:#21262d;padding:10px 14px;border-bottom:1px solid #30363d;display:flex;align-items:center;justify-content:space-between}"
".card-header h2{font-size:0.75rem;font-weight:600;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px}"
".card-body{padding:14px}"
".btn{background:#21262d;color:#c9d1d9;border:1px solid #30363d;padding:4px 10px;border-radius:6px;font-size:0.7rem;cursor:pointer;transition:all 0.15s ease;display:inline-flex;align-items:center;gap:6px;font-weight:500}"
".btn:hover{background:#30363d;border-color:#8b949e}"
"#terminal{background:#0d1117;font-family:'SF Mono',Monaco,Consolas,monospace;font-size:0.75rem;line-height:1.5;height:350px;overflow-y:auto;padding:10px;color:#7ee787;white-space:pre-wrap;word-break:break-all}"
"#terminal .info{color:#8b949e}"
"#terminal .rtt{color:#a5d6ff}"
"#terminal .swo{color:#d2a8ff}"
".card.fullscreen{position:fixed;top:0;left:0;right:0;bottom:0;z-index:1000;margin:0;border-radius:0;display:flex;flex-direction:column}"
".card.fullscreen .card-header{flex-shrink:0}"
".card.fullscreen #terminal{flex:1;height:auto;max-height:none}"
".target-status{display:flex;align-items:center;gap:10px;padding:10px 14px;background:#0d1117;border-radius:4px}"
".target-status .indicator{width:10px;height:10px;border-radius:50%;background:#f85149}"
".target-status .indicator.connected{background:#3fb950}"
".target-status .indicator.gdb{background:#58a6ff}"
".target-status .text{font-size:0.8rem}"
".target-name{font-weight:600;color:#f0f6fc}"
".target-state{color:#8b949e;font-size:0.75rem}"
".info-grid{display:grid;gap:8px;grid-template-columns:repeat(auto-fit,minmax(150px,1fr))}"
".info-item{display:flex;justify-content:space-between;padding:6px 10px;background:#0d1117;border-radius:4px;font-size:0.75rem}"
".info-label{color:#8b949e}"
".info-value{color:#f0f6fc;font-weight:500;font-family:'SF Mono',Monaco,Consolas,monospace}"
"</style>"
"</head>"
"<body>"
"<header>"
"<div class=\"logo\">"
"<svg viewBox=\"0 0 24 24\"><path d=\"M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-2 15l-5-5 1.41-1.41L10 14.17l7.59-7.59L19 8l-9 9z\"/></svg>"
"<h1>Black Magic Probe</h1>"
"</div>"
"<div class=\"status\"><span class=\"status-dot\" id=\"ws-status\"></span><span id=\"ws-status-text\">Connecting...</span></div>"
"</header>"
"<div class=\"container\">"
"<div class=\"card\">"
"<div class=\"card-header\"><h2>Connection Status</h2></div>"
"<div class=\"card-body\">"
"<div class=\"target-status\">"
"<div class=\"indicator\" id=\"target-indicator\"></div>"
"<div class=\"text\"><div class=\"target-name\" id=\"target-name\">Waiting for GDB...</div><div class=\"target-state\" id=\"target-state\">Connect with GDB to control target</div></div>"
"</div>"
"</div>"
"</div>"
"<div class=\"card\" id=\"terminal-card\">"
"<div class=\"card-header\"><h2>UART/RTT Terminal</h2>"
"<div style=\"display:flex;gap:8px;align-items:center\">"
"<button class=\"btn\" onclick=\"clearTerminal()\">Clear</button>"
"<button class=\"btn\" id=\"expand-btn\" onclick=\"toggleFullscreen()\">Expand</button>"
"</div></div>"
"<div id=\"terminal\"><span class=\"info\">UART/RTT Terminal Ready - Output will appear here</span>\n</div>"
"</div>"
"<div class=\"card\">"
"<div class=\"card-header\"><h2>System Info</h2></div>"
"<div class=\"card-body\">"
"<div class=\"info-grid\">"
"<div class=\"info-item\"><span class=\"info-label\">GDB Port</span><span class=\"info-value\" id=\"gdb-port\">2345</span></div>"
"<div class=\"info-item\"><span class=\"info-label\">IP Address</span><span class=\"info-value\" id=\"ip-addr\">-</span></div>"
"<div class=\"info-item\"><span class=\"info-label\">Free Heap</span><span class=\"info-value\" id=\"free-heap\">-</span></div>"
"<div class=\"info-item\"><span class=\"info-label\">GDB Client</span><span class=\"info-value\" id=\"gdb-status\">Disconnected</span></div>"
"</div></div></div>"
"</div>"
"<script>"
"let ws;"
"const term=document.getElementById('terminal');"
"function log(msg,cls=''){const span=document.createElement('span');if(cls)span.className=cls;span.textContent=msg+'\\n';term.appendChild(span);term.scrollTop=term.scrollHeight;}"
"function connectWS(){"
"ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=()=>{document.getElementById('ws-status').classList.remove('offline');document.getElementById('ws-status-text').textContent='Connected';ws.send('{\"cmd\":\"status\"}');};"
"ws.onclose=()=>{document.getElementById('ws-status').classList.add('offline');document.getElementById('ws-status-text').textContent='Disconnected';setTimeout(connectWS,2000);};"
"ws.onmessage=(e)=>{if(e.data.startsWith('{')){handleJSON(JSON.parse(e.data));}else{term.appendChild(document.createTextNode(e.data));term.scrollTop=term.scrollHeight;}};"
"ws.onerror=()=>{};"
"}"
"function handleJSON(d){"
"if(d.type==='status'){"
"document.getElementById('free-heap').textContent=d.heap+' bytes';"
"document.getElementById('ip-addr').textContent=d.ip;"
"document.getElementById('gdb-port').textContent=d.gdb_port;"
"const gdbConnected=d.gdb_connected;"
"document.getElementById('gdb-status').textContent=gdbConnected?'Connected':'Disconnected';"
"updateTargetStatus(gdbConnected);"
"}"
"if(d.type==='target'){updateTargetInfo(d);}"
"if(d.type==='rtt'){appendAnsi(d.data,'rtt');}"
"if(d.type==='swo'){appendAnsi(d.data,'swo');}"
"}"
"function updateTargetStatus(gdbConnected){"
"const ind=document.getElementById('target-indicator'),name=document.getElementById('target-name'),state=document.getElementById('target-state');"
"if(gdbConnected){ind.classList.add('gdb');ind.classList.remove('connected');name.textContent='GDB Connected';state.textContent='Target controlled by GDB client';}"
"else{ind.classList.remove('gdb','connected');name.textContent='Waiting for GDB...';state.textContent='Connect with GDB to control target';}"
"}"
"function updateTargetInfo(d){"
"const ind=document.getElementById('target-indicator'),name=document.getElementById('target-name'),state=document.getElementById('target-state');"
"if(d.attached){ind.classList.add('connected');name.textContent=d.name||'Target Connected';state.textContent=d.details||'Target attached via GDB';}"
"else if(d.found){name.textContent=d.name||'Target Found';state.textContent='Target detected';}"
"}"
"function clearTerminal(){term.innerHTML='<span class=\"info\">Terminal cleared</span>\\n';}"
"function toggleFullscreen(){const card=document.getElementById('terminal-card');const btn=document.getElementById('expand-btn');if(card.classList.contains('fullscreen')){card.classList.remove('fullscreen');btn.textContent='Expand';}else{card.classList.add('fullscreen');btn.textContent='Collapse';}}"
"const ansiColors={0:'inherit',1:'#fff',30:'#545454',31:'#f85149',32:'#3fb950',33:'#d29922',34:'#58a6ff',35:'#d2a8ff',36:'#39c5cf',37:'#c9d1d9',90:'#6e7681',91:'#ff7b72',92:'#7ee787',93:'#e3b341',94:'#79c0ff',95:'#d2a8ff',96:'#56d4dd',97:'#f0f6fc'};"
"function appendAnsi(text,cls){const re=/\\x1b\\[(\\d+)m|\\u001b\\[(\\d+)m|\\[([0-9;]+)m/g;let color=null;let last=0;let m;while((m=re.exec(text))!==null){if(m.index>last){const span=document.createElement('span');if(cls)span.className=cls;if(color)span.style.color=color;span.textContent=text.slice(last,m.index);term.appendChild(span);}const code=parseInt(m[1]||m[2]||m[3]);if(code===0)color=null;else if(ansiColors[code])color=ansiColors[code];last=m.index+m[0].length;}if(last<text.length){const span=document.createElement('span');if(cls)span.className=cls;if(color)span.style.color=color;span.textContent=text.slice(last);term.appendChild(span);}term.scrollTop=term.scrollHeight;}"
"connectWS();"
"setInterval(()=>{if(ws&&ws.readyState===1)ws.send('{\"cmd\":\"status\"}');},3000);"
"</script>"
"</body>"
"</html>";

// ============== HTTP Handlers ==============

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, strlen(index_html));
}

// ============== WebSocket Handler ==============

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Handshake - store socket fd so we can send data
        xSemaphoreTake(ws_mutex, portMAX_DELAY);
        ws_fd = httpd_req_to_sockfd(req);
        xSemaphoreGive(ws_mutex);
        ESP_LOGI(TAG, "WebSocket handshake, fd=%d", ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Get frame length first
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ws_pkt.len > 0) {
        uint8_t *buf = malloc(ws_pkt.len + 1);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }

        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
        buf[ws_pkt.len] = '\0';

        // Store socket fd for async sending
        xSemaphoreTake(ws_mutex, portMAX_DELAY);
        ws_fd = httpd_req_to_sockfd(req);
        xSemaphoreGive(ws_mutex);

        // Handle incoming data - only status requests
        if (buf[0] == '{') {
            if (strstr((char *)buf, "\"status\"")) {
                // Send status response
                char status[256];
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                esp_netif_get_ip_info(netif, &ip_info);

                snprintf(status, sizeof(status),
                    "{\"type\":\"status\",\"heap\":%lu,\"ip\":\"" IPSTR "\",\"gdb_port\":%d,\"gdb_connected\":%s}",
                    esp_get_free_heap_size(), IP2STR(&ip_info.ip), gdb_port,
                    gdb_if_is_connected() ? "true" : "false");

                httpd_ws_frame_t resp = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)status,
                    .len = strlen(status)
                };
                httpd_ws_send_frame(req, &resp);
            }
        }

        free(buf);
    }

    return ESP_OK;
}

void web_server_send_uart_data(const uint8_t *data, size_t len)
{
    if (ws_fd < 0 || !server || len == 0) return;

    // Strip ANSI escape codes (ESC [ ... letter)
    uint8_t *clean = malloc(len);
    if (!clean) return;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0x1b && i + 1 < len && data[i + 1] == '[') {
            // Skip ESC [
            i += 2;
            // Skip parameters until we hit the command letter (@ to ~)
            while (i < len && (data[i] < 0x40 || data[i] > 0x7e)) {
                i++;
            }
            // Skip the command letter itself (loop will increment i)
        } else {
            clean[j++] = data[i];
        }
    }

    if (j == 0) {
        free(clean);
        return;
    }

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_fd >= 0) {
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = clean,
            .len = j
        };
        httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
    }
    xSemaphoreGive(ws_mutex);
    free(clean);
}

void web_server_notify_target_status(const char *status)
{
    if (ws_fd < 0 || !server) return;

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_fd >= 0) {
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)status,
            .len = strlen(status)
        };
        httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
    }
    xSemaphoreGive(ws_mutex);
}

/*
 * Send SWO trace data to WebSocket client
 * Data is wrapped in JSON: {"type":"swo","data":"..."}
 */
void web_server_send_swo_data(const uint8_t *data, size_t len)
{
    if (ws_fd < 0 || !server || len == 0) return;

    /* Allocate buffer for JSON wrapper + unicode escaped data */
    size_t max_json_len = 32 + len * 6;
    char *json_buf = malloc(max_json_len);
    if (!json_buf) return;

    /* Build JSON message with escaped data */
    int pos = snprintf(json_buf, max_json_len, "{\"type\":\"swo\",\"data\":\"");

    /* Escape special characters for JSON */
    for (size_t i = 0; i < len && pos < (int)(max_json_len - 10); i++) {
        uint8_t c = data[i];
        if (c == '"') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\\"");
        } else if (c == '\\') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\\\");
        } else if (c == '\n') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\n");
        } else if (c == '\r') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\r");
        } else if (c == '\t') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\t");
        } else if (c >= 32 && c < 127) {
            json_buf[pos++] = c;
        } else {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\u00%02x", c);
        }
    }

    pos += snprintf(json_buf + pos, max_json_len - pos, "\"}");

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_fd >= 0) {
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json_buf,
            .len = pos
        };
        httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
    }
    xSemaphoreGive(ws_mutex);
    free(json_buf);
}

/*
 * Send RTT data to WebSocket client
 * Data is wrapped in JSON: {"type":"rtt","data":"..."}
 */
void web_server_send_rtt_data(const uint8_t *data, size_t len)
{
    if (ws_fd < 0 || !server || len == 0) return;

    /* Allocate buffer for JSON wrapper + unicode escaped data */
    size_t max_json_len = 32 + len * 6;
    char *json_buf = malloc(max_json_len);
    if (!json_buf) return;

    /* Build JSON message with escaped data */
    int pos = snprintf(json_buf, max_json_len, "{\"type\":\"rtt\",\"data\":\"");

    /* Escape special characters for JSON */
    for (size_t i = 0; i < len && pos < (int)(max_json_len - 10); i++) {
        uint8_t c = data[i];
        if (c == '"') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\\"");
        } else if (c == '\\') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\\\");
        } else if (c == '\n') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\n");
        } else if (c == '\r') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\r");
        } else if (c == '\t') {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\t");
        } else if (c >= 32 && c < 127) {
            json_buf[pos++] = c;
        } else {
            pos += snprintf(json_buf + pos, max_json_len - pos, "\\u00%02x", c);
        }
    }

    pos += snprintf(json_buf + pos, max_json_len - pos, "\"}");

    xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_fd >= 0) {
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json_buf,
            .len = pos
        };
        httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
    }
    xSemaphoreGive(ws_mutex);
    free(json_buf);
}

void web_server_init(void)
{
    ws_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 4;
    config.stack_size = 4096;

    ESP_LOGI(TAG, "Starting web server on port %d", WEB_SERVER_PORT);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    // Register handlers - only index and websocket
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t ws_uri = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    httpd_register_uri_handler(server, &ws_uri);

    ESP_LOGI(TAG, "Web server started - informational view at http://IP:%d", WEB_SERVER_PORT);
}
