/*
 * Web UI for ESP32 Black Magic Probe
 * Provides HTTP server with WebSocket for UART terminal and GDB control panel
 */

#include "general.h"
#include "platform.h"
#include "web_server.h"
#include "uart_passthrough.h"
#include "target.h"
#include "target/target_internal.h"
#include "exception.h"
#include "gdb_main.h"
#include "command.h"

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
#include <ctype.h>

static const char *TAG = "web_server";

static httpd_handle_t server = NULL;
static int ws_fd = -1;  // WebSocket file descriptor
static SemaphoreHandle_t ws_mutex = NULL;
static SemaphoreHandle_t target_mutex = NULL;  // Mutex for thread-safe target access

// Forward declarations
extern unsigned short gdb_port;
extern target_s *cur_target;
extern int command_process(target_s *t, char *cmd);

// Breakpoint tracking
#define MAX_BREAKPOINTS 16
typedef struct {
    target_addr_t addr;
    target_breakwatch_e type;
    bool active;
} breakpoint_t;
static breakpoint_t breakpoints[MAX_BREAKPOINTS];
static int breakpoint_count = 0;


// ============== Thread-Safe Target Access ==============

static bool target_lock(TickType_t timeout)
{
    if (!target_mutex) return false;
    return xSemaphoreTake(target_mutex, timeout) == pdTRUE;
}

static void target_unlock(void)
{
    if (target_mutex) {
        xSemaphoreGive(target_mutex);
    }
}

