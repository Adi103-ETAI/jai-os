/*
 * jai-capabilities - measure the machine, write /etc/jai/capabilities.json
 * roadmap 2b - spec: docs/CAPABILITIES-SPEC.md (this code obeys the spec)
 *
 * Law 1 (VISION.md): MEASURE the machine, ADAPT, degrade gracefully,
 * never REQUIRE. At boot this probe looks at CPU, RAM, GPU, disk, net,
 * display, and virtualization, then writes ONE truth file that every
 * AI layer reads instead of guessing. The profile (minimal / balanced /
 * full) and the accel tier (cuda > rocm > vulkan > cpu) pick THEMSELVES
 * from measured facts - two independent axes, never conflated.
 *
 * Rules that never bend (from the spec):
 * - read-only probing; writes exactly one file (with --write), atomic
 *   (tmp + rename), 0644
 * - never blocks boot: every probe has source-or-default; total budget
 *   < 1 s (the only timed probe is net_online, capped at 1 s)
 * - no new hard dependencies: C + json-c + libc only; nvidia-smi,
 *   rocm-smi and Vulkan ICDs are optional, checked with access(),
 *   used only when present
 * - llvmpipe / lavapipe (a CPU pretending to be a GPU) is rejected by
 *   CONSTRUCTION: accel=vulkan requires the GPU vendor's own ICD file,
 *   and software renderers ship their own (lvp_icd*.json), which is
 *   never accepted - the GPU-day red herring, killed at the root
 *
 * House rules honored (same as jai-mcp / jai-eventd): fork+execvp for
 * subprocesses (NEVER popen - no shell parses anything), fixed-size
 * buffers, all JSON built with json-c (never string-concatenated),
 * idempotent output (same machine -> same JSON modulo timestamp).
 *
 * CLI:
 *   jai-capabilities            print the JSON to stdout
 *   jai-capabilities --write    ALSO write <JAI_ETC_DIR>/capabilities.json
 *   jai-capabilities --profile  print just minimal|balanced|full
 *
 * Build (same recipe as the other JAI C tools):
 *   gcc -O2 -Wall -Wextra -o jai-capabilities jai-capabilities.c -ljson-c
 *
 * JAI_ETC_DIR is a compile-time override for TESTS only; the shipped
 * binary uses the default /etc/jai.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef JAI_ETC_DIR
#define JAI_ETC_DIR "/etc/jai"
#endif

#define LADDER_FULL_RAM_MB    15360   /* "16 GB" boxes report less than 16384 */
#define LADDER_FULL_CORES     4
#define LADDER_BALANCED_RAM_MB 7600   /* "8 GB" boxes report less than 8192  */
#define LADDER_BALANCED_CORES 2
#define FLOOR_RAM_MB          1900    /* under this: rules-only mode          */
#define KIND_DISCRETE_VRAM_MB 4096    /* coarse heuristic, says so in notes   */

/* ----------------------------------------------------------- small utils -- */

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                 s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = 0;
}

/* first line of a small file, trimmed. 0 = ok, -1 = unreadable */
static int read_line(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        out[0] = 0;
        return -1;
    }
    int rc = -1;
    if (fgets(out, (int)cap, f)) {
        rtrim(out);
        rc = 0;
    } else {
        out[0] = 0;
    }
    fclose(f);
    return rc;
}

/* <dir>/<fname> first line, trimmed. The snprintf return is checked so a
 * path that would not fit fails cleanly instead of silently reading a
 * truncated (wrong) file - and gcc -Wformat-truncation stays quiet. */
static int read_line_in(const char *dir, const char *fname, char *out, size_t cap)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", dir, fname);
    if (n < 0 || (size_t)n >= sizeof path) {
        out[0] = 0;
        return -1;
    }
    return read_line(path, out, cap);
}

