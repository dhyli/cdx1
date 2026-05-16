/*
 * redirect_daemon.c — 易の重定向 守护进程 (优化版)
 *
 * 改进内容：
 *   1. 日志轮转（超过 1MB 自动归档）
 *   2. SELinux 上下文复制（xattr）
 *   3. 配置自动备份（.bak）
 *   4. Inotify 递归目录监控（自动追踪新建子目录）
 *   5. Mount 模式文件类型修复（正确创建目标文件）
 *   6. 线程崩溃后自动恢复预留接口
 *
 * 编译（Termux）:
 *   clang -static -O2 -o daemon redirect_daemon.c
 *
 * 编译（NDK）:
 *   aarch64-linux-android34-clang -static -O2 -o daemon redirect_daemon.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <pthread.h>

/* ============ 配置常量 ============ */

#define MAX_RULES       128
#define MAX_ORG_RULES   64
#define MAX_PATH        1024
#define MAX_LINE        1024
#define BUF_SIZE        8192
#define LOG_MAX_SIZE    (1024 * 1024)   /* 1MB 日志轮转 */

#define MODE_MOUNT      0
#define MODE_INOTIFY    1

#define TYPE_DIR        0
#define TYPE_FILE       1

#define CONF_DIR        "/data/adb/modules/redirector/config"
#define REDIRECTS_CONF  CONF_DIR "/redirects.conf"
#define MODE_CONF       CONF_DIR "/mode.conf"
#define ACTIVATED_CONF  CONF_DIR "/activated.conf"
#define ORG_CONF        CONF_DIR "/organizer.conf"
#define LOG_FILE        "/data/adb/modules/redirector/daemon.log"
#define PID_FILE        "/data/adb/modules/redirector/daemon.pid"
#define MOD_DIR         "/data/adb/modules/redirector"

/* ============ 数据结构 ============ */

typedef struct {
    int  enabled;
    char source[MAX_PATH];
    char target[MAX_PATH];
    int  type;          /* 0=dir, 1=file */
    char comment[128];
} RedirectRule;

typedef struct {
    char ext[32];
    char dest[MAX_PATH];
} OrgRule;

typedef struct {
    RedirectRule rules[MAX_RULES];
    int          rule_count;
    OrgRule      org_rules[MAX_ORG_RULES];
    int          org_rule_count;
    int          org_interval;     /* 秒 */
    int          run_mode;         /* MODE_MOUNT or MODE_INOTIFY */
    int          daemon_interval;  /* 主循环间隔秒数 */
    time_t       redirects_mtime;
    time_t       mode_mtime;
    time_t       org_mtime;
    int          running;
} DaemonState;

/* 用于递归 inotify 监控的 watch 映射 */
typedef struct WatchMap {
    int wd;
    char path[MAX_PATH];
    struct WatchMap *next;
} WatchMap;

typedef struct {
    const RedirectRule *rule;
    int                 inotify_fd;
    pthread_t           thread;
    volatile int        running;
    pthread_mutex_t     lock;
    WatchMap           *watches;
} InotifyCtx;

static DaemonState g_state;
static FILE       *g_log = NULL;
static InotifyCtx  g_inotify_ctx[MAX_RULES];

/* ============ 日志与轮转 ============ */

static void log_init(void) {
    g_log = fopen(LOG_FILE, "a");
    if (!g_log) g_log = stderr;
}

static void log_close(void) {
    if (g_log && g_log != stderr) fclose(g_log);
}

static void log_rotate(void) {
    if (!g_log || g_log == stderr) return;
    int fd = fileno(g_log);
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > LOG_MAX_SIZE) {
        fclose(g_log);
        char old_log[MAX_PATH];
        snprintf(old_log, sizeof(old_log), "%s.old", LOG_FILE);
        unlink(old_log);
        rename(LOG_FILE, old_log);
        g_log = fopen(LOG_FILE, "a");
        if (!g_log) g_log = stderr;
    }
}

static void log_msg(const char *level, const char *fmt, ...) {
    if (!g_log) return;
    log_rotate();

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    fprintf(g_log, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, level);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);

    fprintf(g_log, "\n");
    fflush(g_log);
}

#define LOGI(...) log_msg("INFO", __VA_ARGS__)
#define LOGW(...) log_msg("WARN", __VA_ARGS__)
#define LOGE(...) log_msg("ERROR", __VA_ARGS__)