// Safe target access wrapper that returns current attached target
static target_s *get_current_target(void)
{
    return cur_target;
}

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
".container{max-width:1400px;margin:0 auto;padding:16px}"
"header{background:linear-gradient(135deg,#161b22 0%,#21262d 100%);border-bottom:1px solid #30363d;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}"
".logo{display:flex;align-items:center;gap:10px}"
".logo svg{width:28px;height:28px;fill:#58a6ff}"
".logo h1{font-size:1.1rem;font-weight:600;color:#f0f6fc}"
".status{display:flex;align-items:center;gap:8px;font-size:0.8rem}"
".status-dot{width:8px;height:8px;border-radius:50%;background:#3fb950}"
".status-dot.offline{background:#f85149}"
".status-dot.halted{background:#f0883e}"
".main-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px}"
"@media(max-width:1100px){.main-grid{grid-template-columns:1fr}}"
".card{background:#161b22;border:1px solid #30363d;border-radius:6px;overflow:hidden}"
".card-header{background:#21262d;padding:10px 14px;border-bottom:1px solid #30363d;display:flex;align-items:center;justify-content:space-between}"
".card-header h2{font-size:0.75rem;font-weight:600;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px}"
".card-body{padding:14px}"
".exec-controls{display:flex;gap:8px;flex-wrap:wrap;padding:14px}"
".btn{background:#21262d;color:#c9d1d9;border:1px solid #30363d;padding:8px 14px;border-radius:6px;font-size:0.8rem;cursor:pointer;transition:all 0.15s ease;display:inline-flex;align-items:center;gap:6px;font-weight:500}"
".btn:hover{background:#30363d;border-color:#8b949e}"
".btn:disabled{opacity:0.5;cursor:not-allowed}"
".btn-primary{background:#238636;border-color:#238636;color:#fff}"
".btn-primary:hover{background:#2ea043}"
".btn-danger{background:#da3633;border-color:#da3633;color:#fff}"
".btn-danger:hover{background:#f85149}"
".btn-warning{background:#9e6a03;border-color:#9e6a03;color:#fff}"
".btn-warning:hover{background:#bb8009}"
".btn svg{width:14px;height:14px;fill:currentColor}"
".reg-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;font-family:'SF Mono',Monaco,Consolas,monospace;font-size:0.7rem}"
".reg-item{background:#0d1117;padding:6px 8px;border-radius:4px;display:flex;justify-content:space-between}"
".reg-name{color:#8b949e}"
".reg-value{color:#7ee787;cursor:pointer}"
".reg-value:hover{color:#58a6ff}"
".mem-viewer{font-family:'SF Mono',Monaco,Consolas,monospace}"
".mem-input{display:flex;gap:8px;margin-bottom:10px}"
".mem-input input{flex:1;background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:6px 10px;border-radius:4px;font-size:0.8rem;font-family:'SF Mono',Monaco,Consolas,monospace}"
".mem-input input:focus{outline:none;border-color:#58a6ff}"
".mem-dump{background:#0d1117;padding:10px;border-radius:4px;font-size:0.7rem;line-height:1.6;max-height:200px;overflow-y:auto;color:#7ee787;white-space:pre}"
".bp-list{max-height:150px;overflow-y:auto}"
".bp-item{display:flex;align-items:center;justify-content:space-between;padding:6px 10px;background:#0d1117;border-radius:4px;margin-bottom:4px;font-family:'SF Mono',Monaco,Consolas,monospace;font-size:0.75rem}"
".bp-addr{color:#f0883e}"
".bp-remove{background:none;border:none;color:#f85149;cursor:pointer;padding:2px 6px;font-size:0.8rem}"
".bp-remove:hover{color:#ff7b72}"
".bp-add{display:flex;gap:6px;margin-top:8px}"
".bp-add input{flex:1;background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:6px 8px;border-radius:4px;font-size:0.75rem;font-family:'SF Mono',Monaco,Consolas,monospace}"
"#terminal{background:#0d1117;font-family:'SF Mono',Monaco,Consolas,monospace;font-size:0.75rem;line-height:1.5;height:200px;overflow-y:auto;padding:10px;color:#7ee787;white-space:pre-wrap;word-break:break-all}"
"#terminal .input{color:#58a6ff}"
"#terminal .error{color:#f85149}"
"#terminal .info{color:#8b949e}"
".input-row{display:flex;border-top:1px solid #30363d}"
".input-row input{flex:1;background:#0d1117;border:none;color:#c9d1d9;padding:10px;font-family:'SF Mono',Monaco,Consolas,monospace;font-size:0.75rem;outline:none}"
".input-row input::placeholder{color:#484f58}"
".input-row select{background:#21262d;border:none;border-left:1px solid #30363d;color:#c9d1d9;padding:8px;font-size:0.75rem}"
".gdb-console{background:#0d1117;font-family:'SF Mono',Monaco,Consolas,monospace;font-size:0.75rem;line-height:1.5;height:150px;overflow-y:auto;padding:10px;color:#c9d1d9;white-space:pre-wrap}"
".gdb-console .cmd{color:#58a6ff}"
".gdb-console .out{color:#7ee787}"
".target-status{display:flex;align-items:center;gap:10px;padding:10px 14px;background:#0d1117;border-radius:4px;margin-bottom:10px}"
".target-status .indicator{width:10px;height:10px;border-radius:50%;background:#f85149}"
".target-status .indicator.attached{background:#3fb950}"
".target-status .indicator.halted{background:#f0883e}"
".target-status .text{font-size:0.8rem}"
".target-name{font-weight:600;color:#f0f6fc}"
".target-state{color:#8b949e;font-size:0.75rem}"
".flash-upload{margin-top:10px}"
".flash-upload input[type=file]{display:none}"
".flash-upload label{display:inline-flex;align-items:center;gap:6px;padding:8px 14px;background:#21262d;border:1px solid #30363d;border-radius:6px;cursor:pointer;font-size:0.8rem}"
".flash-upload label:hover{background:#30363d}"
".progress-bar{height:6px;background:#21262d;border-radius:3px;margin-top:8px;overflow:hidden;display:none}"
".progress-bar .fill{height:100%;background:#238636;width:0%;transition:width 0.3s}"
".info-grid{display:grid;gap:8px}"
".info-item{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #21262d;font-size:0.75rem}"
".info-item:last-child{border-bottom:none}"
".info-label{color:#8b949e}"
".info-value{color:#f0f6fc;font-weight:500}"
".full-width{grid-column:1/-1}"
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
"<div class=\"card\" style=\"margin-bottom:16px\">"
"<div class=\"card-header\"><h2>Target Status</h2><div class=\"btn-group\"><button class=\"btn\" onclick=\"scanTarget()\">Scan</button><button class=\"btn\" onclick=\"attachTarget()\">Attach</button></div></div>"
"<div class=\"card-body\">"
"<div class=\"target-status\">"
"<div class=\"indicator\" id=\"target-indicator\"></div>"
"<div class=\"text\"><div class=\"target-name\" id=\"target-name\">No Target</div><div class=\"target-state\" id=\"target-state\">Click Scan to detect target</div></div>"
"</div>"
"</div>"
"</div>"
"<div class=\"card\" style=\"margin-bottom:16px\">"
"<div class=\"card-header\"><h2>Execution Control</h2></div>"
"<div class=\"exec-controls\">"
"<button class=\"btn btn-primary\" id=\"btn-run\" onclick=\"resumeTarget()\"><svg viewBox=\"0 0 24 24\"><path d=\"M8 5v14l11-7z\"/></svg>Run</button>"
"<button class=\"btn btn-warning\" id=\"btn-halt\" onclick=\"haltTarget()\"><svg viewBox=\"0 0 24 24\"><path d=\"M6 19h4V5H6v14zm8-14v14h4V5h-4z\"/></svg>Halt</button>"
"<button class=\"btn\" id=\"btn-step\" onclick=\"stepTarget()\"><svg viewBox=\"0 0 24 24\"><path d=\"M6 18l8.5-6L6 6v12zM16 6v12h2V6h-2z\"/></svg>Step</button>"
"<button class=\"btn\" onclick=\"resetTarget()\"><svg viewBox=\"0 0 24 24\"><path d=\"M17.65 6.35A7.958 7.958 0 0012 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08A5.99 5.99 0 0112 18c-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z\"/></svg>Reset</button>"
"<div class=\"flash-upload\">"
"<input type=\"file\" id=\"flash-file\" accept=\".bin,.hex,.elf\" onchange=\"uploadFlash()\">"
"<label for=\"flash-file\"><svg viewBox=\"0 0 24 24\" style=\"width:14px;height:14px;fill:currentColor\"><path d=\"M9 16h6v-6h4l-7-7-7 7h4v6zm-4 2h14v2H5v-2z\"/></svg>Flash</label>"
"</div>"
"<div class=\"progress-bar\" id=\"flash-progress\"><div class=\"fill\" id=\"flash-progress-fill\"></div></div>"
"</div>"
"</div>"
"<div class=\"main-grid\">"
"<div class=\"card\">"
"<div class=\"card-header\"><h2>Registers</h2><button class=\"btn\" onclick=\"refreshRegs()\" style=\"padding:4px 10px;font-size:0.7rem\">Refresh</button></div>"
"<div class=\"card-body\"><div class=\"reg-grid\" id=\"reg-grid\"><div class=\"reg-item\"><span class=\"reg-name\">--</span><span class=\"reg-value\">--</span></div></div></div>"
"</div>"
"<div class=\"card\">"
"<div class=\"card-header\"><h2>Memory Viewer</h2></div>"
"<div class=\"card-body mem-viewer\">"
"<div class=\"mem-input\">"
"<input type=\"text\" id=\"mem-addr\" placeholder=\"0x20000000\" value=\"0x20000000\">"
"<input type=\"text\" id=\"mem-len\" placeholder=\"Length\" value=\"64\" style=\"width:80px\">"
"<button class=\"btn\" onclick=\"readMemory()\" style=\"padding:6px 12px\">Read</button>"
"</div>"
"<div class=\"mem-dump\" id=\"mem-dump\">Enter address and click Read</div>"
"</div>"
"</div>"
"<div class=\"card\">"
"<div class=\"card-header\"><h2>Breakpoints</h2></div>"
"<div class=\"card-body\">"
"<div class=\"bp-list\" id=\"bp-list\"><div style=\"color:#8b949e;font-size:0.75rem\">No breakpoints set</div></div>"
"<div class=\"bp-add\">"
"<input type=\"text\" id=\"bp-addr\" placeholder=\"0x08001000\">"
"<select id=\"bp-type\"><option value=\"1\">Hardware</option><option value=\"0\">Software</option></select>"
"<button class=\"btn btn-primary\" onclick=\"addBreakpoint()\" style=\"padding:6px 12px\">Add</button>"
"</div>"
"</div>"
"</div>"
"<div class=\"card\">"
"<div class=\"card-header\"><h2>GDB Console</h2></div>"
"<div class=\"gdb-console\" id=\"gdb-console\"><span style=\"color:#8b949e\">Enter monitor commands below...</span>\n</div>"
"<div class=\"input-row\">"
"<input type=\"text\" id=\"gdb-input\" placeholder=\"mon swdp_scan\" onkeypress=\"handleGdbInput(event)\">"
"</div>"
"</div>"
"<div class=\"card full-width\">"
"<div class=\"card-header\"><h2>UART Terminal</h2>"
"<div style=\"display:flex;gap:8px;align-items:center\">"
"<select id=\"baud-select\" onchange=\"setBaud()\" style=\"background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:4px 8px;border-radius:4px;font-size:0.75rem\">"
"<option value=\"9600\">9600</option><option value=\"19200\">19200</option><option value=\"38400\">38400</option>"
"<option value=\"57600\">57600</option><option value=\"115200\" selected>115200</option>"
"<option value=\"230400\">230400</option><option value=\"460800\">460800</option><option value=\"921600\">921600</option>"
"</select>"
"<button class=\"btn\" onclick=\"clearTerminal()\" style=\"padding:4px 10px;font-size:0.7rem\">Clear</button>"
"</div></div>"
"<div id=\"terminal\"><span class=\"info\">UART Terminal Ready</span>\n</div>"
"<div class=\"input-row\">"
"<input type=\"text\" id=\"uart-input\" placeholder=\"Type and press Enter...\" onkeypress=\"handleUartInput(event)\">"
"</div>"
"</div>"
"<div class=\"card full-width\">"
"<div class=\"card-header\"><h2>System Info</h2></div>"
"<div class=\"card-body\">"
"<div class=\"info-grid\" style=\"grid-template-columns:repeat(4,1fr)\">"
"<div class=\"info-item\"><span class=\"info-label\">GDB Port</span><span class=\"info-value\" id=\"gdb-port\">2345</span></div>"
"<div class=\"info-item\"><span class=\"info-label\">UART Port</span><span class=\"info-value\">2346</span></div>"
"<div class=\"info-item\"><span class=\"info-label\">IP Address</span><span class=\"info-value\" id=\"ip-addr\">-</span></div>"
"<div class=\"info-item\"><span class=\"info-label\">Free Heap</span><span class=\"info-value\" id=\"free-heap\">-</span></div>"
"</div></div></div>"
"</div></div>"
"<script>"
"let ws,targetAttached=false,targetHalted=false,pollInterval=null;"
"const term=document.getElementById('terminal'),gdbCon=document.getElementById('gdb-console');"
"function log(msg,cls=''){const span=document.createElement('span');if(cls)span.className=cls;span.textContent=msg+'\\n';term.appendChild(span);term.scrollTop=term.scrollHeight;}"
"function gdbLog(msg,cls=''){const span=document.createElement('span');if(cls)span.className=cls;span.textContent=msg+'\\n';gdbCon.appendChild(span);gdbCon.scrollTop=gdbCon.scrollHeight;}"
"function connectWS(){"
"ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=()=>{document.getElementById('ws-status').classList.remove('offline');document.getElementById('ws-status-text').textContent='Connected';};"
"ws.onclose=()=>{document.getElementById('ws-status').classList.add('offline');document.getElementById('ws-status-text').textContent='Disconnected';setTimeout(connectWS,2000);};"
"ws.onmessage=(e)=>{if(e.data.startsWith('{')){handleJSON(JSON.parse(e.data));}else{term.appendChild(document.createTextNode(e.data));term.scrollTop=term.scrollHeight;}};"
"ws.onerror=()=>{};"
"}"
"function handleJSON(d){"
"if(d.type==='status'){document.getElementById('free-heap').textContent=d.heap+' bytes';document.getElementById('ip-addr').textContent=d.ip;document.getElementById('gdb-port').textContent=d.gdb_port;}"
"if(d.type==='target'){updateTargetUI(d);}"
"if(d.type==='halt_status'){targetHalted=d.halted;updateExecButtons();if(d.halted&&targetAttached)refreshRegs();}"
"}"
"function updateTargetUI(d){"
"const ind=document.getElementById('target-indicator'),name=document.getElementById('target-name'),state=document.getElementById('target-state');"
"if(d.attached){targetAttached=true;ind.classList.add('attached');name.textContent=d.name||'Target Attached';state.textContent=d.details||'Ready';}else if(d.found){ind.classList.remove('attached','halted');name.textContent=d.name||'Target Found';state.textContent='Click Attach to connect';}else{targetAttached=false;ind.classList.remove('attached','halted');name.textContent='No Target';state.textContent=d.error||'Click Scan to detect';}"
"updateExecButtons();"
"}"
"function updateExecButtons(){"
"const canExec=targetAttached;document.getElementById('btn-run').disabled=!canExec||!targetHalted;document.getElementById('btn-halt').disabled=!canExec||targetHalted;document.getElementById('btn-step').disabled=!canExec||!targetHalted;"
"const ind=document.getElementById('target-indicator');if(targetAttached&&targetHalted){ind.classList.add('halted');ind.classList.remove('attached');}else if(targetAttached){ind.classList.add('attached');ind.classList.remove('halted');}"
"}"
"function handleUartInput(e){if(e.key==='Enter'&&ws&&ws.readyState===1){const inp=document.getElementById('uart-input');log('> '+inp.value,'input');ws.send(inp.value+'\\n');inp.value='';}}"
"function handleGdbInput(e){if(e.key==='Enter'){const inp=document.getElementById('gdb-input');runMonitor(inp.value);inp.value='';}}"
"function clearTerminal(){term.innerHTML='<span class=\"info\">Terminal cleared.</span>\\n';}"
"async function api(endpoint,method='POST',body=null){try{const opts={method};if(body){opts.headers={'Content-Type':'application/json'};opts.body=JSON.stringify(body);}const r=await fetch('/api/'+endpoint,opts);return await r.json();}catch(e){return{error:e.message};}}"
"async function scanTarget(){gdbLog('> Scanning...','cmd');const r=await api('scan');if(r.ok){gdbLog('Found '+r.count+' target(s): '+r.targets,'out');}else{gdbLog('Scan failed: '+r.error,'error');}}"
"async function attachTarget(){gdbLog('> Attaching...','cmd');const r=await api('target/attach');if(r.ok){gdbLog('Attached to '+r.name,'out');targetAttached=true;targetHalted=r.halted;updateExecButtons();if(r.halted)refreshRegs();startPolling();}else{gdbLog('Attach failed: '+r.error,'error');}}"
"async function haltTarget(){const r=await api('target/halt');if(r.ok){targetHalted=true;updateExecButtons();refreshRegs();}}"
"async function resumeTarget(){const r=await api('target/resume');if(r.ok){targetHalted=false;updateExecButtons();}}"
"async function stepTarget(){const r=await api('target/step');if(r.ok){refreshRegs();}}"
"async function resetTarget(){await api('reset');targetAttached=false;targetHalted=false;updateExecButtons();setTimeout(scanTarget,500);}"
"async function refreshRegs(){if(!targetAttached)return;const r=await api('regs','GET');if(r.ok){renderRegs(r.regs);}}"
"function renderRegs(regs){const grid=document.getElementById('reg-grid');grid.innerHTML='';const names=['R0','R1','R2','R3','R4','R5','R6','R7','R8','R9','R10','R11','R12','SP','LR','PC','xPSR'];regs.forEach((v,i)=>{const div=document.createElement('div');div.className='reg-item';const nm=names[i]||'R'+i;div.innerHTML='<span class=\"reg-name\">'+nm+'</span><span class=\"reg-value\" onclick=\"editReg('+i+')\">0x'+v.toString(16).padStart(8,'0').toUpperCase()+'</span>';grid.appendChild(div);});}"
"async function editReg(idx){const val=prompt('Enter new value for register '+idx+' (hex):','0x');if(val){const r=await api('regs','POST',{reg:idx,value:parseInt(val,16)});if(r.ok)refreshRegs();}}"
"async function readMemory(){const addr=parseInt(document.getElementById('mem-addr').value,16);const len=parseInt(document.getElementById('mem-len').value,10)||64;if(isNaN(addr)){document.getElementById('mem-dump').textContent='Invalid address';return;}const r=await api('mem/read?addr='+addr+'&len='+len,'GET');if(r.ok){renderMemDump(addr,r.data);}else{document.getElementById('mem-dump').textContent='Error: '+r.error;}}"
"function renderMemDump(baseAddr,data){let out='';for(let i=0;i<data.length;i+=16){let line=(baseAddr+i).toString(16).padStart(8,'0')+': ';let ascii='';for(let j=0;j<16&&i+j<data.length;j++){line+=data[i+j].toString(16).padStart(2,'0')+' ';const c=data[i+j];ascii+=(c>=32&&c<127)?String.fromCharCode(c):'.';}out+=line.padEnd(58,' ')+ascii+'\\n';}document.getElementById('mem-dump').textContent=out;}"
"async function addBreakpoint(){const addr=parseInt(document.getElementById('bp-addr').value,16);const type=parseInt(document.getElementById('bp-type').value,10);if(isNaN(addr)){alert('Invalid address');return;}const r=await api('bp/set','POST',{addr:addr,type:type});if(r.ok){refreshBreakpoints();document.getElementById('bp-addr').value='';}else{alert('Failed: '+r.error);}}"
"async function removeBreakpoint(addr){const r=await api('bp/clear','POST',{addr:addr});if(r.ok)refreshBreakpoints();}"
"async function refreshBreakpoints(){const r=await api('bp/list','GET');if(r.ok){renderBreakpoints(r.breakpoints);}}"
"function renderBreakpoints(bps){const list=document.getElementById('bp-list');if(!bps||bps.length===0){list.innerHTML='<div style=\"color:#8b949e;font-size:0.75rem\">No breakpoints set</div>';return;}list.innerHTML='';bps.forEach(bp=>{const div=document.createElement('div');div.className='bp-item';div.innerHTML='<span class=\"bp-addr\">0x'+bp.addr.toString(16).padStart(8,'0')+'</span><span>'+(bp.type===1?'HW':'SW')+'</span><button class=\"bp-remove\" onclick=\"removeBreakpoint('+bp.addr+')\">×</button>';list.appendChild(div);});}"
"async function runMonitor(cmd){gdbLog('> '+cmd,'cmd');const r=await api('monitor','POST',{cmd:cmd});if(r.ok){gdbLog(r.output||'OK','out');}else{gdbLog('Error: '+r.error,'error');}}"
"async function setBaud(){const b=document.getElementById('baud-select').value;await api('uart/baud?baud='+b);log('Baud rate set to '+b,'info');}"
"async function uploadFlash(){const file=document.getElementById('flash-file').files[0];if(!file)return;const prog=document.getElementById('flash-progress'),fill=document.getElementById('flash-progress-fill');prog.style.display='block';fill.style.width='0%';gdbLog('> Flashing '+file.name+' ('+file.size+' bytes)...','cmd');try{const data=await file.arrayBuffer();const r=await fetch('/api/flash/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-Flash-Size':file.size},body:data});const json=await r.json();if(json.ok){fill.style.width='100%';gdbLog('Flash complete!','out');}else{gdbLog('Flash failed: '+json.error,'error');}}catch(e){gdbLog('Flash error: '+e.message,'error');}setTimeout(()=>{prog.style.display='none';},2000);}"
"function startPolling(){if(pollInterval)return;pollInterval=setInterval(async()=>{if(targetAttached&&!targetHalted){const r=await api('target/status','GET');if(r.ok&&r.halted!==targetHalted){targetHalted=r.halted;updateExecButtons();if(r.halted)refreshRegs();}}},500);}"
"connectWS();"
"setTimeout(()=>{api('scan');},1000);"
"setInterval(()=>{if(ws&&ws.readyState===1)ws.send('{\"cmd\":\"status\"}');},5000);"
"</script>"
"</body>"
"</html>";

