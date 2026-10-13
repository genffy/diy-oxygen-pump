// 充氧泵控制器 - ESP8266 AP 热点(手机本地控制) + STA 连家庭路由器(NTP 校时) + 每日定时
// 板子:Arduino IDE 选 "Wemos D1 mini" (LOLIN(WEMOS) D1 mini)
// 接线:PUMP_PIN(D1) 接泵蓝线(PWM);泵红线接5V;泵黑线接GND;绿线黄线悬空
// 泵型号:Kamoer HDVP1-B6 (6V/3W, 内置无刷驱动, 支持 PWM 调速)

#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <sys/time.h>
#include <time.h>

#define PUMP_PIN D1 // 接泵蓝线(PWM 调速)

const char *AP_SSID = "OxygenPump"; // 手机上搜到的 WiFi 名
const char *AP_PASS = "12345678";   // 至少 8 位,可改

// 家庭路由器账号,只用于连 NTP 校时。留空则不连路由器(纯热点,每日计划不可用)。
// 凭据不写在代码里:scripts/flash.sh 读取 .env.local 生成
// oxygen_pump.ino.globals.h(core 编译时自动强制 include)完成注入,
// 密码不会进入 git,也不会通过网页 API 或串口日志输出。
#ifndef STA_SSID
#define STA_SSID "" // 未注入时默认纯热点模式
#endif
#ifndef STA_PASS
#define STA_PASS ""
#endif

// 时区集中定义:中国标准时间 UTC+8,不启用夏令时。时间换算只从这里取偏移。
const int TZ_OFFSET_SEC = 8 * 3600;
// 至少两个 NTP 服务器提高可用性
const char *NTP_SERVER1 = "ntp1.aliyun.com";
const char *NTP_SERVER2 = "pool.ntp.org";
const char *NTP_SERVER3 = "time.google.com";
// 判定时间有效的最小 Unix 时间(2020-09),防止未同步时把垃圾值当真实时间
const time_t TIME_EPOCH_MIN = 1600000000;
// STA 断线重连间隔
const unsigned long STA_RETRY_MS = 30000;

ESP8266WebServer server(80);

// 设置结构 v2:magic 换新值,旧固件(0xA5A5A5A5)数据自动迁移
// 前 5 个字段与 v1 布局完全一致,迁移时可直接沿用读出的值
struct Settings {
  uint32_t magic;    // 校验位,判断 EEPROM 是否已初始化
  uint32_t onMin;    // 工作分钟(纯间歇循环与每日窗口内循环共用)
  uint32_t offMin;   // 休息分钟
  uint8_t mode;      // 0=自动定时 1=手动开 2=手动关
  uint8_t speed;     // 速度百分比 10~100
  uint8_t daily;     // 每日计划 0=关(纯间歇循环) 1=开(按每日窗口)
  uint8_t cycle;     // 运行窗口内 0=连续运行 1=间歇循环
  uint16_t startMin; // 每日开始运行时刻,当天分钟数 0~1439
  uint16_t stopMin;  // 每日停止运行时刻,当天分钟数
} s;

#define MAGIC_V1 0xA5A5A5A5
#define MAGIC_V2 0x5A5A5A5A

bool pumpOn = false;
unsigned long since = 0;      // 当前工作/休息阶段的计时起点(millis)
bool wasSynced = false;       // 上一轮循环时间是否有效(检测"首次校时")
bool wasInWindow = false;     // 上一轮循环是否在运行窗口内(检测"进入窗口")
bool ntpSynced = false;       // SNTP 回调置位:收到过服务器时间
unsigned long lastStaTry = 0; // 上次尝试连接路由器的时刻

// STA 连接诊断:串口只输出 SSID/IP/原因码,不输出密码
WiFiEventHandler onGotIp, onStaDisc;

