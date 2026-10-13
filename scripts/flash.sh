#!/usr/bin/env bash
# 一键编译/烧录脚本
# 1. 读取 .env.local 里的 WIFI_SSID / WIFI_PASS
# 2. 生成 oxygen_pump/oxygen_pump.ino.globals.h(ESP8266 core 编译时自动强制
#    include 该文件,实现编译期注入,固件源码无需改动)
# 3. 调 arduino-cli 编译并烧录(globals.h 和 .env.local 均已被 git 忽略)
#
# 用法:
#   bash scripts/flash.sh                # 自动探测串口,编译 + 烧录
#   bash scripts/flash.sh -p /dev/cu.XXX # 指定串口
#   bash scripts/flash.sh -c             # 只生成注入文件并编译,不烧录
#
# 用 Arduino IDE 的用户:每次修改 .env.local 后先跑一次
#   bash scripts/flash.sh -c
# 生成 globals.h 之后,在 IDE 里直接上传即可(IDE 编译同样会自动带上它)。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENV_FILE="$ROOT/.env.local"
GLOBALS_H="$ROOT/oxygen_pump/oxygen_pump.ino.globals.h"
FQBN="${FQBN:-esp8266:esp8266:d1_mini}"

# 找 arduino-cli:PATH 优先,其次用 Arduino IDE 自带的
CLI="$(command -v arduino-cli || true)"
if [ -z "$CLI" ]; then
  CANDIDATE="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
  if [ -x "$CANDIDATE" ]; then
    CLI="$CANDIDATE"
  fi
fi
if [ -z "$CLI" ]; then
  echo "错误: 未找到 arduino-cli,请安装 (brew install arduino-cli) 或安装 Arduino IDE" >&2
  exit 1
fi

# ---- 读取 .env.local ----
if [ ! -f "$ENV_FILE" ]; then
  echo "错误: 未找到 .env.local。请先执行:" >&2
  echo "  cp .env.sample .env.local   然后填写 WIFI_SSID / WIFI_PASS" >&2
  exit 1
fi
SSID=""
PASS=""
while IFS='=' read -r key val || [ -n "$key" ]; do
  key="$(echo "$key" | tr -d '[:space:]')"
  case "$key" in "" | \#*) continue ;; esac
  val="${val%%$'\r'}" # 兼容 CRLF
  val="${val%\"}" && val="${val#\"}" # 去掉成对的英文引号
  [ "$key" = "WIFI_SSID" ] && SSID="$val"
  [ "$key" = "WIFI_PASS" ] && PASS="$val"
done < "$ENV_FILE"

if [[ "$SSID" == *'"'* || "$PASS" == *'"'* || "$SSID" == *'\'* || "$PASS" == *'\'* ]]; then
  echo "错误: WIFI_SSID / WIFI_PASS 不能包含英文双引号或反斜杠" >&2
  exit 1
fi

# ---- 生成 globals.h(core 强制 include,编译期注入) ----
cat > "$GLOBALS_H" <<EOF
// 本文件由 scripts/flash.sh 从 .env.local 自动生成,已被 git 忽略,请勿手动提交
#define STA_SSID "$SSID"
#define STA_PASS "$PASS"
EOF
if [ -n "$SSID" ]; then
  echo "已生成 oxygen_pump.ino.globals.h (WiFi: $SSID)"
else
  echo "已生成 oxygen_pump.ino.globals.h (WiFi 未配置,将编译为纯热点模式)"
fi

# ---- 解析参数 ----
PORT=""
COMPILE_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
  -p | --port)
    PORT="$2"
    shift 2
    ;;
  -c | --compile-only)
    COMPILE_ONLY=1
    shift
    ;;
  *)
    echo "未知参数: $1 (可用: -p <串口> / -c 只编译)" >&2
    exit 1
    ;;
  esac
done

if [ "$COMPILE_ONLY" -eq 0 ] && [ -z "$PORT" ]; then
  PORT="$(ls /dev/cu.usbserial-* /dev/cu.usbmodem* 2>/dev/null | head -n1 || true)"
  if [ -z "$PORT" ]; then
    echo "未检测到串口设备,改为只编译(插上板子重跑,或用 -p 指定串口)"
    COMPILE_ONLY=1
  else
    echo "检测到串口: $PORT"
  fi
fi

if [ "$COMPILE_ONLY" -eq 1 ]; then
  "$CLI" compile --fqbn "$FQBN" "$ROOT/oxygen_pump"
else
  "$CLI" compile --upload -p "$PORT" --fqbn "$FQBN" "$ROOT/oxygen_pump"
fi