// ============== HTTP Handlers ==============

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, strlen(index_html));
}

// Context for collecting scan results
typedef struct {
    char *buffer;
    size_t buf_size;
    size_t offset;
    int count;
} scan_context_t;

static void scan_callback(size_t index, target_s *target, void *context)
{
    scan_context_t *ctx = (scan_context_t *)context;
    const char *driver = target_driver_name(target);
    const char *core = target_core_name(target);

    if (ctx->offset < ctx->buf_size - 100) {
        int written = snprintf(ctx->buffer + ctx->offset, ctx->buf_size - ctx->offset,
            "%s%zu: %s%s%s",
            ctx->count > 0 ? ", " : "",
            index + 1,
            driver ? driver : "Unknown",
            core ? " " : "",
            core ? core : "");
        if (written > 0) {
            ctx->offset += written;
        }
    }
    ctx->count++;
}

static esp_err_t api_scan_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char response[512];
    char targets_buf[256] = "";

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    // Free any existing target list
    target_list_free();
    cur_target = NULL;  // Clear any previous attachment

    // Perform SWD scan
    bool scan_result = false;
    volatile exception_s e;
    TRY_CATCH (e, EXCEPTION_ALL) {
        scan_result = adiv5_swd_scan(0);
    }

    if (e.type == EXCEPTION_TIMEOUT) {
        snprintf(response, sizeof(response),
            "{\"ok\":false,\"error\":\"Timeout during scan. Is target connected?\"}");
    } else if (e.type == EXCEPTION_ERROR) {
        snprintf(response, sizeof(response),
            "{\"ok\":false,\"error\":\"Scan error: %s\"}", e.msg ? e.msg : "unknown");
    } else if (!scan_result) {
        snprintf(response, sizeof(response),
            "{\"ok\":false,\"error\":\"No targets found. Check wiring.\"}");
    } else {
        // Collect target info
        scan_context_t ctx = {
            .buffer = targets_buf,
            .buf_size = sizeof(targets_buf),
            .offset = 0,
            .count = 0
        };
        target_foreach(scan_callback, &ctx);

        snprintf(response, sizeof(response),
            "{\"ok\":true,\"count\":%d,\"targets\":\"%s\"}", ctx.count, targets_buf);
    }

    target_unlock();

    // Also notify via WebSocket
    if (ws_fd >= 0) {
        char ws_msg[512];
        if (scan_result) {
            scan_context_t ctx2 = {
                .buffer = targets_buf,
                .buf_size = sizeof(targets_buf),
                .offset = 0,
                .count = 0
            };
            target_foreach(scan_callback, &ctx2);
            snprintf(ws_msg, sizeof(ws_msg),
                "{\"type\":\"target\",\"found\":true,\"name\":\"%s\",\"details\":\"Found %d target(s)\"}",
                targets_buf, ctx2.count);
        } else {
            const char *err_msg = "No targets found";
            if (e.type == EXCEPTION_TIMEOUT) err_msg = "Timeout during scan";
            else if (e.type == EXCEPTION_ERROR) err_msg = e.msg ? e.msg : "Scan error";
            snprintf(ws_msg, sizeof(ws_msg),
                "{\"type\":\"target\",\"found\":false,\"error\":\"%s\"}", err_msg);
        }
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)ws_msg,
            .len = strlen(ws_msg)
        };
        httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
    }

    return httpd_resp_sendstr(req, response);
}