void load() {
  EEPROM.begin(128);
  EEPROM.get(0, s);
  if (s.magic == MAGIC_V1) {
    // 旧版固件数据:沿用速度/模式/间歇时长,每日计划默认关闭,升级后行为不变
    s.daily = 0;
    s.cycle = 0;
    s.startMin = 17 * 60;
    s.stopMin = 8 * 60;
  } else if (s.magic != MAGIC_V2) {
    // 首次上电写默认值(每日计划默认关闭,需手动开启)
    s.onMin = 2;
    s.offMin = 10;
    s.mode = 0;
    s.speed = 50; // 默认 50% 速度
    s.daily = 0;
    s.cycle = 0;
    s.startMin = 17 * 60;
    s.stopMin = 8 * 60;
  }
  // 无论来源,写入前统一钳制,防损坏数据产生无效时间或异常状态
  s.onMin = constrain(s.onMin, 1UL, 1440UL);
  s.offMin = constrain(s.offMin, 1UL, 1440UL);
  if (s.mode > 2)
    s.mode = 0;
  if (s.speed < 10 || s.speed > 100)
    s.speed = 50;
  s.daily = s.daily ? 1 : 0;
  s.cycle = s.cycle ? 1 : 0;
  s.startMin %= 1440;
  s.stopMin %= 1440;
  if (s.startMin == s.stopMin) { // 相同视为无效,恢复默认窗口
    s.startMin = 17 * 60;
    s.stopMin = 8 * 60;
  }
  s.magic = MAGIC_V2;
  EEPROM.put(0, s);
  EEPROM.commit();
}
void save() {
  s.magic = MAGIC_V2;
  EEPROM.put(0, s);
  EEPROM.commit();
}

// PWM 值: 速度百分比 -> 0~1023
// HDVP1-B6: PWM 高=转, 低=停
// 最低 10% 保证泵能启动
int speedToPWM(uint8_t pct) {
  return map(constrain(pct, 10, 100), 0, 100, 0, 1023);
}

// 泵输出的唯一入口:Web handler、计划逻辑都通过它驱动,避免多处写 PWM 状态不一致
void applySpeed() { analogWrite(PUMP_PIN, speedToPWM(s.speed)); }
void pump(bool on) {
  pumpOn = on;
  if (on)
    applySpeed();
  else
    analogWrite(PUMP_PIN, 0);
}

// ---------- 时间工具 ----------

bool timeSynced() {
  return ntpSynced && time(nullptr) > TIME_EPOCH_MIN;
}

// 本地时间当天分钟数(0~1439)
int minuteOfDay() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  return lt.tm_hour * 60 + lt.tm_min;
}

String fmtHHMM(int min) {
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", min / 60, min % 60);
  return String(b);
}

String fmtClock() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", lt.tm_hour, lt.tm_min);
  return String(b);
}

void onTimeSync() { // SNTP 首次及每次校准都会回调
  ntpSynced = true;
  Serial.printf("[NTP] 时间已同步 %s\n", fmtClock().c_str());
}

// 是否在每日运行窗口内:开始包含,停止不包含;停止早于开始表示跨午夜
bool inWindow(int mod) {
  if (s.startMin < s.stopMin)
    return mod >= s.startMin && mod < s.stopMin;
  return mod >= s.startMin || mod < s.stopMin;
}

// ---------- 自动模式状态机 ----------

