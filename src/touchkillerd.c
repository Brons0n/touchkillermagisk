/*
 * touchkillerd - hold EVIOCGRAB on all touchscreen/stylus evdev nodes so the
 * physical digitizer is dead while remote automation (scrcpy / Laixi) drives
 * the device.
 *
 * Grabbing an evdev node with EVIOCGRAB gives this process exclusive access:
 * the kernel stops delivering those events to every other reader, including
 * /dev/input readers inside system_server (InputReader). Releasing the grab
 * (or closing the fd) restores delivery immediately, with no driver reset and
 * no partially-delivered gesture, because the grab only affects dispatch.
 *
 * Framework-level injection (InputManager.injectInputEvent, which is what the
 * scrcpy server uses for control) does not travel through the evdev node, so
 * remote control keeps working while the physical panel is grabbed.
 *
 * Build: see build.sh (NDK clang, -static, arm64-v8a + armeabi-v7a).
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>

#ifndef INPUT_PROP_DIRECT
#define INPUT_PROP_DIRECT 0x01
#endif
#ifndef INPUT_PROP_POINTER
#define INPUT_PROP_POINTER 0x00
#endif
#ifndef BUS_VIRTUAL
#define BUS_VIRTUAL 0x06
#endif

/* ------------------------------------------------------------------ paths */

#define FLAG_DIR      "/data/local/tmp"
#define FLAG_ENABLE   FLAG_DIR "/touchkill_enable"
#define FLAG_AUTO     FLAG_DIR "/touchkill_auto"
#define FILE_STATUS   FLAG_DIR "/touchkill_status"

#define STATE_DIR     "/data/adb/touchkiller"
#define LOG_PATH      STATE_DIR "/touchkillerd.log"
#define LOG_PATH_OLD  STATE_DIR "/touchkillerd.log.1"
#define PID_PATH      STATE_DIR "/touchkillerd.pid"
#define CONF_PATH     STATE_DIR "/config"

#define LOG_MAX_BYTES (512 * 1024)

/* ---------------------------------------------------------------- tunables */

static int   opt_poll_ms       = 1000; /* fallback poll / auto-detect cadence */
static int   opt_auto_default  = 0;    /* value of auto mode when flag absent */
static int   opt_rescan_ms     = 5000; /* hotplug rescan while grabbed        */
static int   opt_grab_stylus   = 1;    /* also grab pen/digitizer nodes       */
static int   opt_allow_virtual = 0;    /* grab BUS_VIRTUAL nodes too          */
static int   opt_verbose       = 0;

/* -------------------------------------------------------------- bit helpers */

#define NBITS(x) ((((x) - 1) / (sizeof(unsigned long) * 8)) + 1)
#define TEST_BIT(bit, array) \
    ((array[(bit) / (sizeof(unsigned long) * 8)] >> ((bit) % (sizeof(unsigned long) * 8))) & 1)

/* ---------------------------------------------------------------- logging */

static volatile sig_atomic_t g_stop = 0;

static void rotate_log_if_needed(void)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        unlink(LOG_PATH_OLD);
        rename(LOG_PATH, LOG_PATH_OLD);
    }
}

static void logmsg(const char *fmt, ...)
{
    char stamp[64];
    time_t now = time(NULL);
    struct tm tm;
    FILE *f;
    va_list ap;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    rotate_log_if_needed();
    f = fopen(LOG_PATH, "ae");
    if (!f)
        f = stderr;

    fprintf(f, "%s ", stamp);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);

    if (f != stderr) {
        fclose(f);
    } else {
        fflush(f);
    }
}

#define logv(...) do { if (opt_verbose) logmsg(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------ grabbed set */

#define MAX_GRABBED 32

struct grabbed {
    int  fd;
    char path[32];
    char name[80];
};

static struct grabbed g_grabbed[MAX_GRABBED];
static int            g_grabbed_n = 0;

static int already_grabbed(const char *path)
{
    for (int i = 0; i < g_grabbed_n; i++)
        if (strcmp(g_grabbed[i].path, path) == 0)
            return 1;
    return 0;
}