static esp_err_t api_attach_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    char response[256];

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    // Attach to first target and set cur_target global
    target_s *target = target_attach_n(1, NULL);
    if (target) {
        cur_target = target;  // Set the global so other handlers can use it
    }

    if (target) {
        const char *driver = target_driver_name(target);
        const char *core = target_core_name(target);

        // Check if halted
        target_addr_t watch;
        target_halt_reason_e reason = target_halt_poll(target, &watch);
        bool halted = (reason != TARGET_HALT_RUNNING);

        snprintf(response, sizeof(response),
            "{\"ok\":true,\"name\":\"%s%s%s\",\"halted\":%s}",
            driver ? driver : "Unknown",
            core ? " " : "",
            core ? core : "",
            halted ? "true" : "false");

        // Notify via WebSocket
        if (ws_fd >= 0) {
            char ws_msg[256];
            snprintf(ws_msg, sizeof(ws_msg),
                "{\"type\":\"target\",\"attached\":true,\"name\":\"%s%s%s\",\"details\":\"Connected\"}",
                driver ? driver : "Unknown",
                core ? " " : "",
                core ? core : "");
            httpd_ws_frame_t ws_pkt = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)ws_msg,
                .len = strlen(ws_msg)
            };
            httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
        }
    } else {
        snprintf(response, sizeof(response),
            "{\"ok\":false,\"error\":\"Failed to attach. Run scan first.\"}");
    }

    target_unlock();
    return httpd_resp_sendstr(req, response);
}