/* ============ 工具函数 ============ */

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static time_t get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_mtime;
    return 0;
}

static void trim(char *s) {
    if (!s || !*s) return;
    char *end;
    while (isspace((unsigned char)*s)) s++;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
}

/* 复制 SELinux 上下文 */
static int copy_selinux_ctx(const char *src, const char *dst) {
    char ctx[512];
    ssize_t len = getxattr(src, "security.selinux", ctx, sizeof(ctx) - 1);
    if (len > 0) {
        ctx[len] = '\0';
        if (lsetxattr(dst, "security.selinux", ctx, len, 0) == 0) {
            return 0;
        }
    }
    return -1;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;

    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) == 0) {
                char parent[MAX_PATH];
                snprintf(parent, sizeof(parent), "%s", tmp);
                char *last = strrchr(parent, '/');
                if (last && last != parent) {
                    *last = 0;
                    copy_selinux_ctx(parent, tmp);
                }
            }
            *p = '/';
        }
    }
    int ret = mkdir(tmp, 0755);
    if (ret == 0) {
        char parent[MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", tmp);
        char *last = strrchr(parent, '/');
        if (last && last != parent) {
            *last = 0;
            copy_selinux_ctx(parent, tmp);
        }
    }
    return ret;
}

/* 递归复制（含 SELinux 上下文） */
static int copy_recursive(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) < 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        ensure_dir(dst);
        copy_selinux_ctx(src, dst);

        DIR *d = opendir(src);
        if (!d) return -1;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char cs[MAX_PATH], cd[MAX_PATH];
            snprintf(cs, sizeof(cs), "%s/%s", src, ent->d_name);
            snprintf(cd, sizeof(cd), "%s/%s", dst, ent->d_name);
            copy_recursive(cs, cd);
        }
        closedir(d);
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        char dst_dir[MAX_PATH];
        snprintf(dst_dir, sizeof(dst_dir), "%s", dst);
        char *slash = strrchr(dst_dir, '/');
        if (slash) { *slash = 0; ensure_dir(dst_dir); }

        int in_fd = open(src, O_RDONLY);
        if (in_fd < 0) return -1;
        int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
        if (out_fd < 0) { close(in_fd); return -1; }

        char buf[BUF_SIZE];
        ssize_t n;
        while ((n = read(in_fd, buf, sizeof(buf))) > 0)
            write(out_fd, buf, n);

        close(in_fd);
        close(out_fd);

        /* 复制 SELinux 上下文 */
        copy_selinux_ctx(src, dst);
        return 0;
    }

    if (S_ISLNK(st.st_mode)) {
        char link[MAX_PATH];
        ssize_t len = readlink(src, link, sizeof(link) - 1);
        if (len > 0) { link[len] = 0; symlink(link, dst); }
        return 0;
    }

    return -1;
}

/* 递归删除 */
static int rm_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return -1;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[MAX_PATH];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            rm_recursive(child);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
    return 0;
}

/* 获取文件扩展名 */
static const char *get_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

/* 获取文件名 */
static const char *get_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* 配置备份 */
static void backup_config(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0) return;

    char bak[MAX_PATH];
    snprintf(bak, sizeof(bak), "%s.bak", path);

    int in_fd = open(path, O_RDONLY);
    if (in_fd < 0) return;
    int out_fd = open(bak, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { close(in_fd); return; }

    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0)
        write(out_fd, buf, n);

    close(in_fd);
    close(out_fd);
}

/* ============ 配置解析 ============ */