/* ----------------------------------------------------------- classification
 *
 * Capability match only - never a device-name match. Vendor names differ per
 * OEM (sec_touchscreen on Samsung *and* on Pixel 6, touchpanel on OnePlus,
 * goodix_ts / synaptics_dsx / fts_ts elsewhere), so names are logged for
 * debugging and otherwise ignored.
 */

enum match_kind {
    MATCH_NONE = 0,
    MATCH_MT,       /* multitouch screen: ABS_MT_POSITION_X + _Y            */
    MATCH_ST,       /* single-touch screen: BTN_TOUCH + ABS_X/Y, direct     */
    MATCH_PEN       /* stylus digitizer: BTN_TOOL_PEN + ABS_X/Y             */
};

static const char *kind_str(enum match_kind k)
{
    switch (k) {
    case MATCH_MT:  return "multitouch";
    case MATCH_ST:  return "single-touch";
    case MATCH_PEN: return "stylus";
    default:        return "none";
    }
}

/* Names that mean "this node was created by a remote-control tool, grabbing
 * it would kill the very thing we are protecting". */
static int looks_synthetic(const char *name)
{
    static const char *bad[] = { "scrcpy", "uhid", "uinput", "virtual keyboard",
                                 "virtual mouse", "virtual finger", NULL };
    char low[80];
    size_t i;

    for (i = 0; i + 1 < sizeof(low) && name[i]; i++)
        low[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    low[i] = '\0';

    for (i = 0; bad[i]; i++)
        if (strstr(low, bad[i]))
            return 1;
    return 0;
}

static enum match_kind classify_fd(int fd, char *name, size_t name_sz, int *bus_out)
{
    unsigned long ev[NBITS(EV_MAX)];
    unsigned long abs[NBITS(ABS_MAX)];
    unsigned long key[NBITS(KEY_MAX)];
    unsigned long prop[NBITS(INPUT_PROP_MAX)];
    struct input_id id;
    int has_mt, has_xy, has_btn_touch, has_pen, is_direct, has_rel;

    memset(ev, 0, sizeof(ev));
    memset(abs, 0, sizeof(abs));
    memset(key, 0, sizeof(key));
    memset(prop, 0, sizeof(prop));
    memset(&id, 0, sizeof(id));

    name[0] = '\0';
    ioctl(fd, EVIOCGNAME(name_sz), name);
    name[name_sz - 1] = '\0';
    ioctl(fd, EVIOCGID, &id);
    if (bus_out)
        *bus_out = id.bustype;

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0)
        return MATCH_NONE;
    if (!TEST_BIT(EV_ABS, ev))
        return MATCH_NONE;
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
        return MATCH_NONE;
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
    ioctl(fd, EVIOCGPROP(sizeof(prop)), prop);

    has_rel       = TEST_BIT(EV_REL, ev);
    has_mt        = TEST_BIT(ABS_MT_POSITION_X, abs) && TEST_BIT(ABS_MT_POSITION_Y, abs);
    has_xy        = TEST_BIT(ABS_X, abs) && TEST_BIT(ABS_Y, abs);
    has_btn_touch = TEST_BIT(BTN_TOUCH, key);
    has_pen       = TEST_BIT(BTN_TOOL_PEN, key);
    is_direct     = TEST_BIT(INPUT_PROP_DIRECT, prop);

    if (has_mt)
        return MATCH_MT;

    /* Single-touch panels (rare, mostly pre-Android-9 and some tablets).
     * Require BTN_TOUCH + absolute X/Y and no relative axes, so trackpads and
     * mice never qualify. INPUT_PROP_DIRECT is a strong hint but some old
     * drivers omit it, so it is not mandatory. */
    if (has_btn_touch && has_xy && !has_rel)
        return MATCH_ST;

    /* Stylus digitizer on its own node (Galaxy Note S Pen: "sec_e-pen"). */
    if (opt_grab_stylus && has_pen && has_xy)
        return MATCH_PEN;

    (void)is_direct;
    return MATCH_NONE;
}

