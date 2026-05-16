#!/system/bin/sh
# service.sh — 开机启动守护进程
# Magisk / KernelSU 通用

MODDIR="${0%/*}"
DAEMON="$MODDIR/bin/daemon"
CONF_DIR="$MODDIR/config"
PID_FILE="$MODDIR/daemon.pid"
LOG_FILE="$MODDIR/daemon.log"

# 等待系统完全启动
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 1
done
sleep 3

# 设置权限
chmod 755 "$DAEMON" 2>/dev/null
chmod 755 "$MODDIR/scripts/helper.sh" 2>/dev/null

# 确保配置目录存在
mkdir -p "$CONF_DIR"

# 确保配置文件存在
[ ! -f "$CONF_DIR/redirects.conf" ] && echo "# 易の重定向 配置文件" > "$CONF_DIR/redirects.conf"
[ ! -f "$CONF_DIR/mode.conf" ] && echo "inotify" > "$CONF_DIR/mode.conf"
[ ! -f "$CONF_DIR/organizer.conf" ] && echo "# File Organizer Config" > "$CONF_DIR/organizer.conf"
[ ! -f "$CONF_DIR/activated.conf" ] && echo "0" > "$CONF_DIR/activated.conf"

# 检查是否已有进程在运行
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        kill "$OLD_PID" 2>/dev/null
        sleep 1
    fi
    rm -f "$PID_FILE"
fi

# 启动守护进程（后台）
nohup "$DAEMON" "$MODDIR" > /dev/null 2>&1 &
echo $! > "$PID_FILE"
