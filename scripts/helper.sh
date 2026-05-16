#!/system/bin/sh
# helper.sh — WebUI 与 Daemon 的通信桥梁
# KernelSU / Magisk 通用

MODDIR="/data/adb/modules/redirector"
DAEMON="$MODDIR/bin/daemon"
PID_FILE="$MODDIR/daemon.pid"
LOG_FILE="$MODDIR/daemon.log"
CONF_DIR="$MODDIR/config"
REDIRECTS_CONF="$CONF_DIR/redirects.conf"
MODE_CONF="$CONF_DIR/mode.conf"
ACTIVATED_CONF="$CONF_DIR/activated.conf"
ORG_CONF="$CONF_DIR/organizer.conf"

# ========== 工具函数 ==========
ensure_dir() {
    [ -d "$1" ] || mkdir -p "$1"
}

daemon_pid() {
    if [ -f "$PID_FILE" ]; then
        cat "$PID_FILE" 2>/dev/null
    else
        pgrep -f "redirect_daemon" 2>/dev/null | head -n1
    fi
}

is_running() {
    local pid=$(daemon_pid)
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

# ========== 命令处理 ==========
case "$1" in
    start)
        if is_running; then
            echo '{"status":"already_running","pid":'"$(daemon_pid)"'}'
            exit 0
        fi
        ensure_dir "$CONF_DIR"
        [ ! -f "$REDIRECTS_CONF" ] && echo "# 易の重定向 配置文件" > "$REDIRECTS_CONF"
        [ ! -f "$MODE_CONF" ] && echo "inotify" > "$MODE_CONF"
        [ ! -f "$ORG_CONF" ] && echo "# File Organizer Config" > "$ORG_CONF"
        [ ! -f "$ACTIVATED_CONF" ] && echo "0" > "$ACTIVATED_CONF"

        nohup "$DAEMON" "$MODDIR" > /dev/null 2>&1 &
        local new_pid=$!
        echo $new_pid > "$PID_FILE"
        sleep 0.5
        if kill -0 $new_pid 2>/dev/null; then
            echo '{"status":"started","pid":'"$new_pid"'}'
        else
            echo '{"status":"failed"}'
            rm -f "$PID_FILE"
        fi
        ;;

    stop)
        local pid=$(daemon_pid)
        if [ -n "$pid" ]; then
            kill -TERM "$pid" 2>/dev/null
            sleep 1
            kill -KILL "$pid" 2>/dev/null
            rm -f "$PID_FILE"
        fi
        echo '{"status":"stopped"}'
        ;;

    restart)
        $0 stop >/dev/null 2>&1
        sleep 1
        $0 start
        ;;

    status)
        local pid=$(daemon_pid)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            echo '{"status":"running","pid":'"$pid"'}'
        else
            rm -f "$PID_FILE"
            echo '{"status":"stopped","pid":0}'
        fi
        ;;

    get_mode)
        if [ -f "$MODE_CONF" ]; then
            cat "$MODE_CONF" 2>/dev/null | tr -d '[:space:]' | head -c20
        else
            echo "inotify"
        fi
        ;;

    set_mode)
        local mode="$2"
        [ -z "$mode" ] && mode="inotify"
        echo "$mode" > "$MODE_CONF"
        touch "$MODE_CONF"
        echo '{"status":"ok","mode":"'"$mode"'"}'
        ;;

    get_activated)
        if [ -f "$ACTIVATED_CONF" ]; then
            cat "$ACTIVATED_CONF" 2>/dev/null | tr -d '[:space:]' | head -c1
        else
            echo "0"
        fi
        ;;

    sysinfo)
        local model=$(getprop ro.product.model 2>/dev/null)
        local rules=0
        if [ -f "$REDIRECTS_CONF" ]; then
            rules=$(grep -v '^#' "$REDIRECTS_CONF" 2>/dev/null | grep -v '^$' | wc -l)
        fi
        local uptime_sec=$(cut -d. -f1 /proc/uptime 2>/dev/null)
        local uptime_str="--"
        if [ -n "$uptime_sec" ]; then
            local d=$((uptime_sec / 86400))
            local h=$(((uptime_sec % 86400) / 3600))
            local m=$(((uptime_sec % 3600) / 60))
            [ $d -gt 0 ] && uptime_str="${d}d ${h}h ${m}m" || uptime_str="${h}h ${m}m"
        fi
        echo '{"model":"'"$model"'","rules":'"$rules"',"uptime":"'"$uptime_str"'"}'
        ;;

    cpu_stat)
        local stat1=$(cat /proc/stat 2>/dev/null | head -1)
        if [ -z "$stat1" ]; then
            echo '{"cpu_percent":0}'
            exit 0
        fi
        local user1=$(echo "$stat1" | awk '{print $2}')
        local nice1=$(echo "$stat1" | awk '{print $3}')
        local sys1=$(echo "$stat1" | awk '{print $4}')
        local idle1=$(echo "$stat1" | awk '{print $5}')
        local iowait1=$(echo "$stat1" | awk '{print $6}')
        local irq1=$(echo "$stat1" | awk '{print $7}')
        local softirq1=$(echo "$stat1" | awk '{print $8}')
        local total1=$((user1 + nice1 + sys1 + idle1 + iowait1 + irq1 + softirq1))

        sleep 0.8

        local stat2=$(cat /proc/stat 2>/dev/null | head -1)
        local user2=$(echo "$stat2" | awk '{print $2}')
        local nice2=$(echo "$stat2" | awk '{print $3}')
        local sys2=$(echo "$stat2" | awk '{print $4}')
        local idle2=$(echo "$stat2" | awk '{print $5}')
        local iowait2=$(echo "$stat2" | awk '{print $6}')
        local irq2=$(echo "$stat2" | awk '{print $7}')
        local softirq2=$(echo "$stat2" | awk '{print $8}')
        local total2=$((user2 + nice2 + sys2 + idle2 + iowait2 + irq2 + softirq2))

        local total_diff=$((total2 - total1))
        local idle_diff=$((idle2 - idle1 + iowait2 - iowait1))

        if [ "$total_diff" -gt 0 ]; then
            local usage=$((100 * (total_diff - idle_diff) / total_diff))
            [ "$usage" -lt 0 ] && usage=0
            [ "$usage" -gt 100 ] && usage=100
            echo '{"cpu_percent":'"$usage"'}'
        else
            echo '{"cpu_percent":0}'
        fi
        ;;

    get_log)
        local lines="${2:-200}"
        if [ -f "$LOG_FILE" ]; then
            tail -n "$lines" "$LOG_FILE" 2>/dev/null
        else
            echo "暂无日志"
        fi
        ;;

    clear_log)
        : > "$LOG_FILE" 2>/dev/null
        echo '{"status":"cleared"}'
        ;;

    *)
        echo "Usage: $0 {start|stop|restart|status|get_mode|set_mode <mode>|get_activated|sysinfo|cpu_stat|get_log [lines]|clear_log}"
        exit 1
        ;;
esac