/* --------------------------------------------------------------- grabbing */

/* Returns number of newly grabbed nodes. */
static int scan_and_grab(void)
{
    DIR *d;
    struct dirent *de;
    int added = 0;

    d = opendir("/dev/input");
    if (!d) {
        logmsg("ERROR: opendir /dev/input: %s", strerror(errno));
        return 0;
    }

    while ((de = readdir(d)) != NULL) {
        char path[32], name[80];
        enum match_kind kind;
        int fd, bus = 0;

        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        if (already_grabbed(path))
            continue;
        if (g_grabbed_n >= MAX_GRABBED) {
            logmsg("WARN: grab table full (%d), skipping %s", MAX_GRABBED, path);
            continue;
        }

        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            logv("open %s failed: %s", path, strerror(errno));
            continue;
        }

        kind = classify_fd(fd, name, sizeof(name), &bus);
        if (kind == MATCH_NONE) {
            close(fd);
            continue;
        }
        if (looks_synthetic(name)) {
            logmsg("skip %s (\"%s\"): synthetic/remote-control node", path, name);
            close(fd);
            continue;
        }
        if (bus == BUS_VIRTUAL && !opt_allow_virtual) {
            logmsg("skip %s (\"%s\"): BUS_VIRTUAL (set allow_virtual=1 to include)",
                   path, name);
            close(fd);
            continue;
        }

        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            logmsg("ERROR: EVIOCGRAB %s (\"%s\") failed: %s%s",
                   path, name, strerror(errno),
                   errno == EBUSY ? " (another process already holds the grab)"
                                  : (errno == EACCES || errno == EPERM)
                                        ? " (check SELinux: dmesg | grep avc)"
                                        : "");
            close(fd);
            continue;
        }

        g_grabbed[g_grabbed_n].fd = fd;
        snprintf(g_grabbed[g_grabbed_n].path, sizeof(g_grabbed[g_grabbed_n].path), "%s", path);
        snprintf(g_grabbed[g_grabbed_n].name, sizeof(g_grabbed[g_grabbed_n].name), "%s", name);
        g_grabbed_n++;
        added++;
        logmsg("GRABBED %s (\"%s\", %s, bus=0x%02x)", path, name, kind_str(kind), bus);
    }

    closedir(d);
    return added;
}

static void release_all(void)
{
    for (int i = 0; i < g_grabbed_n; i++) {
        if (ioctl(g_grabbed[i].fd, EVIOCGRAB, 0) < 0)
            logmsg("WARN: ungrab %s failed: %s", g_grabbed[i].path, strerror(errno));
        close(g_grabbed[i].fd); /* close alone would also drop the grab */
        logmsg("RELEASED %s (\"%s\")", g_grabbed[i].path, g_grabbed[i].name);
    }
    g_grabbed_n = 0;
}

/* Discard queued events so the per-fd kernel buffer never sits full. */
static void drain_grabbed(void)
{
    struct input_event buf[64];
    for (int i = 0; i < g_grabbed_n; i++)
        while (read(g_grabbed[i].fd, buf, sizeof(buf)) > 0)
            ;
}

/* ------------------------------------------------------------- flag files
 *
 * Race-free contract: a flag file is only ever *consumed* as a whole. A
 * truncated-but-not-yet-written file (the window inside `echo 1 > file`)
 * reads back as empty, and an empty/garbage read returns "unchanged" rather
 * than flipping state. Writers that want zero-window updates should use the
 * documented `mv` form; inotify then reports IN_MOVED_TO.
 */

static int read_flag(const char *path, int fallback)
{
    char buf[16];
    int fd, n;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fallback;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1; /* unreadable / mid-write: caller keeps previous value */
    buf[n] = '\0';

    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T' ||
            (c == 'o' && (buf[i + 1] == 'n' || buf[i + 1] == 'N')))
            return 1;
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F')
            return 0;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        return -1;
    }
    return -1;
}