// 每日计划关闭:原有纯间歇循环(上电/切自动后先休息再工作,行为与旧固件一致)
// 每日计划开启:窗口外停止;窗口内连续运行或间歇循环;窗口开始立即进入工作段
void autoLoop() {
  unsigned long onMs = s.onMin * 60000UL;
  unsigned long offMs = s.offMin * 60000UL;

  if (!s.daily) {
    unsigned long el = millis() - since; // unsigned 自动处理 49 天溢出
    if (pumpOn && el >= onMs) {
      pump(false);
      since = millis();
    } else if (!pumpOn && el >= offMs) { // else if:同轮不能先关又开
      pump(true);
      since = millis();
    }
    return;
  }

  // 时间未同步:保持停止并显示等待,不猜测时刻,避免反复开关泵
  if (!timeSynced()) {
    if (pumpOn)
      pump(false);
    wasSynced = false;
    wasInWindow = false;
    return;
  }
  if (!wasSynced) {
    // 首次校时(或重启后重新同步):不推测断网期间的周期,从现在开始新的工作段
    wasSynced = true;
    wasInWindow = false;
  }

  bool inw = inWindow(minuteOfDay());
  if (inw && !wasInWindow) { // 进入运行窗口:重置循环相位,立即进入工作段
    since = millis();
    if (s.cycle)
      pump(true);
  }
  if (!inw) {
    if (pumpOn)
      pump(false); // 到停止时刻立即停,可截断未完成的工作段
  } else if (!s.cycle) {
    if (!pumpOn)
      pump(true); // 窗口内连续运行
  } else {
    // 窗口内间歇循环:阶段用 millis 计时,窗口边界按本地时钟判断,
    // 避免 NTP 小幅校时造成工作/休息阶段跳变
    unsigned long el = millis() - since;
    if (pumpOn && el >= onMs) {
      pump(false);
      since = millis();
    } else if (!pumpOn && el >= offMs) {
      pump(true);
      since = millis();
    }
  }
  wasInWindow = inw;
}

// STA 周期性重连:断网不影响热点网页和泵控制
void staReconnect() {
  if (!STA_SSID[0] || WiFi.status() == WL_CONNECTED)
    return;
  if (millis() - lastStaTry < STA_RETRY_MS)
    return;
  lastStaTry = millis();
  WiFi.begin(STA_SSID, STA_PASS); // 不打印密码
}

// ---------- 网页 ----------

