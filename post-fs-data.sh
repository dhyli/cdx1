#!/system/bin/sh
# post-fs-data.sh — 在 post-fs-data 阶段执行挂载
# 仅 mount 模式需要此脚本
# inotify 模式由 service.sh 启动守护进程处理

MODDIR="${0%/*}"
MODE_CONF="$MODDIR/config/mode.conf"

if [ -f "$MODE_CONF" ]; then
    MODE=$(cat "$MODE_CONF" 2>/dev/null | tr -d '[:space:]')
else
    MODE="inotify"
fi

[ "$MODE" != "mount" ] && exit 0

# Mount 模式下不直接挂载，等 service.sh 启动 daemon 统一处理
# 这样可以确保配置文件可读（post-fs-data 阶段 /data 可能还未完全就绪）
exit 0