static void write_status(int grabbed, int manual, int autom, int detected)
{
    char tmp[PATH_MAX];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", FILE_STATUS);
    f = fopen(tmp, "we");
    if (!f)
        return;
    fprintf(f,
            "{\"grabbed\":%d,\"nodes\":%d,\"manual\":%d,\"auto\":%d,"
            "\"session_detected\":%d,\"pid\":%d",
            grabbed, g_grabbed_n, manual, autom, detected, (int)getpid());
    if (g_grabbed_n > 0) {
        fprintf(f, ",\"devices\":[");
        for (int i = 0; i < g_grabbed_n; i++)
            fprintf(f, "%s{\"node\":\"%s\",\"name\":\"%s\"}",
                    i ? "," : "", g_grabbed[i].path, g_grabbed[i].name);
        fprintf(f, "]");
    }
    fprintf(f, "}\n");
    fclose(f);
    chmod(tmp, 0644);
    rename(tmp, FILE_STATUS); /* atomic for readers */
}

/* -------------------------------------------------- phase 2: session detect
 *
 * Signature established by observation on the test fleet, not from
 * assumption (see README "How auto-detect works"):
 *
 *   - scrcpy (upstream) pushes scrcpy-server.jar to /data/local/tmp and runs
 *     `app_process ... com.genymobile.scrcpy.Server <ver> ...` as uid shell.
 *   - Laixi (youhu.laixijs) ships /data/local/tmp/laixi.jar, which *is* a
 *     repackaged scrcpy server v2.2, launched with the identical
 *     `com.genymobile.scrcpy.Server` entry point.
 *
 * Both therefore share one signal: a live process whose cmdline contains
 * "com.genymobile.scrcpy.Server". The abstract socket (@scrcpy on v2.x,
 * @scrcpy_<scid> on v3+/4.x) is checked as a secondary signal because it is
 * cheaper to read, but the process check is authoritative: the server exits
 * when the client disconnects, and the socket goes with it.
 */

static const char *SERVER_MARKERS[] = {
    "com.genymobile.scrcpy.Server", /* scrcpy upstream AND Laixi's laixi.jar */
    NULL
};

static int cmdline_matches(const char *pid)
{
    char path[64], buf[4096];
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%s/cmdline", pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    for (int i = 0; i < n; i++)
        if (buf[i] == '\0')
            buf[i] = ' ';
    buf[n] = '\0';

    for (int i = 0; SERVER_MARKERS[i]; i++)
        if (strstr(buf, SERVER_MARKERS[i]))
            return 1;
    return 0;
}

static int session_active(char *who, size_t who_sz)
{
    DIR *d;
    struct dirent *de;
    int found = 0;

    d = opendir("/proc");
    if (d) {
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            if (cmdline_matches(de->d_name)) {
                snprintf(who, who_sz, "scrcpy-server pid %s", de->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
    }
    if (found)
        return 1;

    /* Secondary: abstract socket named @scrcpy / @scrcpy_<scid>. */
    {
        FILE *f = fopen("/proc/net/unix", "re");
        char line[512];
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char *at = strstr(line, "@scrcpy");
                if (at) {
                    char *nl = strpbrk(at, " \t\r\n");
                    if (nl)
                        *nl = '\0';
                    snprintf(who, who_sz, "abstract socket %s", at);
                    found = 1;
                    break;
                }
            }
            fclose(f);
        }
    }
    return found;
}

/* ------------------------------------------------------------------ config */

static void load_config(void)
{
    FILE *f = fopen(CONF_PATH, "re");
    char line[256];

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%63[^= \t] = %d", key, &val) != 2 &&
            sscanf(line, "%63[^=]=%d", key, &val) != 2)
            continue;
        if (!strcmp(key, "poll_ms"))            opt_poll_ms = val > 100 ? val : 100;
        else if (!strcmp(key, "auto_default"))  opt_auto_default = !!val;
        else if (!strcmp(key, "rescan_ms"))     opt_rescan_ms = val > 500 ? val : 500;
        else if (!strcmp(key, "grab_stylus"))   opt_grab_stylus = !!val;
        else if (!strcmp(key, "allow_virtual")) opt_allow_virtual = !!val;
        else if (!strcmp(key, "verbose"))       opt_verbose = !!val;
    }
    fclose(f);
}