static esp_err_t api_halt_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    volatile exception_s e;
    TRY_CATCH (e, EXCEPTION_ALL) {
        target_halt_request(target);
    }

    target_unlock();

    if (e.type) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Halt failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    // Notify via WebSocket
    if (ws_fd >= 0) {
        const char *ws_msg = "{\"type\":\"halt_status\",\"halted\":true}";
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)ws_msg,
            .len = strlen(ws_msg)
        };
        httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_resume_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    volatile exception_s e;
    TRY_CATCH (e, EXCEPTION_ALL) {
        target_halt_resume(target, false);
    }

    target_unlock();

    if (e.type) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Resume failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    // Notify via WebSocket
    if (ws_fd >= 0) {
        const char *ws_msg = "{\"type\":\"halt_status\",\"halted\":false}";
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)ws_msg,
            .len = strlen(ws_msg)
        };
        httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_step_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    volatile exception_s e;
    TRY_CATCH (e, EXCEPTION_ALL) {
        target_halt_resume(target, true);  // true = single step
    }

    target_unlock();

    if (e.type) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Step failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_target_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!target_lock(pdMS_TO_TICKS(100))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":true,\"attached\":false,\"halted\":false}");
    }

    target_addr_t watch;
    target_halt_reason_e reason = target_halt_poll(target, &watch);
    bool halted = (reason != TARGET_HALT_RUNNING);

    target_unlock();

    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":true,\"attached\":true,\"halted\":%s}", halted ? "true" : "false");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t api_regs_read_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    size_t reg_size = target_regs_size(target);
    if (reg_size == 0 || reg_size > 256) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid register size\"}");
    }

    uint8_t *regs = malloc(reg_size);
    if (!regs) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\"}");
    }

    volatile exception_s e;
    TRY_CATCH (e, EXCEPTION_ALL) {
        target_regs_read(target, regs);
    }

    target_unlock();

    if (e.type) {
        free(regs);
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Read failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    // Build JSON response with register values
    char *response = malloc(2048);
    if (!response) {
        free(regs);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\"}");
    }

    int offset = snprintf(response, 2048, "{\"ok\":true,\"regs\":[");
    size_t num_regs = reg_size / 4;  // Assume 32-bit registers
    uint32_t *reg32 = (uint32_t *)regs;

    for (size_t i = 0; i < num_regs && offset < 1900; i++) {
        offset += snprintf(response + offset, 2048 - offset, "%s%lu", i > 0 ? "," : "", (unsigned long)reg32[i]);
    }
    snprintf(response + offset, 2048 - offset, "]}");

    httpd_resp_sendstr(req, response);
    free(regs);
    free(response);
    return ESP_OK;
}

static esp_err_t api_regs_write_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (content_len <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No data\"}");
    }
    content[content_len] = '\0';

    // Parse JSON: {"reg": N, "value": V}
    int reg_num = -1;
    uint32_t value = 0;

    char *reg_str = strstr(content, "\"reg\":");
    char *val_str = strstr(content, "\"value\":");
    if (reg_str) reg_num = atoi(reg_str + 6);
    if (val_str) value = strtoul(val_str + 8, NULL, 0);

    if (reg_num < 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid register\"}");
    }

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    volatile exception_s e;
    TRY_CATCH (e, EXCEPTION_ALL) {
        target_reg_write(target, reg_num, &value, sizeof(value));
    }

    target_unlock();

    if (e.type) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Write failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_mem_read_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // Parse query string
    char query[64];
    target_addr_t addr = 0;
    size_t len = 64;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(query, "addr", param, sizeof(param)) == ESP_OK) {
            addr = strtoul(param, NULL, 0);
        }
        if (httpd_query_key_value(query, "len", param, sizeof(param)) == ESP_OK) {
            len = atoi(param);
        }
    }

    if (len > 256) len = 256;  // Limit read size

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    uint8_t *data = malloc(len);
    if (!data) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\"}");
    }

    volatile exception_s e;
    int result = 0;
    TRY_CATCH (e, EXCEPTION_ALL) {
        result = target_mem_read(target, data, addr, len);
    }

    target_unlock();

    if (e.type || result != 0) {
        free(data);
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Read failed: %s\"}", e.msg ? e.msg : "memory error");
        return httpd_resp_sendstr(req, response);
    }

    // Build JSON response
    char *response = malloc(len * 4 + 64);
    if (!response) {
        free(data);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\"}");
    }

    int offset = snprintf(response, len * 4 + 64, "{\"ok\":true,\"data\":[");
    for (size_t i = 0; i < len; i++) {
        offset += snprintf(response + offset, len * 4 + 64 - offset, "%s%d", i > 0 ? "," : "", data[i]);
    }
    snprintf(response + offset, len * 4 + 64 - offset, "]}");

    httpd_resp_sendstr(req, response);
    free(data);
    free(response);
    return ESP_OK;
}