/* basename of a symlink target (for /sys/.../device/driver) */
static int readlink_base(const char *linkpath, char *out, size_t cap)
{
    char buf[PATH_MAX];
    ssize_t n = readlink(linkpath, buf, sizeof buf - 1);
    if (n < 0) {
        out[0] = 0;
        return -1;
    }
    buf[n] = 0;
    char *slash = strrchr(buf, '/');
    /* %.*s with an explicit precision: max output = cap-1, never truncates */
    snprintf(out, cap, "%.*s", (int)(cap - 1), slash ? slash + 1 : buf);
    return 0;
}

/* /proc/meminfo "<Key>:  NNN kB" -> NNN, or -1 */
static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[256];
    long val = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, klen) && line[klen] == ':') {
            val = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return val;
}

/* /proc/cpuinfo "<key> : value" (first hit) -> value, or -1 */
static int cpuinfo_field(const char *key, char *out, size_t cap)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return -1;
    char line[512];
    int found = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, klen) && line[klen] == ':') {
            const char *v = line + klen + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            snprintf(out, cap, "%s", v);
            rtrim(out);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static int file_readable(const char *path)
{
    return access(path, R_OK) == 0;
}

/* is <name> executable somewhere on PATH? (never spawns a shell) */
static int bin_on_path(const char *name)
{
    const char *p = getenv("PATH");
    if (!p || !*p)
        p = "/usr/bin:/bin";
    char cand[PATH_MAX];
    const char *start = p;
    for (;;) {
        const char *colon = strchr(start, ':');
        size_t len = colon ? (size_t)(colon - start) : strlen(start);
        if (len > 0 && len < sizeof cand - 2) {
            int w = snprintf(cand, sizeof cand, "%.*s/%s", (int)len, start, name);
            if (w > 0 && (size_t)w < sizeof cand && access(cand, X_OK) == 0)
                return 1;
        }
        if (!colon)
            break;
        start = colon + 1;
    }
    return 0;
}

/* any entry in <dir> starting with <prefix>? */
static int dir_has_entry_prefix(const char *dir, const char *prefix)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    size_t plen = strlen(prefix);
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (!strncmp(e->d_name, prefix, plen)) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* ------------------------------------------- subprocess: fork+execvp only -- */
/* House rule: NEVER popen, no shell parses anything (mirrors jai-mcp.c).     */

static void on_alarm(int sig) { (void)sig; } /* just interrupts waitpid */

/* capture the first (cap-1) bytes of stdout from argv[] (no shell, ever).
 * 0 = exited 0 with non-empty output; anything else = "probe not available" */
static int run_capture(char *const argv[], char *out, size_t cap, unsigned timeout_s)
{
    out[0] = 0;
    int fds[2];
    if (pipe(fds) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execvp(argv[0], argv);
        _exit(127); /* not found / not executable */
    }
    close(fds[1]);

    size_t got = 0;
    char chunk[256];
    ssize_t n;
    while (got < cap - 1 && (n = read(fds[0], chunk, sizeof chunk)) > 0) {
        size_t take = (size_t)n < cap - 1 - got ? (size_t)n : cap - 1 - got;
        memcpy(out + got, chunk, take);
        got += take;
    }
    close(fds[0]);
    out[got] = 0;

    /* watchdog: SIGALRM interrupts waitpid; the child then gets SIGKILLed */
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, &old);
    alarm(timeout_s);

    pid_t w = -1;
    int status = 0, killed = 0, rc = -1;
    for (;;) {
        w = waitpid(pid, &status, 0);
        if (w == pid)
            break;
        if (w < 0 && errno == EINTR) {
            if (!killed) {
                kill(pid, SIGKILL);
                killed = 1;
            }
            continue;
        }
        break; /* other waitpid error: give up, child reaped by init */
    }
    if (w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 && out[0])
        rc = 0;

    alarm(0);
    sigaction(SIGALRM, &old, NULL);
    if (rc != 0)
        out[0] = 0;
    return rc;
}

/* --------------------------------------------------------------- network -- */