/* -------------------------------------------------------------- lifecycle */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int single_instance_lock(void)
{
    char buf[32];
    int fd = open(PID_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        logmsg("ERROR: open %s: %s", PID_PATH, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        logmsg("another touchkillerd already holds %s, exiting", PID_PATH);
        close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) != 0) {
        /* non-fatal */
    }
    snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (write(fd, buf, strlen(buf)) < 0) {
        /* non-fatal */
    }
    return fd; /* held for process lifetime */
}

/* Diagnostic: show every process and socket the auto-detector considers a
 * live remote-control session. */
static int do_detect_report(void)
{
    DIR *d = opendir("/proc");
    struct dirent *de;
    FILE *f;
    char line[512];
    int hits = 0;

    printf("marker: \"%s\"\n", SERVER_MARKERS[0]);
    printf("-- matching processes --\n");
    if (d) {
        while ((de = readdir(d)) != NULL) {
            char path[64], buf[4096];
            int fd, n;
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            if (!cmdline_matches(de->d_name))
                continue;
            snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;
            n = (int)read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0)
                continue;
            for (int i = 0; i < n; i++)
                if (buf[i] == '\0')
                    buf[i] = ' ';
            buf[n] = '\0';
            if (strlen(buf) > 150)
                buf[150] = '\0';
            printf("  pid %-7s %s\n", de->d_name, buf);
            hits++;
        }
        closedir(d);
    }
    if (!hits)
        printf("  (none)\n");

    printf("-- matching abstract sockets --\n");
    f = fopen("/proc/net/unix", "re");
    hits = 0;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *at = strstr(line, "@scrcpy");
            if (at) {
                char *nl = strpbrk(at, " \t\r\n");
                if (nl)
                    *nl = '\0';
                printf("  %s\n", at);
                hits++;
            }
        }
        fclose(f);
    }
    if (!hits)
        printf("  (none)\n");

    {
        char who[128] = "";
        printf("verdict: session_detected=%d %s\n",
               session_active(who, sizeof(who)), who);
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-f] [-v] [-p poll_ms]\n"
            "  -f  run in foreground (default: fork into background)\n"
            "  -v  verbose logging\n"
            "  -p  poll interval in ms (default %d)\n"
            "  -s  print the evdev classification table and exit (no grab)\n"
            "  -d  print what the auto-detector currently sees and exit\n",
            argv0, opt_poll_ms);
}

