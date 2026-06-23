// 充氧泵控制器 - ESP8266 AP 热点模式,手机浏览器控制
// 板子:Arduino IDE 选 "Wemos D1 mini" (LOLIN(WEMOS) D1 mini)
// 接线:PUMP_PIN(D1) 接泵蓝线(PWM);泵红线接5V;泵黑线接GND;绿线黄线悬空
// 泵型号:Kamoer HDVP1-B6 (6V/3W, 内置无刷驱动, 支持 PWM 调速)

#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#define PUMP_PIN D1 // 接泵蓝线(PWM 调速)

const char *AP_SSID = "OxygenPump"; // 手机上搜到的 WiFi 名
const char *AP_PASS = "12345678";   // 至少 8 位,可改

ESP8266WebServer server(80);

struct Settings {
  uint32_t magic;  // 校验位,判断 EEPROM 是否已初始化
  uint32_t onMin;  // 工作分钟
  uint32_t offMin; // 停止分钟
  uint8_t mode;    // 0=自动定时 1=手动开 2=手动关
  uint8_t speed;   // 速度百分比 10~100
} s;

bool pumpOn = false;
unsigned long since = 0;

void load() {
  EEPROM.begin(128);
  EEPROM.get(0, s);
  if (s.magic != 0xA5A5A5A5) { // 首次上电写默认值
    s.magic = 0xA5A5A5A5;
    s.onMin = 2;
    s.offMin = 10;
    s.mode = 0;
    s.speed = 50; // 默认 50% 速度
    EEPROM.put(0, s);
    EEPROM.commit();
  }
  // 兼容旧固件:speed 为 0 说明是旧数据,设为默认值
  if (s.speed < 10 || s.speed > 100) {
    s.speed = 50;
  }
}
void save() {
  EEPROM.put(0, s);
  EEPROM.commit();
}

// PWM 值: 速度百分比 -> 0~1023
// HDVP1-B6: PWM 高=转, 低=停
// 最低 10% 保证泵能启动
int speedToPWM(uint8_t pct) {
  return map(constrain(pct, 10, 100), 0, 100, 0, 1023);
}

void pump(bool on) {
  pumpOn = on;
  if (on) {
    analogWrite(PUMP_PIN, speedToPWM(s.speed));
  } else {
    analogWrite(PUMP_PIN, 0);
  }
}

String page() {
  String h = "<!DOCTYPE html><html><head><meta name='viewport' "
             "content='width=device-width,initial-scale=1'>";
  h += "<style>body{font-family:sans-serif;max-width:420px;margin:18px "
       "auto;padding:0 14px}h1{font-size:22px}";
  h += ".st{font-size:42px;font-weight:bold;margin:14px "
       "0}.on{color:#1a8}.off{color:#888}";
  h += "label{display:block;margin:14px 0 "
       "4px;font-size:15px}input[type=number]{width:100%;padding:10px;font-"
       "size:16px;box-sizing:border-box}";
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
  h += "<form action='/set'>";

  // 速度滑块
  h += "<label>💨 出氧速度</label>";
  h += "<div class='spd' id='sv'>" + String(s.speed) + "%</div>";
  h += "<input type='range' name='speed' min='10' max='100' step='5' value='" +
       String(s.speed) +
       "' oninput=\"document.getElementById('sv').textContent=this.value+'%'\">";

  h += "<label>工作几分钟</label><input type='number' name='on' min='1' "
       "max='1440' value='" +
       String(s.onMin) + "'>";
  h += "<label>停几分钟</label><input type='number' name='off' min='1' "
       "max='1440' value='" +
       String(s.offMin) + "'>";
  h += "<button class='b1' name='mode' value='0'>保存并自动定时</button>";
  h += "<button class='b2' name='mode' value='1'>立即手动开</button>";
  h += "<button class='b3' name='mode' value='2'>立即手动关</button>";
  h += "</form>";
  h += "<script>setInterval(async()=>{let r=await fetch('/state');let d=await "
       "r.json();"
       "let "
       "e=document.getElementById('st');e.textContent=d.on?'工作中 "
       "'+d.speed+'%':'已停止';"
       "e.className='st '+(d.on?'on':'off');},2000);</script>";
  h += "</body></html>";
  return h;
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", page()); }
void handleState() {
  server.send(200, "application/json",
              "{\"on\":" + String(pumpOn ? 1 : 0) +
                  ",\"speed\":" + String(s.speed) + "}");
}
void handleSet() {
  if (server.hasArg("on"))
    s.onMin = max(1UL, (unsigned long)server.arg("on").toInt());
  if (server.hasArg("off"))
    s.offMin = max(1UL, (unsigned long)server.arg("off").toInt());
  if (server.hasArg("speed")) {
    int spd = server.arg("speed").toInt();
    s.speed = constrain(spd, 10, 100);
    // 如果泵正在转,立即应用新速度
    if (pumpOn) {
      analogWrite(PUMP_PIN, speedToPWM(s.speed));
    }
  }
  if (server.hasArg("mode")) {
    s.mode = server.arg("mode").toInt();
    if (s.mode == 1)
      pump(true);
    if (s.mode == 2)
      pump(false);
    if (s.mode == 0)
      since = millis(); // 重启自动周期
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
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.println(WiFi.softAPIP()); // 打印 192.168.4.1
  server.on("/", handleRoot);
  server.on("/state", handleState);
  server.on("/set", HTTP_GET, handleSet);
  server.begin();
  pump(false);
  since = millis();
}

void loop() {
  server.handleClient();
  if (s.mode == 0) { // 自动定时
    unsigned long onMs = s.onMin * 60000UL;
    unsigned long offMs = s.offMin * 60000UL;
    unsigned long el = millis() - since; // unsigned 自动处理 49 天溢出
    if (pumpOn && el >= onMs) {
      pump(false);
      since = millis();
    }
    if (!pumpOn && el >= offMs) {
      pump(true);
      since = millis();
    }
  }
}