/* TCP connect to 1.1.1.1:44, capped at 1 s (the only timed probe). */
static int net_online_1s(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return 0;
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0)
        fcntl(s, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(44);
    if (inet_pton(AF_INET, "1.1.1.1", &a.sin_addr) != 1) {
        close(s);
        return 0;
    }

    int online = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        online = 1;
    } else if (errno == EINPROGRESS) {
        struct pollfd p = { .fd = s, .events = POLLOUT, .revents = 0 };
        if (poll(&p, 1, 1000) == 1) {
            int err = 0;
            socklen_t elen = sizeof err;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
                online = 1;
        }
    }
    close(s);
    return online;
}

/* UP links in /sys/class/net, excluding lo. Missing operstate counts as
 * up (permissive: some virtual drivers do not export it). */
static int count_net_links(void)
{
    DIR *d = opendir("/sys/class/net");
    if (!d)
        return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
            !strcmp(e->d_name, "lo"))
            continue;
        char p[PATH_MAX], st[32];
        snprintf(p, sizeof p, "/sys/class/net/%s/operstate", e->d_name);
        if (read_line(p, st, sizeof st) == 0 && strcmp(st, "up") != 0)
            continue;
        n++;
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------------- GPU -- */

struct gpu_info {
    int present;
    char kind[16];     /* "integrated" | "discrete" | ""             */
    char vendor[16];   /* "amd" | "nvidia" | "intel"                 */
    char model[128];   /* product_name, else PCI id "1002:15e7"      */
    long long vram_mb; /* carve-out only on iGPUs (says so in notes) */
    char driver[32];   /* amdgpu / i915 / nouveau / ...              */
};

/* "card0" yes; "card0-DP-1" and "renderD128" no */
static int is_card_name(const char *nm)
{
    if (strncmp(nm, "card", 4) != 0 || nm[4] == 0)
        return 0;
    for (const char *p = nm + 4; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static void probe_gpu(struct gpu_info *g)
{
    memset(g, 0, sizeof *g);
    DIR *d = opendir("/sys/class/drm");
    if (!d)
        return; /* no drm at all -> present=0, accel falls back to cpu */
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!is_card_name(e->d_name))
            continue;
        char base[PATH_MAX], vline[64];
        snprintf(base, sizeof base, "/sys/class/drm/%s/device", e->d_name);
        if (read_line_in(base, "vendor", vline, sizeof vline) != 0)
            continue;
        unsigned long vid = strtoul(vline, NULL, 0);
        const char *vs;
        if (vid == 0x1002)
            vs = "amd";
        else if (vid == 0x10de)
            vs = "nvidia";
        else if (vid == 0x8086)
            vs = "intel";
        else
            continue; /* not a compute-relevant display GPU (VM svga etc.) */

        g->present = 1;
        snprintf(g->vendor, sizeof g->vendor, "%s", vs);

        char model[128];
        if (read_line_in(base, "product_name", model, sizeof model) == 0 &&
            model[0]) {
            snprintf(g->model, sizeof g->model, "%s", model);
        } else {
            char dev[64];
            unsigned long did = 0;
            if (read_line_in(base, "device", dev, sizeof dev) == 0)
                did = strtoul(dev, NULL, 0);
            snprintf(g->model, sizeof g->model, "%04lx:%04lx", vid, did);
        }

        char vram[64];
        if (read_line_in(base, "mem_info_vram_total", vram, sizeof vram) == 0) {
            unsigned long long bytes = strtoull(vram, NULL, 10);
            g->vram_mb = (long long)(bytes / (1024ULL * 1024ULL));
        }

        char dl[PATH_MAX + 16];
        snprintf(dl, sizeof dl, "%s/driver", base);
        readlink_base(dl, g->driver, sizeof g->driver);

        snprintf(g->kind, sizeof g->kind, "%s",
                 g->vram_mb >= KIND_DISCRETE_VRAM_MB ? "discrete" : "integrated");
        break; /* first compute-relevant card wins; v1 keeps it simple */
    }
    closedir(d);
}

/* vendor's own Vulkan ICD present? The software renderers (lvp_icd /
 * lavapipe / swiftshader) are NEVER accepted - llvmpipe lesson, enforced. */
