#!/system/bin/sh
# uninstall.sh — 清理挂载点和残留进程

MODDIR="/data/adb/modules/redirector"
CONF="$MODDIR/config/redirects.conf"

# 停止守护进程
if [ -f "$MODDIR/daemon.pid" ]; then
    PID=$(cat "$MODDIR/daemon.pid" 2>/dev/null)
    if [ -n "$PID" ]; then
        kill -TERM "$PID" 2>/dev/null
        sleep 1
        kill -KILL "$PID" 2>/dev/null
    fi
    rm -f "$MODDIR/daemon.pid"
fi

# 清理所有 bind mount（从配置中读取源路径并卸载）
if [ -f "$CONF" ]; then
    while IFS='|' read -r enabled source target type comment; do
        [ -z "$source" ] && continue
        # 尝试卸载（忽略错误）
        umount -l "$source" 2>/dev/null
    done < "$CONF"
fi

# 清理日志
rm -f "$MODDIR/daemon.log" "$MODDIR/daemon.log.old"

# 注意：保留用户配置目录，方便重新安装后恢复
# 如需清理配置，取消下一行注释：
# rm -rf "$MODDIR/config"