static esp_err_t api_mem_write_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char content[1024];
    int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (content_len <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No data\"}");
    }
    content[content_len] = '\0';

    // Parse JSON: {"addr": A, "data": [b1, b2, ...]}
    target_addr_t addr = 0;
    char *addr_str = strstr(content, "\"addr\":");
    if (addr_str) addr = strtoul(addr_str + 7, NULL, 0);

    char *data_str = strstr(content, "\"data\":[");
    if (!data_str) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Missing data\"}");
    }

    // Parse data array
    uint8_t data[256];
    size_t data_len = 0;
    char *p = data_str + 8;
    while (*p && *p != ']' && data_len < sizeof(data)) {
        while (*p && !isdigit((unsigned char)*p) && *p != ']') p++;
        if (*p == ']') break;
        data[data_len++] = atoi(p);
        while (*p && isdigit((unsigned char)*p)) p++;
    }

    if (data_len == 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Empty data\"}");
    }

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    volatile exception_s e;
    int result = 0;
    TRY_CATCH (e, EXCEPTION_ALL) {
        result = target_mem_write(target, addr, data, data_len);
    }

    target_unlock();

    if (e.type || result != 0) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Write failed: %s\"}", e.msg ? e.msg : "memory error");
        return httpd_resp_sendstr(req, response);
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_bp_list_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char response[1024];
    int offset = snprintf(response, sizeof(response), "{\"ok\":true,\"breakpoints\":[");

    for (int i = 0; i < breakpoint_count && offset < 900; i++) {
        if (breakpoints[i].active) {
            offset += snprintf(response + offset, sizeof(response) - offset,
                "%s{\"addr\":%lu,\"type\":%d}",
                i > 0 ? "," : "",
                (unsigned long)breakpoints[i].addr,
                breakpoints[i].type);
        }
    }
    snprintf(response + offset, sizeof(response) - offset, "]}");

    return httpd_resp_sendstr(req, response);
}

static esp_err_t api_bp_set_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (content_len <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No data\"}");
    }
    content[content_len] = '\0';

    // Parse JSON: {"addr": A, "type": T}
    target_addr_t addr = 0;
    target_breakwatch_e type = TARGET_BREAK_HARD;

    char *addr_str = strstr(content, "\"addr\":");
    char *type_str = strstr(content, "\"type\":");
    if (addr_str) addr = strtoul(addr_str + 7, NULL, 0);
    if (type_str) type = atoi(type_str + 7);

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    volatile exception_s e;
    int result = -1;
    TRY_CATCH (e, EXCEPTION_ALL) {
        result = target_breakwatch_set(target, type, addr, 4);
    }

    target_unlock();

    if (e.type || result < 0) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Set failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    // Track breakpoint
    if (breakpoint_count < MAX_BREAKPOINTS) {
        breakpoints[breakpoint_count].addr = addr;
        breakpoints[breakpoint_count].type = type;
        breakpoints[breakpoint_count].active = true;
        breakpoint_count++;
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_bp_clear_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char content[128];
    int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (content_len <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No data\"}");
    }
    content[content_len] = '\0';

    // Parse JSON: {"addr": A}
    target_addr_t addr = 0;
    char *addr_str = strstr(content, "\"addr\":");
    if (addr_str) addr = strtoul(addr_str + 7, NULL, 0);

    // Find breakpoint type
    target_breakwatch_e type = TARGET_BREAK_HARD;
    for (int i = 0; i < breakpoint_count; i++) {
        if (breakpoints[i].addr == addr && breakpoints[i].active) {
            type = breakpoints[i].type;
            break;
        }
    }

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    volatile exception_s e;
    int result = -1;
    TRY_CATCH (e, EXCEPTION_ALL) {
        result = target_breakwatch_clear(target, type, addr, 4);
    }

    target_unlock();

    if (e.type || result < 0) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Clear failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    // Remove from tracking
    for (int i = 0; i < breakpoint_count; i++) {
        if (breakpoints[i].addr == addr && breakpoints[i].active) {
            breakpoints[i].active = false;
            // Compact array
            for (int j = i; j < breakpoint_count - 1; j++) {
                breakpoints[j] = breakpoints[j + 1];
            }
            breakpoint_count--;
            break;
        }
    }

    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ELF32 structures for parsing
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

#define PT_LOAD 1
#define ELF_MAX_SEGMENTS 16

// Helper to receive exact number of bytes
static int recv_exact(httpd_req_t *req, uint8_t *buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, (char *)buf + received, len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        received += ret;
    }
    return received;
}