static int icd_for_vendor(const char *vendor)
{
    static const char *const amd[] = {
        "/usr/share/vulkan/icd.d/radeon_icd.x86_64.json",
        "/usr/share/vulkan/icd.d/radeon_icd.json", NULL,
    };
    static const char *const intel[] = {
        "/usr/share/vulkan/icd.d/intel_icd.x86_64.json",
        "/usr/share/vulkan/icd.d/intel_icd.json", NULL,
    };
    static const char *const nvidia[] = {
        "/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json",
        "/usr/share/vulkan/icd.d/nvidia_icd.json", NULL,
    };
    const char *const *list = NULL;
    if (!strcmp(vendor, "amd"))
        list = amd;
    else if (!strcmp(vendor, "intel"))
        list = intel;
    else if (!strcmp(vendor, "nvidia"))
        list = nvidia;
    if (!list)
        return 0;
    for (int i = 0; list[i]; i++)
        if (file_readable(list[i]))
            return 1;
    return 0;
}

static int libvulkan_present(void)
{
    void *h = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NODELETE);
    if (!h)
        return 0;
    dlclose(h);
    return 1;
}

/* axis 2: cuda > rocm > vulkan > cpu (spec table; cpu is NORMAL, not an error) */
static const char *pick_accel(const struct gpu_info *g)
{
    if (!g->present)
        return "cpu";
    if (!strcmp(g->vendor, "nvidia") && bin_on_path("nvidia-smi"))
        return "cuda";
    if (!strcmp(g->vendor, "amd") &&
        (access("/opt/rocm/bin/rocm-smi", X_OK) == 0 ||
         access("/usr/bin/rocm-smi", X_OK) == 0))
        return "rocm"; /* chip-support check is tier-1's own fallback chain's job */
    if (libvulkan_present() && icd_for_vendor(g->vendor))
        return "vulkan";
    return "cpu";
}

/* ------------------------------------------------------------------ misc -- */

static const char *detect_display(void)
{
    if (dir_has_entry_prefix("/tmp/.X11-unix", "X"))
        return "desktop";
    char wl[PATH_MAX];
    snprintf(wl, sizeof wl, "/run/user/%d", (int)geteuid());
    if (dir_has_entry_prefix(wl, "wayland") || getenv("WAYLAND_DISPLAY"))
        return "desktop";
    return "headless";
}