String page() {
  String h = "<!DOCTYPE html><html><head><meta name='viewport' "
             "content='width=device-width,initial-scale=1'>";
  h += "<style>body{font-family:sans-serif;max-width:420px;margin:18px "
       "auto;padding:0 14px}h1{font-size:22px}";
  h += ".st{font-size:42px;font-weight:bold;margin:14px "
       "0}.on{color:#1a8}.off{color:#888}";
  h += ".tm{font-size:14px;color:#666;margin-bottom:10px}";
  h += "label{display:block;margin:14px 0 "
       "4px;font-size:15px}input[type=number]{width:100%;padding:10px;font-"
       "size:16px;box-sizing:border-box}";
  h += "input[type=time]{padding:8px;font-size:16px;border-radius:6px;"
       "border:1px solid #ccc}";
  h += "select{width:100%;padding:10px;font-size:16px;margin-top:4px;"
       "border-radius:6px;border:1px solid #ccc;background:#fff}";
  h += "button{width:100%;padding:14px;font-size:17px;margin-top:12px;border:0;"
       "border-radius:8px}";
  h += ".b1{background:#1a8;color:#fff}.b2{background:#eee}.b3{background:#d63;"
       "color:#fff}";
  // 滑块样式
  h += "input[type=range]{width:100%;margin:8px 0;height:36px;"
       "-webkit-appearance:none;background:linear-gradient(90deg,#1a8,#0d5);"
       "border-radius:18px;outline:none}";
  h += "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;"
       "width:32px;height:32px;border-radius:50%;background:#fff;"
       "box-shadow:0 2px 6px rgba(0,0,0,.3);cursor:pointer}";
  h += ".spd{font-size:28px;font-weight:bold;color:#1a8;text-align:center;"
       "margin:4px 0}";
  h += "</style></head><body>";
  h += "<h1>🐟 充氧泵</h1>";
  h += "<div id='st' class='st off'>--</div>";
  h += "<div id='tm' class='tm'>⏳</div>";
  h += "<form action='/set' method='POST'>";

  // 速度滑块
  h += "<label>💨 出氧速度</label>";
  h += "<div class='spd' id='sv'>" + String(s.speed) + "%</div>";
  h += "<input type='range' name='speed' min='10' max='100' step='5' value='" +
       String(s.speed) +
       "' oninput=\"document.getElementById('sv').textContent=this.value+'%'\">";

  // 每日计划
  h += "<label>🕒 每日计划</label>";
  h += "<select name='daily'><option value='0'";
  h += s.daily ? "" : " selected";
  h += ">关闭(全天间歇循环)</option><option value='1'";
  h += s.daily ? " selected" : "";
  h += ">开启(每日定时)</option></select>";
  h += "<label>每天开始运行</label><input type='time' name='start' value='" +
       fmtHHMM(s.startMin) + "'>";
  h += "<label>每天停止运行</label><input type='time' name='stop' value='" +
       fmtHHMM(s.stopMin) + "'>";
  h += "<label>运行时段内</label>";
  h += "<select name='cycle'><option value='0'";
  h += s.cycle ? "" : " selected";
  h += ">连续运行</option><option value='1'";
  h += s.cycle ? " selected" : "";
  h += ">间歇循环(用下方工作/休息时长)</option></select>";

  h += "<label>⏱️ 工作几分钟</label><input type=number name='on' min='1' "
       "max='1440' value='" +
       String(s.onMin) + "'>";
  h += "<label>😴 停几分钟</label><input type=number name='off' min='1' "
       "max='1440' value='" +
       String(s.offMin) + "'>";
  h += "<button class='b1' name='mode' value='0'>保存并自动定时</button>";
  h += "<button class='b2' name='mode' value='1'>立即手动开</button>";
  h += "<button class='b3' name='mode' value='2'>立即手动关</button>";
  h += "</form>";
  h += "<script>setInterval(async()=>{let r=await fetch('/state');let d=await "
       "r.json();"
       "let e=document.getElementById('st');e.textContent=d.on?'工作中 "
       "'+d.speed+'%':'已停止';"
       "e.className='st '+(d.on?'on':'off');"
       "let t=document.getElementById('tm');"
       "t.textContent=(d.sync?('🕒 本地时间 '+d.time):'⏳ 等待时间同步')+' · '+"
       "(d.mode==0?(d.daily?('计划 '+d.start+'~'+d.stop+(d.cycle?' 循环':' "
       "连续')):'全天间歇循环'):(d.mode==1?'手动开':'手动关'));},2000);</script>";
  h += "</body></html>";
  return h;
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", page()); }

void handleState() {
  String j = "{\"on\":" + String(pumpOn ? 1 : 0);
  j += ",\"speed\":" + String(s.speed);
  j += ",\"mode\":" + String(s.mode);
  j += ",\"daily\":" + String(s.daily);
  j += ",\"cycle\":" + String(s.cycle);
  j += ",\"start\":\"" + fmtHHMM(s.startMin) + "\"";
  j += ",\"stop\":\"" + fmtHHMM(s.stopMin) + "\"";
  j += ",\"sync\":" + String(timeSynced() ? 1 : 0);
  j += ",\"time\":\"" + (timeSynced() ? fmtClock() : String("--:--")) + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

// "HH:MM" -> 当天分钟数;非法返回 -1
int parseHHMM(const String &v) {
  int hh, mm;
  if (sscanf(v.c_str(), "%d:%d", &hh, &mm) != 2)
    return -1;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59)
    return -1;
  return hh * 60 + mm;
}

void rejectTime() {
  server.send(400, "text/html; charset=utf-8",
              "<!DOCTYPE html><meta name='viewport' "
              "content='width=device-width,initial-scale=1'><meta "
              "charset='utf-8'><body style='font-family:sans-serif;"
              "max-width:420px;margin:40px auto;padding:0 14px'>"
              "<h2>⛔ 保存失败</h2><p>开始与停止时刻不能相同,且需为 00:00 ~ "
              "23:59 之间的有效时间。</p><p><a href='/'>返回修改</a></p>"
              "</body>");
}

void handleSet() {
  if (server.hasArg("on"))
    s.onMin = constrain((uint32_t)server.arg("on").toInt(), 1UL, 1440UL);
  if (server.hasArg("off"))
    s.offMin = constrain((uint32_t)server.arg("off").toInt(), 1UL, 1440UL);
  if (server.hasArg("speed")) {
    s.speed = constrain(server.arg("speed").toInt(), 10, 100);
    if (pumpOn)
      applySpeed(); // 运行中立即应用新速度
  }
  // 每日计划字段:后端统一校验,开始=停止或格式非法时拒绝保存
  if (server.hasArg("start") || server.hasArg("stop") || server.hasArg("daily") ||
      server.hasArg("cycle")) {
    int start =
        server.hasArg("start") ? parseHHMM(server.arg("start")) : s.startMin;
    int stop =
        server.hasArg("stop") ? parseHHMM(server.arg("stop")) : s.stopMin;
    if (start < 0 || stop < 0 || start == stop) {
      rejectTime();
      return;
    }
    s.startMin = start;
    s.stopMin = stop;
    if (server.hasArg("daily"))
      s.daily = server.arg("daily").toInt() ? 1 : 0;
    if (server.hasArg("cycle"))
      s.cycle = server.arg("cycle").toInt() ? 1 : 0;
  }
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 0 && m <= 2) {
      s.mode = m;
      if (s.mode == 0) {
        since = millis(); // 重启自动周期
        wasSynced = wasInWindow = false; // 立即按当前本地时间重算
      } else {
        pump(s.mode == 1); // 手动开/关覆盖计划
      }
    }
  }
  save();
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  pinMode(PUMP_PIN, OUTPUT);
  analogWriteFreq(20000); // 20kHz PWM, HDVP1-B6 要求 10~30kHz
  analogWrite(PUMP_PIN, 0); // 初始停止
  Serial.begin(115200);
  load();
  // AP+STA:保留热点供手机本地控制,同时连家庭路由器获取 NTP 时间
  WiFi.mode(STA_SSID[0] ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.println(WiFi.softAPIP()); // 打印 192.168.4.1
  if (STA_SSID[0]) {
    Serial.printf("[STA] 正在连接 %s ...\n", STA_SSID);
    onGotIp = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP &e) {
      Serial.printf("[STA] 已连接, IP: %s, 开始 NTP 校时\n",
                    e.ip.toString().c_str());
    });
    onStaDisc = WiFi.onStationModeDisconnected(
        [](const WiFiEventStationModeDisconnected &e) {
          Serial.printf("[STA] 断开 (reason=%d)\n", e.reason);
        });
    WiFi.begin(STA_SSID, STA_PASS); // 密码不写入日志
    lastStaTry = millis();
  }
  settimeofday_cb(onTimeSync);
  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
  server.on("/", handleRoot);
  server.on("/state", handleState);
  // 表单改用 POST 防浏览器预取误改配置;保留 GET 兼容 curl 等直接调用
  server.on("/set", HTTP_POST, handleSet);
  server.on("/set", HTTP_GET, handleSet);
  server.begin();
  // 手动开跨重启保持(自动/手动关从停止开始,自动模式由 autoLoop 决定)
  pump(s.mode == 1);
  since = millis();
}

void loop() {
  server.handleClient();
  staReconnect();
  if (s.mode == 0) // 自动定时
    autoLoop();
  // 每 10 秒串口心跳,随时挂上监控都能看到当前连接/同步/泵状态
  static unsigned long lastReport = 0;
  if (millis() - lastReport >= 10000) {
    lastReport = millis();
    Serial.printf("[状态] 路由器:%s | NTP:%s | 泵:%s\n",
                  WiFi.status() == WL_CONNECTED
                      ? WiFi.localIP().toString().c_str()
                      : "未连接",
                  timeSynced() ? fmtClock().c_str() : "未同步",
                  pumpOn ? "工作中" : "停止");
  }
}