static int load_redirects(const char *path) {
    backup_config(path);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOGW("Cannot open redirects.conf: %s", path);
        return -1;
    }

    g_state.rule_count = 0;
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), f) && g_state.rule_count < MAX_RULES) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;

        RedirectRule *r = &g_state.rules[g_state.rule_count];
        memset(r, 0, sizeof(RedirectRule));

        char *p = line;
        char *tok;
        int field = 0;

        while ((tok = strsep(&p, "|")) != NULL && field < 5) {
            trim(tok);
            switch (field) {
                case 0: r->enabled = (atoi(tok) == 1); break;
                case 1: strncpy(r->source, tok, MAX_PATH - 1); break;
                case 2: strncpy(r->target, tok, MAX_PATH - 1); break;
                case 3: r->type = (strcmp(tok, "file") == 0) ? TYPE_FILE : TYPE_DIR; break;
                case 4: strncpy(r->comment, tok, 127); break;
            }
            field++;
        }

        if (r->source[0] && r->target[0]) {
            LOGI("Rule[%d]: %s -> %s (%s, %s)",
                 g_state.rule_count, r->source, r->target,
                 r->enabled ? "ON" : "OFF", r->comment);
            g_state.rule_count++;
        }
    }

    fclose(f);
    g_state.redirects_mtime = get_mtime(path);
    LOGI("Loaded %d redirect rules", g_state.rule_count);
    return 0;
}

static int load_mode(const char *path) {
    backup_config(path);
    FILE *f = fopen(path, "r");
    if (!f) {
        g_state.run_mode = MODE_INOTIFY;
        return -1;
    }

    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        trim(buf);
        if (strcmp(buf, "mount") == 0)
            g_state.run_mode = MODE_MOUNT;
        else
            g_state.run_mode = MODE_INOTIFY;
    }
    fclose(f);
    g_state.mode_mtime = get_mtime(path);

    LOGI("Run mode: %s", g_state.run_mode == MODE_MOUNT ? "mount" : "inotify");
    return 0;
}