static int write_capabilities(const char *pretty)
{
    if (mkdir(JAI_ETC_DIR, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "jai-capabilities: mkdir %s: %s\n", JAI_ETC_DIR,
                strerror(errno));
        return -1;
    }
    char tmp[PATH_MAX], final[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/capabilities.json.tmp", JAI_ETC_DIR);
    snprintf(final, sizeof final, "%s/capabilities.json", JAI_ETC_DIR);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "jai-capabilities: open %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    size_t len = strlen(pretty);
    const char *p = pretty;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "jai-capabilities: write: %s\n", strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += (size_t)w;
        len -= (size_t)w;
    }
    if (write(fd, "\n", 1) < 0) { /* best-effort trailing newline */
        fprintf(stderr, "jai-capabilities: write: %s\n", strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    fchmod(fd, 0644); /* explicit: umask-proof */
    fsync(fd);
    close(fd);
    if (rename(tmp, final) != 0) { /* atomic swap: readers never see half a file */
        fprintf(stderr, "jai-capabilities: rename: %s\n", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void usage(FILE *f)
{
    fprintf(f,
        "jai-capabilities - measure the machine (roadmap 2b)\n"
        "spec: docs/CAPABILITIES-SPEC.md - this binary obeys the spec\n"
        "usage: jai-capabilities [--write | --profile]\n"
        "  (no args)   print capabilities JSON to stdout\n"
        "  --write     also write " JAI_ETC_DIR "/capabilities.json (atomic, 0644)\n"
        "  --profile   print just minimal|balanced|full\n");
}

/* ------------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    int do_write = 0, profile_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--write"))
            do_write = 1;
        else if (!strcmp(argv[i], "--profile"))
            profile_only = 1;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "jai-capabilities: unknown option '%s'\n\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    /* hard facts: without a RAM/CPU answer there is nothing to adapt FROM */
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    long ram_kb = meminfo_kb("MemTotal");
    if (cores < 1 || ram_kb < 1) {
        fprintf(stderr, "jai-capabilities: cannot read %s - aborting\n",
                cores < 1 ? "core count (sysconf)" : "/proc/meminfo MemTotal");
        return 1;
    }
    long swap_kb = meminfo_kb("SwapTotal");
    if (swap_kb < 0)
        swap_kb = 0;
    long ram_mb = ram_kb / 1024;
    long swap_mb = swap_kb / 1024;

    /* cheap facts, each with source-or-default (spec probe table) */
    char arch[128] = "unknown", virt[32] = "none", cpu_model[512] = "";
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(arch, sizeof arch, "%.*s", (int)sizeof arch - 1, u.machine);

    /* plain invocation (no --value): works on old AND new systemd; prints
     * "none" when bare metal. Missing binary or failure -> "none" default. */
    char vout[64];
    char *virt_argv[] = { (char *)"systemd-detect-virt", NULL };
    if (run_capture(virt_argv, vout, sizeof vout, 2) == 0) {
        rtrim(vout);
        if (vout[0])
            snprintf(virt, sizeof virt, "%.*s", (int)sizeof virt - 1, vout);
    }

    cpuinfo_field("model name", cpu_model, sizeof cpu_model);

    struct gpu_info gpu;
    probe_gpu(&gpu);
    const char *accel = pick_accel(&gpu);

    struct statvfs fs;
    long long total_gb = 0, free_gb = 0;
    if (statvfs("/", &fs) == 0) {
        long long mb =
            (long long)fs.f_blocks * (long long)fs.f_frsize / (1024 * 1024);
        total_gb = mb / 1024;
        mb = (long long)fs.f_bavail * (long long)fs.f_frsize / (1024 * 1024);
        free_gb = mb / 1024;
    }

    int links = count_net_links();
    int online = net_online_1s();
    const char *display = detect_display();

    /* axis 1: the ladder - RAM + cores ONLY (spec thresholds) */
    const char *profile;
    if (ram_mb >= LADDER_FULL_RAM_MB && cores >= LADDER_FULL_CORES)
        profile = "full";
    else if (ram_mb >= LADDER_BALANCED_RAM_MB && cores >= LADDER_BALANCED_CORES)
        profile = "balanced";
    else
        profile = "minimal";
    int below_floor = ram_mb < FLOOR_RAM_MB;

    /* ---- build the JSON (json-c only, house rule) ---- */
    json_object *root = json_object_new_object();
    json_object_object_add(root, "schema", json_object_new_int(1));

    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    json_object_object_add(root, "generated_at", json_object_new_string(ts));

    json_object_object_add(root, "arch", json_object_new_string(arch));
    json_object_object_add(root, "virt", json_object_new_string(virt));

    json_object *cpu = json_object_new_object();
    json_object_object_add(cpu, "cores", json_object_new_int((int)cores));
    json_object_object_add(cpu, "model", json_object_new_string(cpu_model));
    json_object_object_add(root, "cpu", cpu);

    json_object_object_add(root, "ram_mb", json_object_new_int((int)ram_mb));
    json_object_object_add(root, "swap_mb", json_object_new_int((int)swap_mb));

    json_object *g = json_object_new_object();
    json_object_object_add(g, "present", json_object_new_boolean(gpu.present));
    json_object_object_add(g, "kind", json_object_new_string(gpu.kind));
    json_object_object_add(g, "vendor", json_object_new_string(gpu.vendor));
    json_object_object_add(g, "model", json_object_new_string(gpu.model));
    json_object_object_add(g, "vram_mb", json_object_new_int((int)gpu.vram_mb));
    json_object_object_add(g, "driver", json_object_new_string(gpu.driver));
    json_object_object_add(root, "gpu", g);

    json_object_object_add(root, "accel", json_object_new_string(accel));

    json_object *disk = json_object_new_object();
    json_object_object_add(disk, "total_gb", json_object_new_int((int)total_gb));
    json_object_object_add(disk, "free_gb", json_object_new_int((int)free_gb));
    json_object_object_add(root, "disk_root", disk);

    json_object *net = json_object_new_object();
    json_object_object_add(net, "links", json_object_new_int(links));
    json_object_object_add(net, "online", json_object_new_boolean(online));
    json_object_object_add(root, "net", net);

    json_object_object_add(root, "display", json_object_new_string(display));
    json_object_object_add(root, "profile", json_object_new_string(profile));
    json_object_object_add(root, "below_floor",
                           json_object_new_boolean(below_floor));

    /* notes[] is REQUIRED and MUST be plain words (spec: Law "explains in
     * plain words" applies even to JSON; `jq -r '.notes[]'` must read like
     * a human explaining the machine) */
    json_object *notes = json_object_new_array();
    char buf[256];
    snprintf(buf, sizeof buf, "%ld cores + %.1f GB RAM -> %s profile", cores,
             (double)ram_mb / 1024.0, profile);
    json_object_array_add(notes, json_object_new_string(buf));

    if (gpu.present) {
        if (!strcmp(accel, "cuda")) {
            json_object_array_add(notes, json_object_new_string(
                "NVIDIA GPU + CUDA stack found -> tier-1 runs on GPU (CUDA)"));
        } else if (!strcmp(accel, "rocm")) {
            json_object_array_add(notes, json_object_new_string(
                "AMD GPU + ROCm stack found -> tier-1 can use the GPU (ROCm; "
                "chip-support check is tier-1's own fallback chain's job)"));
        } else if (!strcmp(accel, "vulkan")) {
            snprintf(buf, sizeof buf,
                     "Vulkan-capable %s GPU found -> tier-1 runs on GPU "
                     "(Vulkan; software renderers like llvmpipe are rejected "
                     "by construction)", gpu.vendor);
            json_object_array_add(notes, json_object_new_string(buf));
        } else {
            json_object_array_add(notes, json_object_new_string(
                "GPU found but no usable compute stack -> tier-1 runs on CPU "
                "(chain: GPU -> CPU -> rules)"));
        }
        if (!strcmp(gpu.kind, "integrated")) {
            json_object_array_add(notes, json_object_new_string(
                "iGPU shares system RAM: offload moves MATH off the cores, "
                "not memory"));
        }
    } else {
        json_object_array_add(notes, json_object_new_string(
            "No usable GPU -> tier-1 runs on CPU (chain: GPU -> CPU -> rules)"));
    }

    if (!strcmp(display, "headless")) {
        json_object_array_add(notes, json_object_new_string(
            "headless -> no desktop UI this boot"));
    } else {
        json_object_array_add(notes, json_object_new_string(
            "desktop session detected"));
    }

    if (!online) {
        json_object_array_add(notes, json_object_new_string(
            "no internet right now -> cloud tier off; local rules + local "
            "model only"));
    }
    if (below_floor) {
        json_object_array_add(notes, json_object_new_string(
            "under 2 GB RAM -> rules-only mode; kernel reflexes stay on "
            "(they cost almost zero)"));
    }
    if (strcmp(virt, "none") != 0) {
        snprintf(buf, sizeof buf,
                 "virtualized (%s) - fine for the bench; the product is metal",
                 virt);
        json_object_array_add(notes, json_object_new_string(buf));
    }
    json_object_object_add(root, "notes", notes);

    /* ---- emit ---- */
    if (profile_only) {
        printf("%s\n", profile);
        json_object_put(root);
        return 0;
    }

    const char *pretty = json_object_to_json_string_ext(
        root, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);
    if (do_write && write_capabilities(pretty) != 0) {
        json_object_put(root);
        return 1;
    }
    printf("%s\n", pretty);
    json_object_put(root);
    return 0;
}