// Helper to skip bytes in stream
static int skip_bytes(httpd_req_t *req, size_t len, uint8_t *buf, size_t buf_size) {
    size_t skipped = 0;
    while (skipped < len) {
        size_t to_read = (len - skipped) > buf_size ? buf_size : (len - skipped);
        int ret = httpd_req_recv(req, (char *)buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        skipped += ret;
    }
    return skipped;
}

static esp_err_t api_flash_upload_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // Get file size from header
    char size_str[16] = {0};
    httpd_req_get_hdr_value_str(req, "X-Flash-Size", size_str, sizeof(size_str));
    size_t file_size = atoi(size_str);

    if (file_size == 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid file size\"}");
    }

    ESP_LOGI(TAG, "Flash upload: %d bytes", file_size);

    // Allocate buffer for operations
    const size_t chunk_size = 4096;
    uint8_t *chunk = malloc(chunk_size);
    if (!chunk) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\"}");
    }

    // Read ELF header (or first 52 bytes)
    Elf32_Ehdr ehdr;
    if (recv_exact(req, (uint8_t *)&ehdr, sizeof(ehdr)) < 0) {
        free(chunk);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Failed to read header\"}");
    }

    bool is_elf = (ehdr.e_ident[0] == 0x7F && ehdr.e_ident[1] == 'E' &&
                   ehdr.e_ident[2] == 'L' && ehdr.e_ident[3] == 'F');

    if (!target_lock(pdMS_TO_TICKS(5000))) {
        free(chunk);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    target_s *target = get_current_target();
    if (!target) {
        target_unlock();
        free(chunk);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No target attached\"}");
    }

    // Get flash base from target
    target_addr_t flash_base = 0;
    if (target->flash) {
        flash_base = target->flash->start;
    }
    if (flash_base == 0) {
        // Fallback defaults
        flash_base = 0x08000000;  // STM32 default
    }

    ESP_LOGI(TAG, "Flash base: 0x%08lx, is_elf: %d", (unsigned long)flash_base, is_elf);

    volatile exception_s e;
    bool success = true;
    size_t bytes_read = sizeof(ehdr);  // Track position in file

    if (is_elf) {
        // Validate ELF
        if (ehdr.e_ident[4] != 1) {  // Must be 32-bit
            free(chunk);
            target_unlock();
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Only 32-bit ELF supported\"}");
        }
        if (ehdr.e_phnum == 0 || ehdr.e_phnum > ELF_MAX_SEGMENTS) {
            free(chunk);
            target_unlock();
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid ELF program headers\"}");
        }

        // Skip to program headers if needed
        if (ehdr.e_phoff > bytes_read) {
            if (skip_bytes(req, ehdr.e_phoff - bytes_read, chunk, chunk_size) < 0) {
                free(chunk);
                target_unlock();
                return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Failed to read ELF\"}");
            }
            bytes_read = ehdr.e_phoff;
        }

        // Read program headers
        Elf32_Phdr *phdrs = malloc(ehdr.e_phnum * sizeof(Elf32_Phdr));
        if (!phdrs) {
            free(chunk);
            target_unlock();
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Out of memory\"}");
        }

        if (recv_exact(req, (uint8_t *)phdrs, ehdr.e_phnum * sizeof(Elf32_Phdr)) < 0) {
            free(phdrs);
            free(chunk);
            target_unlock();
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Failed to read program headers\"}");
        }
        bytes_read += ehdr.e_phnum * sizeof(Elf32_Phdr);

        // Find PT_LOAD segments and calculate total size to erase
        volatile uint32_t min_addr = 0xFFFFFFFF, max_addr = 0;
        for (int i = 0; i < ehdr.e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_filesz > 0) {
                if (phdrs[i].p_paddr < min_addr) min_addr = phdrs[i].p_paddr;
                if (phdrs[i].p_paddr + phdrs[i].p_filesz > max_addr)
                    max_addr = phdrs[i].p_paddr + phdrs[i].p_filesz;
            }
        }

        if (min_addr >= max_addr) {
            free(phdrs);
            free(chunk);
            target_unlock();
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No loadable segments in ELF\"}");
        }

        ESP_LOGI(TAG, "ELF: erasing 0x%08lx - 0x%08lx", (unsigned long)min_addr, (unsigned long)max_addr);

        // Erase flash range
        TRY_CATCH (e, EXCEPTION_ALL) {
            success = target_flash_erase(target, min_addr, max_addr - min_addr);
        }
        if (e.type || !success) {
            free(phdrs);
            free(chunk);
            target_unlock();
            char response[128];
            snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Erase failed: %s\"}", e.msg ? e.msg : "unknown");
            return httpd_resp_sendstr(req, response);
        }

        // Process each PT_LOAD segment - need to stream through file
        // Sort segments by file offset for streaming
        for (int i = 0; i < ehdr.e_phnum - 1; i++) {
            for (int j = i + 1; j < ehdr.e_phnum; j++) {
                if (phdrs[j].p_offset < phdrs[i].p_offset) {
                    Elf32_Phdr tmp = phdrs[i];
                    phdrs[i] = phdrs[j];
                    phdrs[j] = tmp;
                }
            }
        }

        // Stream through file, flashing PT_LOAD segments
        volatile size_t total_written = 0;
        for (volatile int i = 0; i < ehdr.e_phnum; i++) {
            if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_filesz == 0) continue;

            // Skip to segment offset
            if (phdrs[i].p_offset > bytes_read) {
                if (skip_bytes(req, phdrs[i].p_offset - bytes_read, chunk, chunk_size) < 0) {
                    free(phdrs);
                    free(chunk);
                    target_flash_complete(target);
                    target_unlock();
                    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Failed reading ELF segment\"}");
                }
                bytes_read = phdrs[i].p_offset;
            }

            // Flash this segment
            ESP_LOGI(TAG, "Flashing segment: 0x%08lx, %lu bytes",
                     (unsigned long)phdrs[i].p_paddr, (unsigned long)phdrs[i].p_filesz);

            volatile target_addr_t seg_addr = phdrs[i].p_paddr;
            volatile size_t seg_remaining = phdrs[i].p_filesz;

            while (seg_remaining > 0) {
                volatile size_t to_read = seg_remaining > chunk_size ? chunk_size : seg_remaining;
                if (recv_exact(req, chunk, to_read) < 0) {
                    free(phdrs);
                    free(chunk);
                    target_flash_complete(target);
                    target_unlock();
                    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Failed reading segment data\"}");
                }
                bytes_read += to_read;

                TRY_CATCH (e, EXCEPTION_ALL) {
                    success = target_flash_write(target, seg_addr, chunk, to_read);
                }
                if (e.type || !success) {
                    free(phdrs);
                    free(chunk);
                    target_flash_complete(target);
                    target_unlock();
                    char response[128];
                    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Write failed at 0x%08lx\"}", (unsigned long)seg_addr);
                    return httpd_resp_sendstr(req, response);
                }

                seg_addr += to_read;
                seg_remaining -= to_read;
                total_written += to_read;
            }
        }

        free(phdrs);

        // Drain any remaining data
        while (bytes_read < file_size) {
            size_t to_read = (file_size - bytes_read) > chunk_size ? chunk_size : (file_size - bytes_read);
            int ret = httpd_req_recv(req, (char *)chunk, to_read);
            if (ret <= 0) break;
            bytes_read += ret;
        }

        // Complete flash
        TRY_CATCH (e, EXCEPTION_ALL) {
            success = target_flash_complete(target);
        }

        free(chunk);
        target_unlock();

        if (e.type || !success) {
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Flash complete failed\"}");
        }

        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":true,\"written\":%lu}", (unsigned long)total_written);
        ESP_LOGI(TAG, "ELF flash complete: %lu bytes written", (unsigned long)total_written);
        return httpd_resp_sendstr(req, response);

    } else {
        // Raw binary file - use target's flash base
        size_t bin_size = file_size;

        ESP_LOGI(TAG, "Binary: flashing %d bytes to 0x%08lx", bin_size, (unsigned long)flash_base);

        // Erase flash
        TRY_CATCH (e, EXCEPTION_ALL) {
            success = target_flash_erase(target, flash_base, bin_size);
        }
        if (e.type || !success) {
            free(chunk);
            target_unlock();
            char response[128];
            snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Erase failed: %s\"}", e.msg ? e.msg : "unknown");
            return httpd_resp_sendstr(req, response);
        }

        // Write the header bytes we already read
        volatile target_addr_t write_addr = flash_base;
        TRY_CATCH (e, EXCEPTION_ALL) {
            success = target_flash_write(target, write_addr, (uint8_t *)&ehdr, sizeof(ehdr));
        }
        if (e.type || !success) {
            free(chunk);
            target_flash_complete(target);
            target_unlock();
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Write failed at start\"}");
        }
        write_addr += sizeof(ehdr);

        // Write rest of file
        while (bytes_read < file_size) {
            int ret = httpd_req_recv(req, (char *)chunk, chunk_size);
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
                break;
            }

            TRY_CATCH (e, EXCEPTION_ALL) {
                success = target_flash_write(target, write_addr, chunk, ret);
            }
            if (e.type || !success) {
                free(chunk);
                target_flash_complete(target);
                target_unlock();
                char response[128];
                snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Write failed at 0x%08lx\"}", (unsigned long)write_addr);
                return httpd_resp_sendstr(req, response);
            }

            bytes_read += ret;
            write_addr += ret;
            ESP_LOGI(TAG, "Flash progress: %d/%d bytes", bytes_read, file_size);
        }

        // Complete flash
        TRY_CATCH (e, EXCEPTION_ALL) {
            success = target_flash_complete(target);
        }

        free(chunk);
        target_unlock();

        if (e.type || !success) {
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Flash complete failed\"}");
        }

        ESP_LOGI(TAG, "Binary flash complete: %d bytes", bytes_read);
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
}