static int load_organizer(const char *path) {
    backup_config(path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    g_state.org_rule_count = 0;
    g_state.org_interval = 300;
    char line[MAX_LINE];

    while (fgets(line, sizeof(line), f) && g_state.org_rule_count < MAX_ORG_RULES) {
        trim(line);
        if (!line[0]) continue;

        if (line[0] == '#') {
            char *p = strstr(line, "interval:");
            if (p) {
                p += 9;
                while (*p == ' ') p++;
                g_state.org_interval = atoi(p);
                if (g_state.org_interval < 10) g_state.org_interval = 10;
            }
            continue;
        }

        char *sep = strchr(line, '|');
        if (!sep) continue;

        *sep = 0;
        OrgRule *r = &g_state.org_rules[g_state.org_rule_count];
        memset(r, 0, sizeof(OrgRule));

        strncpy(r->ext, line, sizeof(r->ext) - 1);
        trim(r->ext);
        for (char *c = r->ext; *c; c++) *c = tolower(*c);

        strncpy(r->dest, sep + 1, MAX_PATH - 1);
        trim(r->dest);

        if (r->ext[0] && r->dest[0]) {
            LOGI("OrgRule[%d]: .%s -> %s", g_state.org_rule_count, r->ext, r->dest);
            g_state.org_rule_count++;
        }
    }

    fclose(f);
    g_state.org_mtime = get_mtime(path);
    LOGI("Loaded %d organizer rules, interval=%ds", g_state.org_rule_count, g_state.org_interval);
    return 0;
}

static void load_all_config(void) {
    load_redirects(REDIRECTS_CONF);
    load_mode(MODE_CONF);
    load_organizer(ORG_CONF);
}

/* ============ 信号处理 ============ */

static void signal_handler(int sig) {
    (void)sig;
    g_state.running = 0;
    LOGI("Received signal %d, shutting down...", sig);
}

/* ============ Mount 模式执行 ============ */

static int do_mount_redirect(const RedirectRule *rule) {
    if (!rule->enabled) return 0;

    LOGI("[mount] %s -> %s", rule->source, rule->target);

    struct stat st;
    if (stat(rule->target, &st) < 0) {
        if (rule->type == TYPE_DIR) {
            ensure_dir(rule->target);
        } else {
            char parent[MAX_PATH];
            snprintf(parent, sizeof(parent), "%s", rule->target);
            char *slash = strrchr(parent, '/');
            if (slash) {
                *slash = 0;
                ensure_dir(parent);
            }
            int fd = open(rule->target, O_WRONLY | O_CREAT, 0644);
            if (fd >= 0) close(fd);
        }
    }

    int ret = mount(rule->target, rule->source, NULL, MS_BIND | MS_REC, NULL);
    if (ret < 0) {
        LOGE("[mount] bind mount failed: %s -> %s: %s",
             rule->target, rule->source, strerror(errno));
        return -1;
    }

    LOGI("[mount] success: %s -> %s", rule->target, rule->source);
    return 0;
}

static int do_mount_unmount(const RedirectRule *rule) {
    if (!rule->source[0]) return 0;

    int ret = umount2(rule->source, MNT_DETACH);
    if (ret < 0 && errno != EINVAL && errno != ENOENT) {
        LOGW("[umount] %s: %s", rule->source, strerror(errno));
    }
    return ret;
}

static void run_mount_mode(void) {
    LOGI("=== Mount mode: applying %d rules ===", g_state.rule_count);

    for (int i = 0; i < g_state.rule_count; i++) {
        do_mount_unmount(&g_state.rules[i]);
    }

    int success = 0;
    for (int i = 0; i < g_state.rule_count; i++) {
        if (g_state.rules[i].enabled) {
            if (do_mount_redirect(&g_state.rules[i]) == 0)
                success++;
        }
    }

    FILE *f = fopen(ACTIVATED_CONF, "w");
    if (f) { fprintf(f, "1"); fclose(f); }

    LOGI("=== Mount mode done: %d/%d succeeded ===", success, g_state.rule_count);
}

/* ============ Inotify 模式执行（递归监控） ============ */

static const char *find_org_dest(const char *filename) {
    const char *ext = get_ext(filename);
    if (!ext[0]) return NULL;

    char lower[32];
    strncpy(lower, ext, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = 0;
    for (char *c = lower; *c; c++) *c = tolower(*c);

    for (int i = 0; i < g_state.org_rule_count; i++) {
        if (strcmp(lower, g_state.org_rules[i].ext) == 0)
            return g_state.org_rules[i].dest;
    }
    return NULL;
}

static int add_inotify_watch(InotifyCtx *ctx, const char *path) {
    uint32_t mask = IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE | IN_MODIFY | IN_ATTRIB;
    int wd = inotify_add_watch(ctx->inotify_fd, path, mask);
    if (wd < 0) {
        LOGW("[inotify] cannot watch %s: %s", path, strerror(errno));
        return -1;
    }

    WatchMap *wm = (WatchMap *)malloc(sizeof(WatchMap));
    if (!wm) return wd;

    wm->wd = wd;
    strncpy(wm->path, path, MAX_PATH - 1);
    wm->path[MAX_PATH - 1] = 0;

    pthread_mutex_lock(&ctx->lock);
    wm->next = ctx->watches;
    ctx->watches = wm;
    pthread_mutex_unlock(&ctx->lock);

    LOGI("[inotify] watching: %s (wd=%d)", path, wd);
    return wd;
}

static void remove_inotify_watch(InotifyCtx *ctx, int wd) {
    pthread_mutex_lock(&ctx->lock);
    WatchMap **cur = &ctx->watches;
    while (*cur) {
        if ((*cur)->wd == wd) {
            WatchMap *tmp = *cur;
            *cur = (*cur)->next;
            free(tmp);
            break;
        }
        cur = &(*cur)->next;
    }
    pthread_mutex_unlock(&ctx->lock);
}

static const char *get_watch_path(InotifyCtx *ctx, int wd) {
    pthread_mutex_lock(&ctx->lock);
    WatchMap *cur = ctx->watches;
    while (cur) {
        if (cur->wd == wd) {
            pthread_mutex_unlock(&ctx->lock);
            return cur->path;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->lock);
    return NULL;
}

static void setup_watches_recursive(InotifyCtx *ctx, const char *path) {
    add_inotify_watch(ctx, path);

    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (ent->d_type == DT_DIR) {
            char sub[MAX_PATH];
            snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
            setup_watches_recursive(ctx, sub);
        }
    }
    closedir(d);
}

static void cleanup_watches(InotifyCtx *ctx) {
    pthread_mutex_lock(&ctx->lock);
    WatchMap *cur = ctx->watches;
    while (cur) {
        WatchMap *next = cur->next;
        free(cur);
        cur = next;
    }
    ctx->watches = NULL;
    pthread_mutex_unlock(&ctx->lock);
}

static void handle_inotify_event(InotifyCtx *ctx, const struct inotify_event *event) {
    if (event->mask & IN_IGNORED) {
        remove_inotify_watch(ctx, event->wd);
        return;
    }

    if (event->mask & IN_Q_OVERFLOW) {
        LOGW("[inotify] queue overflow");
        return;
    }

    if (!event->len || event->name[0] == '.') return;

    const char *base_path = get_watch_path(ctx, event->wd);
    if (!base_path) return;

    const RedirectRule *rule = ctx->rule;
    char src_path[MAX_PATH], dst_path[MAX_PATH];

    snprintf(src_path, sizeof(src_path), "%s/%s", base_path, event->name);

    const char *org_dest = find_org_dest(event->name);
    if (org_dest) {
        snprintf(dst_path, sizeof(dst_path), "%s/%s", org_dest, event->name);
    } else {
        snprintf(dst_path, sizeof(dst_path), "%s/%s", rule->target, event->name);
    }

    if (event->mask & (IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE)) {
        LOGI("[inotify] %s -> %s", src_path, dst_path);

        if (event->mask & IN_CREATE) {
            usleep(100000);
        }

        struct stat st;
        if (stat(src_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                copy_recursive(src_path, dst_path);
                if ((event->mask & IN_ISDIR) && (event->mask & IN_CREATE)) {
                    setup_watches_recursive(ctx, src_path);
                }
            } else {
                copy_recursive(src_path, dst_path);
            }
        }
    }

    if (event->mask & IN_DELETE) {
        LOGI("[inotify] deleted: %s", src_path);
    }
}

static void *inotify_thread(void *arg) {
    InotifyCtx *ctx = (InotifyCtx *)arg;
    const RedirectRule *rule = ctx->rule;

    LOGI("[inotify] thread started for: %s", rule->source);

    struct stat st;
    if (stat(rule->source, &st) < 0) {
        LOGW("[inotify] source not found: %s, skipping", rule->source);
        return NULL;
    }

    ctx->inotify_fd = inotify_init1(IN_NONBLOCK);
    if (ctx->inotify_fd < 0) {
        LOGE("[inotify] init failed: %s", strerror(errno));
        return NULL;
    }

    if (S_ISDIR(st.st_mode)) {
        setup_watches_recursive(ctx, rule->source);
    } else {
        add_inotify_watch(ctx, rule->source);
    }

    if (!ctx->watches) {
        LOGE("[inotify] no watches established for %s", rule->source);
        close(ctx->inotify_fd);
        return NULL;
    }

    char event_buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (ctx->running) {
        ssize_t len = read(ctx->inotify_fd, event_buf, sizeof(event_buf));

        if (len > 0) {
            const struct inotify_event *event;
            for (char *ptr = event_buf; ptr < event_buf + len;
                 ptr += sizeof(struct inotify_event) + event->len) {
                event = (const struct inotify_event *)ptr;
                handle_inotify_event(ctx, event);
            }
        } else if (len < 0 && errno != EAGAIN) {
            LOGE("[inotify] read error: %s", strerror(errno));
            break;
        }

        usleep(200000);
    }

    close(ctx->inotify_fd);
    cleanup_watches(ctx);
    LOGI("[inotify] thread stopped for: %s", rule->source);
    return NULL;
}

static void start_inotify_threads(void) {
    LOGI("=== Inotify mode: starting %d watchers ===", g_state.rule_count);

    for (int i = 0; i < g_state.rule_count; i++) {
        g_inotify_ctx[i].rule = &g_state.rules[i];
        g_inotify_ctx[i].running = 1;
        g_inotify_ctx[i].watches = NULL;
        pthread_mutex_init(&g_inotify_ctx[i].lock, NULL);

        if (g_state.rules[i].enabled) {
            pthread_create(&g_inotify_ctx[i].thread, NULL,
                          inotify_thread, &g_inotify_ctx[i]);
        }
    }

    FILE *f = fopen(ACTIVATED_CONF, "w");
    if (f) { fprintf(f, "1"); fclose(f); }
}

static void stop_inotify_threads(void) {
    LOGI("Stopping inotify threads...");
    for (int i = 0; i < g_state.rule_count; i++) {
        if (g_inotify_ctx[i].running) {
            g_inotify_ctx[i].running = 0;
            pthread_join(g_inotify_ctx[i].thread, NULL);
            pthread_mutex_destroy(&g_inotify_ctx[i].lock);
        }
    }
}

/* ============ 文件整理器（Organizer） ============ */

static time_t g_last_org_scan = 0;

static int scan_and_organize(void) {
    if (g_state.org_rule_count == 0) return 0;

    time_t now = time(NULL);
    if (now - g_last_org_scan < g_state.org_interval) return 0;
    g_last_org_scan = now;

    LOGI("[organizer] scanning downloads...");
    int moved = 0;

    for (int i = 0; i < g_state.rule_count; i++) {
        RedirectRule *r = &g_state.rules[i];
        if (!r->enabled) continue;

        DIR *d = opendir(r->source);
        if (!d) continue;

        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (ent->d_type != DT_REG) continue;

            const char *dest = find_org_dest(ent->d_name);
            if (!dest) continue;

            char src[MAX_PATH], dst[MAX_PATH];
            snprintf(src, sizeof(src), "%s/%s", r->source, ent->d_name);
            snprintf(dst, sizeof(dst), "%s/%s", dest, ent->d_name);

            ensure_dir(dest);

            if (rename(src, dst) == 0) {
                LOGI("[organizer] moved: %s -> %s", src, dst);
                moved++;
            } else {
                if (copy_recursive(src, dst) == 0) {
                    unlink(src);
                    LOGI("[organizer] copied: %s -> %s", src, dst);
                    moved++;
                }
            }
        }
        closedir(d);
    }

    if (moved > 0) LOGI("[organizer] moved %d files", moved);
    return moved;
}

/* ============ 配置热重载检查 ============ */

static int check_config_changed(void) {
    int changed = 0;
    time_t t;

    t = get_mtime(REDIRECTS_CONF);
    if (t != g_state.redirects_mtime) {
        LOGI("redirects.conf changed, reloading...");
        load_redirects(REDIRECTS_CONF);
        changed = 1;
    }

    t = get_mtime(MODE_CONF);
    if (t != g_state.mode_mtime) {
        LOGI("mode.conf changed, reloading...");
        int old_mode = g_state.run_mode;
        load_mode(MODE_CONF);
        if (old_mode != g_state.run_mode) changed = 2;
    }

    t = get_mtime(ORG_CONF);
    if (t != g_state.org_mtime) {
        LOGI("organizer.conf changed, reloading...");
        load_organizer(ORG_CONF);
        changed = 1;
    }

    return changed;
}

/* ============ 主函数 ============ */

int main(int argc, char *argv[]) {
    const char *mod_dir = MOD_DIR;
    if (argc > 1) mod_dir = argv[1];

    memset(&g_state, 0, sizeof(g_state));
    g_state.running = 1;
    g_state.daemon_interval = 5;

    log_init();
    LOGI("========================================");
    LOGI("易の重定向 Daemon starting (optimized)");
    LOGI("Module dir: %s", mod_dir);
    LOGI("PID: %d", getpid());
    LOGI("========================================");

    FILE *pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    ensure_dir(CONF_DIR);

    load_all_config();

    if (g_state.run_mode == MODE_MOUNT) {
        run_mount_mode();
        LOGI("Mount mode: daemon running (waiting for config changes or signal)...");

        while (g_state.running) {
            int changed = check_config_changed();
            if (changed == 2) {
                LOGI("Mode changed to inotify, switching...");
                break;
            } else if (changed == 1) {
                run_mount_mode();
            }
            scan_and_organize();
            sleep(g_state.daemon_interval);
        }

        if (g_state.running && g_state.run_mode == MODE_INOTIFY) {
            start_inotify_threads();
            while (g_state.running) {
                int changed = check_config_changed();
                if (changed == 2) {
                    stop_inotify_threads();
                    run_mount_mode();
                    break;
                }
                scan_and_organize();
                sleep(g_state.daemon_interval);
            }
        }
    } else {
        start_inotify_threads();
        LOGI("Inotify mode: daemon running...");

        while (g_state.running) {
            int changed = check_config_changed();
            if (changed == 2) {
                stop_inotify_threads();
                run_mount_mode();
                break;
            }
            scan_and_organize();
            sleep(g_state.daemon_interval);
        }
    }

    stop_inotify_threads();

    FILE *f = fopen(ACTIVATED_CONF, "w");
    if (f) { fprintf(f, "0"); fclose(f); }

    unlink(PID_FILE);

    LOGI("========================================");
    LOGI("Daemon stopped");
    LOGI("========================================");
    log_close();

    return 0;
}