/* Diagnostic mode: list every evdev node and how it classifies. */
static int do_scan_report(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *de;

    if (!d) {
        fprintf(stderr, "opendir /dev/input: %s\n", strerror(errno));
        return 1;
    }
    printf("%-20s %-34s %-13s %s\n", "NODE", "NAME", "CLASS", "BUS");
    while ((de = readdir(d)) != NULL) {
        char path[32], name[80];
        int fd, bus = 0;
        enum match_kind k;

        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            printf("%-20s %-34s %-13s %s\n", path, "(open failed)", strerror(errno), "-");
            continue;
        }
        k = classify_fd(fd, name, sizeof(name), &bus);
        printf("%-20s %-34s %-13s 0x%02x%s\n", path, name, kind_str(k), bus,
               (k != MATCH_NONE && looks_synthetic(name)) ? "  [skipped: synthetic]" :
               (k != MATCH_NONE && bus == BUS_VIRTUAL && !opt_allow_virtual)
                   ? "  [skipped: virtual]" : "");
        close(fd);
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv)
{
    int foreground = 0, scan_only = 0, detect_only = 0, opt;
    int ino_fd = -1, ep_fd = -1, lock_fd = -1;
    int manual = 0, autom, want = 0, applied = 0;
    int last_detected = -1;
    long since_rescan = 0;
    char who[128] = "";

    mkdir(STATE_DIR, 0700);
    load_config();

    while ((opt = getopt(argc, argv, "fvsdp:h")) != -1) {
        switch (opt) {
        case 'f': foreground = 1; break;
        case 'v': opt_verbose = 1; break;
        case 's': scan_only = 1; break;
        case 'd': detect_only = 1; break;
        case 'p': opt_poll_ms = atoi(optarg) > 100 ? atoi(optarg) : 100; break;
        default:  usage(argv[0]); return 2;
        }
    }

    if (scan_only)
        return do_scan_report();
    if (detect_only)
        return do_detect_report();

    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid > 0)
            return 0;
        setsid();
    }

    lock_fd = single_instance_lock();
    if (lock_fd < 0)
        return 1;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);

    autom = opt_auto_default;
    logmsg("touchkillerd starting (pid %d, poll=%dms, auto_default=%d, stylus=%d)",
           (int)getpid(), opt_poll_ms, opt_auto_default, opt_grab_stylus);

    /* inotify on the flag directory gives sub-millisecond reaction to a
     * toggle; the epoll timeout below is the fallback if inotify is
     * unavailable (and drives the auto-detect poll). */
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd >= 0) {
        if (inotify_add_watch(ino_fd, FLAG_DIR,
                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                              IN_DELETE | IN_MODIFY | IN_ATTRIB) < 0) {
            logmsg("WARN: inotify_add_watch %s: %s (falling back to polling)",
                   FLAG_DIR, strerror(errno));
            close(ino_fd);
            ino_fd = -1;
        }
    } else {
        logmsg("WARN: inotify_init1: %s (falling back to polling)", strerror(errno));
    }

    ep_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ep_fd >= 0 && ino_fd >= 0) {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = ino_fd };
        epoll_ctl(ep_fd, EPOLL_CTL_ADD, ino_fd, &ev);
    }

    /* Initial state, then loop. */
    {
        int v = read_flag(FLAG_ENABLE, 0);
        if (v >= 0) manual = v;
        v = read_flag(FLAG_AUTO, opt_auto_default);
        if (v >= 0) autom = v;
    }

    while (!g_stop) {
        struct epoll_event evs[8];
        int detected = 0, n = 0;
        int v;

        /* --- read desired state ------------------------------------- */
        v = read_flag(FLAG_ENABLE, 0);
        if (v >= 0) manual = v;
        v = read_flag(FLAG_AUTO, opt_auto_default);
        if (v >= 0) autom = v;

        if (autom) {
            who[0] = '\0';
            detected = session_active(who, sizeof(who));
            if (detected != last_detected) {
                logmsg("auto-detect: session %s%s%s",
                       detected ? "STARTED" : "ENDED",
                       detected ? " - " : "", detected ? who : "");
                last_detected = detected;
            }
        } else if (last_detected != -1) {
            last_detected = -1;
        }

        want = manual || (autom && detected);

        /* --- apply --------------------------------------------------- */
        if (want && !applied) {
            int got = scan_and_grab();
            if (got == 0 && g_grabbed_n == 0)
                logmsg("WARN: enable requested but no touchscreen node could be grabbed");
            applied = 1;
            since_rescan = 0;
        } else if (!want && applied) {
            release_all();
            applied = 0;
        } else if (want && applied) {
            drain_grabbed();
            since_rescan += opt_poll_ms;
            if (since_rescan >= opt_rescan_ms) {
                scan_and_grab(); /* pick up nodes that appeared since */
                since_rescan = 0;
            }
        }

        write_status(applied, manual, autom, detected);

        /* --- wait ---------------------------------------------------- */
        if (ep_fd >= 0 && ino_fd >= 0) {
            n = epoll_wait(ep_fd, evs, 8, opt_poll_ms);
            if (n > 0) {
                char buf[4096];
                while (read(ino_fd, buf, sizeof(buf)) > 0)
                    ; /* drain; we re-read the flags unconditionally above */
            }
        } else {
            struct timespec ts = { opt_poll_ms / 1000,
                                   (long)(opt_poll_ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
    }

    logmsg("touchkillerd stopping (signal), releasing grabs");
    release_all();
    write_status(0, manual, autom, 0);
    unlink(PID_PATH);
    return 0;
}