static esp_err_t api_monitor_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char content[256];
    int content_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (content_len <= 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No data\"}");
    }
    content[content_len] = '\0';

    // Parse JSON: {"cmd": "command"}
    char cmd[128] = {0};
    char *cmd_str = strstr(content, "\"cmd\":\"");
    if (cmd_str) {
        cmd_str += 7;
        char *end = strchr(cmd_str, '"');
        if (end && (size_t)(end - cmd_str) < sizeof(cmd)) {
            memcpy(cmd, cmd_str, end - cmd_str);
        }
    }

    if (strlen(cmd) == 0) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Empty command\"}");
    }

    if (!target_lock(pdMS_TO_TICKS(2000))) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    // Execute command using command_process
    target_s *target = get_current_target();
    volatile exception_s e;

    TRY_CATCH (e, EXCEPTION_ALL) {
        (void)command_process(target, cmd);
    }

    target_unlock();

    if (e.type) {
        char response[128];
        snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"Command failed: %s\"}", e.msg ? e.msg : "unknown");
        return httpd_resp_sendstr(req, response);
    }

    return httpd_resp_sendstr(req, "{\"ok\":true,\"output\":\"Command executed\"}");
}

static esp_err_t api_reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Resetting target");

    if (!target_lock(pdMS_TO_TICKS(1000))) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Target busy\"}");
    }

    httpd_resp_set_type(req, "application/json");

    // If we have an attached target, use the debug interface to reset
    if (cur_target) {
        ESP_LOGI(TAG, "Resetting via SWD debug interface");
        target_reset(cur_target);
        target_unlock();
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    // No target attached - try hardware reset via NRST pin
    ESP_LOGI(TAG, "No target attached, pulsing NRST pin");
    gpio_set_direction(NRST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(NRST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));  // Hold reset for 50ms
    gpio_set_level(NRST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));  // Wait for target to start
    gpio_set_direction(NRST_PIN, GPIO_MODE_INPUT);  // Release to high-Z

    target_unlock();
    return httpd_resp_sendstr(req, "{\"ok\":true,\"note\":\"NRST pulse only, no target attached\"}");
}

static esp_err_t api_baud_handler(httpd_req_t *req)
{
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "baud", param, sizeof(param)) == ESP_OK) {
            uint32_t baud = atoi(param);
            if (baud > 0) {
#ifdef PLATFORM_HAS_UART_PASSTHROUGH
                uart_passthrough_set_baud(baud);
#endif
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_sendstr(req, "{\"ok\":true}");
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid baud rate\"}");
}

// ============== WebSocket Handler ==============

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Handshake - store socket fd so we can send UART data
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

        // Handle incoming data
        if (buf[0] == '{') {
            // JSON command
            if (strstr((char *)buf, "\"status\"")) {
                // Send status response
                char status[256];
                esp_netif_ip_info_t ip_info;
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                esp_netif_get_ip_info(netif, &ip_info);

                snprintf(status, sizeof(status),
                    "{\"type\":\"status\",\"heap\":%lu,\"ip\":\"" IPSTR "\",\"gdb_port\":%d}",
                    esp_get_free_heap_size(), IP2STR(&ip_info.ip), gdb_port);

                httpd_ws_frame_t resp = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)status,
                    .len = strlen(status)
                };
                httpd_ws_send_frame(req, &resp);
            }
        } else {
            // Forward to UART
#ifdef PLATFORM_HAS_UART_PASSTHROUGH
            uart_write_bytes(TARGET_UART_PORT, buf, ws_pkt.len);
#endif
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

void web_server_init(void)
{
    ws_mutex = xSemaphoreCreateMutex();
    target_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 24;  // Increased for all new endpoints
    config.stack_size = 8192;  // Increased stack for flash operations

    ESP_LOGI(TAG, "Starting web server on port %d", WEB_SERVER_PORT);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return;
    }

    // Register handlers
    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t ws_uri = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    httpd_register_uri_handler(server, &ws_uri);

    httpd_uri_t scan_uri = { .uri = "/api/scan", .method = HTTP_POST, .handler = api_scan_handler };
    httpd_register_uri_handler(server, &scan_uri);

    httpd_uri_t reset_uri = { .uri = "/api/reset", .method = HTTP_POST, .handler = api_reset_handler };
    httpd_register_uri_handler(server, &reset_uri);

    httpd_uri_t baud_uri = { .uri = "/api/uart/baud", .method = HTTP_POST, .handler = api_baud_handler };
    httpd_register_uri_handler(server, &baud_uri);

    // New GDB control panel endpoints
    httpd_uri_t attach_uri = { .uri = "/api/target/attach", .method = HTTP_POST, .handler = api_attach_handler };
    httpd_register_uri_handler(server, &attach_uri);

    httpd_uri_t halt_uri = { .uri = "/api/target/halt", .method = HTTP_POST, .handler = api_halt_handler };
    httpd_register_uri_handler(server, &halt_uri);

    httpd_uri_t resume_uri = { .uri = "/api/target/resume", .method = HTTP_POST, .handler = api_resume_handler };
    httpd_register_uri_handler(server, &resume_uri);

    httpd_uri_t step_uri = { .uri = "/api/target/step", .method = HTTP_POST, .handler = api_step_handler };
    httpd_register_uri_handler(server, &step_uri);

    httpd_uri_t status_uri = { .uri = "/api/target/status", .method = HTTP_GET, .handler = api_target_status_handler };
    httpd_register_uri_handler(server, &status_uri);

    httpd_uri_t regs_read_uri = { .uri = "/api/regs", .method = HTTP_GET, .handler = api_regs_read_handler };
    httpd_register_uri_handler(server, &regs_read_uri);

    httpd_uri_t regs_write_uri = { .uri = "/api/regs", .method = HTTP_POST, .handler = api_regs_write_handler };
    httpd_register_uri_handler(server, &regs_write_uri);

    httpd_uri_t mem_read_uri = { .uri = "/api/mem/read", .method = HTTP_GET, .handler = api_mem_read_handler };
    httpd_register_uri_handler(server, &mem_read_uri);

    httpd_uri_t mem_write_uri = { .uri = "/api/mem/write", .method = HTTP_POST, .handler = api_mem_write_handler };
    httpd_register_uri_handler(server, &mem_write_uri);

    httpd_uri_t bp_list_uri = { .uri = "/api/bp/list", .method = HTTP_GET, .handler = api_bp_list_handler };
    httpd_register_uri_handler(server, &bp_list_uri);

    httpd_uri_t bp_set_uri = { .uri = "/api/bp/set", .method = HTTP_POST, .handler = api_bp_set_handler };
    httpd_register_uri_handler(server, &bp_set_uri);

    httpd_uri_t bp_clear_uri = { .uri = "/api/bp/clear", .method = HTTP_POST, .handler = api_bp_clear_handler };
    httpd_register_uri_handler(server, &bp_clear_uri);

    httpd_uri_t flash_uri = { .uri = "/api/flash/upload", .method = HTTP_POST, .handler = api_flash_upload_handler };
    httpd_register_uri_handler(server, &flash_uri);

    httpd_uri_t monitor_uri = { .uri = "/api/monitor", .method = HTTP_POST, .handler = api_monitor_handler };
    httpd_register_uri_handler(server, &monitor_uri);

    ESP_LOGI(TAG, "Web server started with GDB control panel at http://IP:%d", WEB_SERVER_PORT);
}
