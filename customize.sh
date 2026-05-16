#!/system/bin/sh
# customize.sh — Magisk/KSU 模块安装脚本

SKIPUNZIP=0

set_perm_recursive() {
    set_perm "$1" 0 0 0755 0755
    for f in "$1"/*; do
        if [ -d "$f" ]; then
            set_perm_recursive "$f"
        else
            set_perm "$f" 0 0 0755 0644
        fi
    done
}

ui_print "- 解压模块文件..."
unzip -o "$ZIPFILE" -x 'META-INF/*' -d "$MODPATH" >&2

ui_print "- 设置权限..."
set_perm "$MODPATH/bin/daemon" 0 0 0755
set_perm "$MODPATH/scripts/helper.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

ui_print "- 创建配置目录..."
mkdir -p "$MODPATH/config"

[ ! -f "$MODPATH/config/redirects.conf" ] && echo "# 易の重定向 配置文件" > "$MODPATH/config/redirects.conf"
[ ! -f "$MODPATH/config/mode.conf" ] && echo "inotify" > "$MODPATH/config/mode.conf"
[ ! -f "$MODPATH/config/organizer.conf" ] && echo "# File Organizer Config" > "$MODPATH/config/organizer.conf"
[ ! -f "$MODPATH/config/activated.conf" ] && echo "0" > "$MODPATH/config/activated.conf"

ui_print "- 安装完成!"
ui_print ""
ui_print "  模式: Inotify 监控 (默认)"
ui_print "  WebUI: 打开 KernelSU 管理器 → 模块 → 易の重定向"
ui_print ""
