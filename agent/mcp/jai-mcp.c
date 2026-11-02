/*
 * jai-mcp.c - the hands of JAI (C rewrite of the agent<->OS bridge, Phase 3b)
 *
 * A Model Context Protocol server speaking newline-delimited JSON-RPC 2.0
 * over stdio. opencode spawns it as a child process:
 *
 *   stdin  <- tool requests from the brain (which can only ASK)
 *   stdout -> results back to the brain
 *   stderr -> human diagnostics (NEVER stdout: stdout is protocol)
 *
 * Security posture (the anti-ULTRON principle):
 *   - runs as the unprivileged `jai` user (child of jai-agent / opencode)
 *   - every argument passes a strict charset allowlist or is refused
 *   - subprocesses via fork+execvp - NEVER popen, no shell parses input
 *   - all JSON is built with json-c - never hand-concatenated strings
 *   - fixed-size buffers; inbound messages capped; subprocess output capped
 *   - exactly ONE privileged action exists (restart jai-* units) and it is
 *     locked twice: here in code (prefix + charset) and in the OS
 *     (/etc/sudoers.d/jai-mcp: sudo -n /usr/bin/systemctl restart jai-*)
 *
 * Tools (supersedes jai-system.py - same jai_* names, three additions):
 *   jai_kernel_status   read /dev/jai (drain events, or the status line)
 *   jai_kernel_event    write <=254 bytes to /dev/jai + read-back proof
 *   jai_dmesg           kernel log tail (journalctl -k, dmesg fallback)
 *   jai_services        active/enabled state of jai-agent, jai-gateway, jai-link
 *   jai_sysinfo         host, kernel, uptime, load, memory, root disk, UTC
 *   jai_memory_read     /var/lib/jai-memory/<key>   (max 64 KB)
 *   jai_memory_write    /var/lib/jai-memory/<key>   (max 64 KB)
 *   disk_usage          statvfs on an absolute path (no subprocess)
 *   service_status      systemctl status + last 10 journal lines (read-only)
 *   service_restart     the one privileged door (double-locked, jai-* only)
 *
 * Daemon caveat: jai-agent.service sets NoNewPrivileges=yes, so when the
 * MCP server runs under the hardened daemon, sudo CANNOT elevate (setuid is
 * blocked) - service_restart returns sudo's refusal. This is by design; the
 * gateway phase gets a proper privileged helper. Interactive `opencode run`
 * (and the `jai` wrapper) do NOT inherit the flag, so the tool works there.
 *
 * Kernel semantics (jai_dev.c v0.2.0): read() drains the ring, oldest
 * first; when empty it returns a status line. write() pushes an event into
 * the ring AND mirrors it to the kernel log ("jai: event: ...").
 *
 * Build: gcc -O2 -Wall -Wextra -o jai-mcp jai-mcp.c -ljson-c
 */

#define _GNU_SOURCE
#include <json-c/json.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME    "jai-mcp"
#define SERVER_VERSION "1.0.0"

#define MAX_LINE      (1u << 20)   /* cap on one inbound JSON-RPC message */
#define OUT_CAP       8192         /* cap on captured subprocess output  */
#define CMD_TIMEOUT   15           /* seconds before a child gets killed */
#define DEV_JAI       "/dev/jai"
#define MEMORY_DIR    "/var/lib/jai-memory"
#define MAX_EVENT     254          /* jai_dev.c v0.2.0 per-write cap      */
#define MAX_MEMORY    (64 * 1024)  /* per-key memory cap                  */
#define MAX_KEY       64

static const char *JAI_UNITS[] = {
    "jai-agent.service", "jai-gateway.service", "jai-link.service"
};

/* ------------------------- bounded string helpers ----------------------- */

static void bappend(char *out, size_t *got, size_t cap, const char *s)
{
    size_t len = strlen(s);
    if (*got + 1 >= cap) return;
    if (*got + len >= cap - 1) len = cap - 1 - *got;
    memcpy(out + *got, s, len);
    *got += len;
    out[*got] = '\0';
}

static void bfmt(char *out, size_t *got, size_t cap, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    bappend(out, got, cap, tmp);
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n' || s[n-1]=='\r' || s[n-1]==' ' || s[n-1]=='\t'))
        s[--n] = '\0';
}

static void str_trim(char *s)
{
    size_t n = strlen(s);
    size_t start = 0;
    while (start < n && (s[start]==' ' || s[start]=='\t' ||
                         s[start]=='\n' || s[start]=='\r')) start++;
    memmove(s, s + start, n - start + 1);
    rtrim(s);
}

/* ------------------ validators: allowlist everything or refuse ---------- */

static int valid_unit(const char *s)      /* systemd unit name */
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 128) return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                 c == '@' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

static int valid_path(const char *s)      /* absolute, printable, bounded */
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 256 || s[0] != '/') return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] == 0x7f) return 0;
    return 1;
}

static int valid_key(const char *s)       /* memory key, python KEY_RE twin */
{
    size_t i, n = strlen(s);
    if (n == 0 || n > MAX_KEY) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
          (s[0] >= '0' && s[0] <= '9'))) return 0;
    for (i = 1; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

static const char *arg_string(json_object *args, const char *key)
{
    json_object *o;
    if (args && json_object_object_get_ex(args, key, &o) &&
        json_object_is_type(o, json_type_string))
        return json_object_get_string(o);
    return NULL;
}

static long arg_int(json_object *args, const char *key, long dflt)
{
    json_object *o;
    if (args && json_object_object_get_ex(args, key, &o))
        return json_object_get_int(o);
    return dflt;
}

/* ------------- subprocess: fork + execvp only, no shell, watchdog -------- */

static void alarm_handler(int sig) { (void)sig; }  /* just interrupts waitpid */

static int run_capture(char *const argv[], char *out, size_t cap)
{
    int pfd[2], rc = 0;
    pid_t pid;
    ssize_t r;
    size_t got = 0;

    out[0] = '\0';
    if (pipe(pfd) != 0) { bfmt(out, &got, cap, "pipe: %s", strerror(errno)); return -1; }

    pid = fork();
    if (pid < 0) {
        bfmt(out, &got, cap, "fork: %s", strerror(errno));
        close(pfd[0]); close(pfd[1]);
        return -1;
    }
    if (pid == 0) {                        /* child: stdout+stderr -> pipe */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);                        /* only reached if exec failed */
    }

    close(pfd[1]);
    while ((r = read(pfd[0], out + got, cap - 1 - got)) > 0) {
        got += (size_t)r;
        if (got >= cap - 1) break;         /* overflow tail dropped */
    }
    close(pfd[0]);
    out[got] = '\0';
    rtrim(out);
    got = strlen(out);

    alarm(CMD_TIMEOUT);
    if (waitpid(pid, &rc, 0) < 0) {
        alarm(0);
        if (errno == EINTR) {              /* watchdog fired */
            kill(pid, SIGKILL);
            waitpid(pid, &rc, 0);
            bfmt(out, &got, cap, "\n[jai-mcp: child killed after %ds]", CMD_TIMEOUT);
            return -1;
        }
        bfmt(out, &got, cap, "\n[jai-mcp: waitpid: %s]", strerror(errno));
        return -1;
    }
    alarm(0);

    if (WIFEXITED(rc))   return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc)) return 128 + WTERMSIG(rc);
    return -1;
}

/* ------------------------------ MCP plumbing ---------------------------- */

static void send_msg(json_object *msg)
{
    const char *s = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);                        /* one message = one flushed line */
}

static void send_result(json_object *id, json_object *result)
{
    json_object *m = json_object_new_object();
    json_object_object_add(m, "jsonrpc", json_object_new_string("2.0"));
    if (id) json_object_object_add(m, "id", json_object_get(id));
    json_object_object_add(m, "result", result);
    send_msg(m);
    json_object_put(m);
}

static void send_error(json_object *id, int code, const char *message)
{
    json_object *m = json_object_new_object();
    json_object *e = json_object_new_object();
    json_object_object_add(e, "code", json_object_new_int(code));
    json_object_object_add(e, "message", json_object_new_string(message));
    json_object_object_add(m, "jsonrpc", json_object_new_string("2.0"));
    if (id) json_object_object_add(m, "id", json_object_get(id));
    json_object_object_add(m, "error", e);
    send_msg(m);
    json_object_put(m);
}

static json_object *text_result(const char *text, int is_error)
{
    json_object *res  = json_object_new_object();
    json_object *arr  = json_object_new_array();
    json_object *item = json_object_new_object();

    json_object_object_add(item, "type", json_object_new_string("text"));
    json_object_object_add(item, "text", json_object_new_string(text));
    json_object_array_add(arr, item);
    json_object_object_add(res, "content", arr);
    if (is_error)
        json_object_object_add(res, "isError", json_object_new_boolean(1));
    return res;
}

/* ------------------------------ kernel tools ---------------------------- */

static json_object *tool_kernel_status(json_object *args)
{
    (void)args;
    char out[4608]; size_t got = 0;
    char buf[4096];
    ssize_t r, i;
    int fd;

    fd = open(DEV_JAI, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        bfmt(out, &got, sizeof out,
             "cannot open %s: %s\nhints: systemctl status systemd-modules-load; "
             "lsmod | grep jai; ls -l %s; id", DEV_JAI, strerror(errno), DEV_JAI);
        return text_result(out, 1);
    }
    r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r < 0) {
        bfmt(out, &got, sizeof out, "read on %s failed: %s", DEV_JAI, strerror(errno));
        return text_result(out, 1);
    }
    if (r == 0)
        return text_result("no output from /dev/jai (unexpected - kernel returns "
                           "a status line even when the ring is empty)", 1);
    for (i = 0; i < r; i++)                /* keep the JSON channel printable */
        if ((unsigned char)buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\t')
            buf[i] = '.';
    buf[r] = '\0';
    bfmt(out, &got, sizeof out, "%s", buf);
    return text_result(out, 0);
}

static json_object *tool_kernel_event(json_object *args)
{
    const char *text = arg_string(args, "text");
    char out[1024]; size_t got = 0;
    char buf[512], clean[512];
    ssize_t r;
    int fd;

    if (!text) return text_result("need: {\"text\": \"<event, <=254 chars>\"}", 1);
    snprintf(clean, sizeof clean, "%s", text);
    str_trim(clean);
    if (clean[0] == '\0')
        return text_result("need: {\"text\": \"<event, <=254 chars>\"}", 1);
    if (strlen(clean) > MAX_EVENT) {
        bfmt(out, &got, sizeof out,
             "event too long (%zu bytes) - kernel cap is %d bytes per write",
             strlen(clean), MAX_EVENT);
        return text_result(out, 1);
    }

    fd = open(DEV_JAI, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        bfmt(out, &got, sizeof out, "cannot open %s for write: %s\n"
             "hints: ls -l %s; id   (the jai user needs write access)",
             DEV_JAI, strerror(errno), DEV_JAI);
        return text_result(out, 1);
    }
    if (write(fd, clean, strlen(clean)) < 0) {
        bfmt(out, &got, sizeof out, "write to %s failed: %s", DEV_JAI, strerror(errno));
        close(fd);
        return text_result(out, 1);
    }
    close(fd);

    /* proof of life: drain the ring and show the kernel accepted the event */
    fd = open(DEV_JAI, O_RDONLY | O_NONBLOCK);
    buf[0] = '\0';
    if (fd >= 0) {
        r = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (r > 0) { buf[r] = '\0'; rtrim(buf); }
        else buf[0] = '\0';
    }
    bfmt(out, &got, sizeof out, "event written to kernel: \"%s\"\n", clean);
    bfmt(out, &got, sizeof out, "kernel log mirror: journalctl -k | grep 'jai: event'\n");
    bfmt(out, &got, sizeof out, "read-back from the ring: %s",
         buf[0] ? buf : "(ring already drained by another reader)");
    return text_result(out, 0);
}

static json_object *tool_dmesg(json_object *args)
{
    long n = arg_int(args, "lines", 20);
    char cap1[OUT_CAP];
    char out[OUT_CAP]; size_t got = 0;
    char num[16];
    int rc;

    if (n < 1) n = 1;
    if (n > 100) n = 100;                  /* dmesg_restrict=1 on Ubuntu: plain */
    snprintf(num, sizeof num, "%ld", n);   /* dmesg is EPERM for users; journalctl */
    char *a1[] = { "journalctl", "-k", "-n", num, "--no-pager", NULL };
    rc = run_capture(a1, cap1, sizeof cap1);
    if (rc != 0 || cap1[0] == '\0') {
        char *a2[] = { "dmesg", "--color=never", NULL };
        rc = run_capture(a2, cap1, sizeof cap1);
    }
    if (rc != 0 || cap1[0] == '\0')
        return text_result("cannot read kernel log - is user jai in the "
                           "systemd-journal group? (usermod -aG systemd-journal,adm jai)", 1);
    bfmt(out, &got, sizeof out, "kernel log (last %ld lines):\n", n);
    bappend(out, &got, sizeof out, cap1);
    return text_result(out, 0);
}

/* ----------------------------- system tools ----------------------------- */

static json_object *tool_services(json_object *args)
{
    (void)args;
    char out[2048]; size_t got = 0;
    char active[256], enabled[256];
    size_t i;

    bfmt(out, &got, sizeof out, "JAI systemd units:\n");
    for (i = 0; i < sizeof JAI_UNITS / sizeof JAI_UNITS[0]; i++) {
        char *a1[] = { "systemctl", "is-active",  (char *)JAI_UNITS[i], NULL };
        char *a2[] = { "systemctl", "is-enabled", (char *)JAI_UNITS[i], NULL };
        run_capture(a1, active,  sizeof active);
        run_capture(a2, enabled, sizeof enabled);
        bfmt(out, &got, sizeof out, "%-22s active=%-9s enabled=%s\n",
             JAI_UNITS[i],
             active[0]  ? active  : "unknown",
             enabled[0] ? enabled : "unknown");
    }
    return text_result(out, 0);
}

static json_object *tool_sysinfo(json_object *args)
{
    (void)args;
    char out[1024]; size_t got = 0;
    struct utsname un;
    struct sysinfo si;
    struct statvfs v;
    FILE *f;
    long mem_total = 0, mem_avail = 0, up;
    char load[128] = "?";
    char nowutc[32];
    time_t t;
    struct tm tmv;

    uname(&un);
    sysinfo(&si);
    up = si.uptime;

    f = fopen("/proc/meminfo", "r");
    if (f) {
        char k[64]; long v; int c, i;
        for (i = 0; i < 64 && fscanf(f, "%63s %ld", k, &v) == 2; i++) {
            if      (!strcmp(k, "MemTotal:"))     mem_total = v;
            else if (!strcmp(k, "MemAvailable:")) mem_avail = v;
            while ((c = fgetc(f)) != '\n' && c != EOF) {}   /* rest of line */
        }
        fclose(f);
    }
    f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fgets(load, sizeof load, f)) rtrim(load);
        fclose(f);
    }
    t = time(NULL);
    gmtime_r(&t, &tmv);
    strftime(nowutc, sizeof nowutc, "%Y-%m-%dT%H:%M:%SZ", &tmv);

    bfmt(out, &got, sizeof out, "host: %s (%s %s)\n", un.nodename, un.sysname, un.release);
    bfmt(out, &got, sizeof out, "uptime: %ldd %ldh %ldm\n",
         up / 86400, (up % 86400) / 3600, (up % 3600) / 60);
    bfmt(out, &got, sizeof out, "load: %s\n", load);
    bfmt(out, &got, sizeof out, "memory: %ld MB available / %ld MB\n",
         mem_avail / 1024, mem_total / 1024);
    if (statvfs("/", &v) == 0) {
        unsigned long long total = (unsigned long long)v.f_blocks * v.f_frsize;
        unsigned long long avail = (unsigned long long)v.f_bavail * v.f_frsize;
        bfmt(out, &got, sizeof out, "disk /: %llu GB free / %llu GB\n",
             avail >> 30, total >> 30);
    }
    bfmt(out, &got, sizeof out, "now (UTC): %s", nowutc);
    return text_result(out, 0);
}

static json_object *tool_disk_usage(json_object *args)
{
    const char *path = arg_string(args, "path");
    struct statvfs v;
    char out[1024]; size_t got = 0;
    unsigned long long total, freeb, avail, used, pct;

    if (!path) path = "/";
    if (!valid_path(path))
        return text_result("refused: path must be absolute, printable, <= 256 bytes", 1);
    if (statvfs(path, &v) != 0) {
        char b[256];
        snprintf(b, sizeof b, "statvfs(%s): %s", path, strerror(errno));
        return text_result(b, 1);
    }
    total = (unsigned long long)v.f_blocks * v.f_frsize;
    freeb = (unsigned long long)v.f_bfree  * v.f_frsize;
    avail = (unsigned long long)v.f_bavail * v.f_frsize;
    used  = total - freeb;
    pct   = total ? used * 100 / total : 0;

    bfmt(out, &got, sizeof out,
         "%s: %llu MiB used / %llu MiB total (%llu%% full), %llu MiB free\n",
         path, used >> 20, total >> 20, pct, avail >> 20);
    bfmt(out, &got, sizeof out, "verdict: %s",
         pct >= 90 ? "critically full" : pct >= 75 ? "getting full" : "healthy");
    return text_result(out, 0);
}

/* ------------------------------ service tools --------------------------- */

static json_object *tool_service_status(json_object *args)
{
    const char *unit = arg_string(args, "unit");
    char out[OUT_CAP]; size_t got = 0;
    char cap1[OUT_CAP], cap2[2048];
    char u[160];
    int rc;

    if (!unit) return text_result("missing argument: unit", 1);
    if (!valid_unit(unit))
        return text_result("refused: unit names may contain only a-z A-Z 0-9 _ . @ -", 1);
    snprintf(u, sizeof u, "%s", unit);

    char *a1[] = { "systemctl", "status", u, "--no-pager", "--lines", "10", NULL };
    rc = run_capture(a1, cap1, sizeof cap1);
    bfmt(out, &got, sizeof out,
         "systemctl status %s (exit %d; 0=active, 3=inactive, 4=unknown):\n", unit, rc);
    bappend(out, &got, sizeof out, cap1);
    bappend(out, &got, sizeof out, "\n--- last journal lines ---\n");

    char *a2[] = { "journalctl", "-u", u, "-n", "10", "--no-pager", "-o", "short", NULL };
    run_capture(a2, cap2, sizeof cap2);
    bappend(out, &got, sizeof out, cap2);
    return text_result(out, rc < 0 ? 1 : 0);
}

static json_object *tool_service_restart(json_object *args)
{
    const char *unit = arg_string(args, "unit");
    char out[2048]; size_t got = 0;
    char cap1[1024], cap2[256];
    char u[160];
    int rc;

    if (!unit) return text_result("missing argument: unit", 1);
    if (!valid_unit(unit))
        return text_result("refused: unit names may contain only a-z A-Z 0-9 _ . @ -", 1);

    /* Gate 1 (this code): jai-* units only. Gate 2 (the OS):
     * /etc/sudoers.d/jai-mcp whitelists exactly
     * `sudo -n /usr/bin/systemctl restart jai-*`. Two locks, different keys. */
    if (strncmp(unit, "jai-", 4) != 0)
        return text_result("refused: allowlist covers jai-* units only", 1);
    snprintf(u, sizeof u, "%s", unit);

    char *a1[] = { "sudo", "-n", "/usr/bin/systemctl", "restart", u, NULL };
    rc = run_capture(a1, cap1, sizeof cap1);
    if (rc != 0) {
        bfmt(out, &got, sizeof out,
             "restart of %s failed (exit %d):\n%s\nhints: does the unit exist? "
             "is /etc/sudoers.d/jai-mcp installed? "
             "(under jai-agent.service, NoNewPrivileges blocks sudo by design)",
             unit, rc, cap1);
        return text_result(out, 1);
    }
    char *a2[] = { "systemctl", "is-active", u, NULL };
    run_capture(a2, cap2, sizeof cap2);
    bfmt(out, &got, sizeof out, "restarted %s - state now: %s", unit, cap2);
    return text_result(out, 0);
}

/* ------------------------------ memory tools ---------------------------- */

static json_object *tool_memory_read(json_object *args)
{
    const char *key = arg_string(args, "key");
    char path[256];
    char *data;
    char out[MAX_MEMORY + 256]; size_t got = 0;
    ssize_t r;
    int fd;

    if (!key || !valid_key(key))
        return text_result("bad key: use [A-Za-z0-9] then [A-Za-z0-9._-], max 64 chars (no paths)", 1);
    snprintf(path, sizeof path, "%s/%s", MEMORY_DIR, key);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        bfmt(out, &got, sizeof out, "no memory at key '%s' (%s)", key, strerror(errno));
        return text_result(out, 1);
    }
    data = malloc(MAX_MEMORY + 2);
    if (!data) { close(fd); return text_result("out of memory", 1); }
    r = read(fd, data, MAX_MEMORY + 1);
    close(fd);
    if (r < 0) {
        free(data);
        bfmt(out, &got, sizeof out, "read %s failed: %s", path, strerror(errno));
        return text_result(out, 1);
    }
    data[r > 0 ? r : 0] = '\0';
    bfmt(out, &got, sizeof out, "%s", data);
    if (r > MAX_MEMORY) bappend(out, &got, sizeof out, "\n...(truncated at 64 KB)");
    free(data);
    return text_result(out, 0);
}

static json_object *tool_memory_write(json_object *args)
{
    const char *key = arg_string(args, "key");
    const char *content = arg_string(args, "content");
    char path[256];
    char out[512]; size_t got = 0;
    size_t len;
    int fd;

    if (!key || !valid_key(key))
        return text_result("bad key: use [A-Za-z0-9] then [A-Za-z0-9._-], max 64 chars (no paths)", 1);
    if (!content) return text_result("need: {\"key\": \"...\", \"content\": \"...\"}", 1);
    len = strlen(content);
    if (len > MAX_MEMORY) {
        bfmt(out, &got, sizeof out, "content too large (%zu bytes) - max is %d", len, MAX_MEMORY);
        return text_result(out, 1);
    }
    snprintf(path, sizeof path, "%s/%s", MEMORY_DIR, key);

    if (mkdir(MEMORY_DIR, 0755) != 0 && errno != EEXIST) {
        bfmt(out, &got, sizeof out, "mkdir %s failed: %s", MEMORY_DIR, strerror(errno));
        return text_result(out, 1);
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        bfmt(out, &got, sizeof out, "open %s failed: %s (is %s writable by jai?)",
             path, strerror(errno), MEMORY_DIR);
        return text_result(out, 1);
    }
    if (write(fd, content, len) < 0) {
        bfmt(out, &got, sizeof out, "write %s failed: %s", path, strerror(errno));
        close(fd);
        return text_result(out, 1);
    }
    close(fd);
    bfmt(out, &got, sizeof out, "stored %zu bytes -> %s", len, path);
    return text_result(out, 0);
}

/* ------------------------------ tools/list ------------------------------ */

static json_object *mk_tool(const char *name, const char *desc, json_object *schema)
{
    json_object *t = json_object_new_object();
    json_object_object_add(t, "name", json_object_new_string(name));
    json_object_object_add(t, "description", json_object_new_string(desc));
    json_object_object_add(t, "inputSchema", schema);
    return t;
}

static json_object *schema_obj(json_object *props, const char **required, int nreq)
{
    json_object *s = json_object_new_object();
    int i;
    json_object_object_add(s, "type", json_object_new_string("object"));
    if (props) json_object_object_add(s, "properties", props);
    if (nreq > 0) {
        json_object *req = json_object_new_array();
        for (i = 0; i < nreq; i++)
            json_object_array_add(req, json_object_new_string(required[i]));
        json_object_object_add(s, "required", req);
    }
    return s;
}

static json_object *str_prop(const char *desc, int max_len)
{
    json_object *p = json_object_new_object();
    json_object_object_add(p, "type", json_object_new_string("string"));
    json_object_object_add(p, "description", json_object_new_string(desc));
    if (max_len > 0)
        json_object_object_add(p, "maxLength", json_object_new_int(max_len));
    return p;
}

static json_object *handle_tools_list(void)
{
    json_object *arr = json_object_new_array();
    json_object *p_event, *p_dmesg, *p_mem, *p_memc, *p_du, *p_un;
    const char *req_text[]  = { "text" };
    const char *req_key[]   = { "key" };
    const char *req_keyc[]  = { "key", "content" };
    const char *req_unit[]  = { "unit" };

    /* jai_kernel_event: { text } */
    p_event = json_object_new_object();
    json_object_object_add(p_event, "text", str_prop("event text, max 254 bytes", MAX_EVENT));

    /* jai_dmesg: { lines } */
    p_dmesg = json_object_new_object();
    {
        json_object *li = json_object_new_object();
        json_object_object_add(li, "type", json_object_new_string("integer"));
        json_object_object_add(li, "description", json_object_new_string("how many lines (1-100, default 20)"));
        json_object_object_add(p_dmesg, "lines", li);
    }

    /* memory: { key } and { key, content } */
    p_mem = json_object_new_object();
    json_object_object_add(p_mem, "key",
        str_prop("memory key: [A-Za-z0-9] then [A-Za-z0-9._-], max 64 chars", MAX_KEY));
    p_memc = json_object_new_object();
    {
        json_object *k = str_prop("memory key", MAX_KEY);
        json_object *c = str_prop("content to store, max 64 KB", 0);
        json_object_object_add(p_memc, "key", k);
        json_object_object_add(p_memc, "content", c);
    }

    /* disk_usage: { path } */
    p_du = json_object_new_object();
    json_object_object_add(p_du, "path", str_prop("absolute path to inspect (default /)", 0));

    /* service_status / service_restart: { unit } */
    p_un = json_object_new_object();
    json_object_object_add(p_un, "unit", str_prop("systemd unit name, e.g. jai-heartbeat.service", 0));

    json_object_array_add(arr, mk_tool("jai_kernel_status",
        "Read /dev/jai: drains pending kernel events (oldest first); when the ring "
        "is empty the kernel returns its status line. Proves the agent-kernel link.",
        schema_obj(NULL, NULL, 0)));
    json_object_array_add(arr, mk_tool("jai_kernel_event",
        "Write a short event (max 254 bytes) to /dev/jai. It enters the kernel event "
        "ring buffer AND the kernel log ('jai: event: <text>'). Returns the read-back as proof.",
        schema_obj(p_event, req_text, 1)));
    json_object_array_add(arr, mk_tool("jai_dmesg",
        "Tail of the kernel log (journalctl -k, dmesg fallback).",
        schema_obj(p_dmesg, NULL, 0)));
    json_object_array_add(arr, mk_tool("jai_services",
        "Health of the JAI systemd units (jai-agent, jai-gateway, jai-link): active + enabled.",
        schema_obj(NULL, NULL, 0)));
    json_object_array_add(arr, mk_tool("jai_sysinfo",
        "Host summary: host, kernel, uptime, load, memory, root disk, UTC time.",
        schema_obj(NULL, NULL, 0)));
    json_object_array_add(arr, mk_tool("jai_memory_read",
        "Read long-term memory: /var/lib/jai-memory/<key> (max 64 KB).",
        schema_obj(p_mem, req_key, 1)));
    json_object_array_add(arr, mk_tool("jai_memory_write",
        "Write long-term memory: /var/lib/jai-memory/<key> (max 64 KB).",
        schema_obj(p_memc, req_keyc, 2)));
    json_object_array_add(arr, mk_tool("disk_usage",
        "Filesystem usage for an absolute path (statvfs, no subprocess).",
        schema_obj(p_du, NULL, 0)));
    json_object_array_add(arr, mk_tool("service_status",
        "systemd unit status plus its last 10 journal lines (read-only, any unit).",
        schema_obj(p_un, req_unit, 1)));
    json_object_array_add(arr, mk_tool("service_restart",
        "Restart a jai-* systemd unit via the sudo allowlist - the only privileged "
        "action JAI has (gated in this server AND in /etc/sudoers.d/jai-mcp).",
        schema_obj(p_un, req_unit, 1)));

    json_object *res = json_object_new_object();
    json_object_object_add(res, "tools", arr);
    return res;
}

static json_object *handle_tools_call(json_object *params)
{
    json_object *args = NULL, *nameo = NULL;
    const char *name = NULL;
    char b[256];

    if (params) {
        if (json_object_object_get_ex(params, "name", &nameo))
            name = json_object_get_string(nameo);
        json_object_object_get_ex(params, "arguments", &args);
    }
    if (!name)
        return text_result("bad request: missing tool name", 1);

    if      (!strcmp(name, "jai_kernel_status")) return tool_kernel_status(args);
    else if (!strcmp(name, "jai_kernel_event"))  return tool_kernel_event(args);
    else if (!strcmp(name, "jai_dmesg"))         return tool_dmesg(args);
    else if (!strcmp(name, "jai_services"))      return tool_services(args);
    else if (!strcmp(name, "jai_sysinfo"))       return tool_sysinfo(args);
    else if (!strcmp(name, "jai_memory_read"))   return tool_memory_read(args);
    else if (!strcmp(name, "jai_memory_write"))  return tool_memory_write(args);
    else if (!strcmp(name, "disk_usage"))        return tool_disk_usage(args);
    else if (!strcmp(name, "service_status"))    return tool_service_status(args);
    else if (!strcmp(name, "service_restart"))   return tool_service_restart(args);

    snprintf(b, sizeof b, "unknown tool: %s", name);
    return text_result(b, 1);
}

static json_object *handle_initialize(json_object *params)
{
    json_object *res = json_object_new_object();
    json_object *caps = json_object_new_object();
    json_object *tools = json_object_new_object();
    json_object *info = json_object_new_object();
    json_object *clientver;
    const char *ver = "2024-11-05";

    /* echo the client's protocol version back (same choice jai-system.py made) */
    if (params && json_object_object_get_ex(params, "protocolVersion", &clientver) &&
        json_object_is_type(clientver, json_type_string))
        ver = json_object_get_string(clientver);

    json_object_object_add(tools, "listChanged", json_object_new_boolean(0));
    json_object_object_add(caps, "tools", tools);
    json_object_object_add(info, "name", json_object_new_string(SERVER_NAME));
    json_object_object_add(info, "version", json_object_new_string(SERVER_VERSION));
    json_object_object_add(res, "protocolVersion", json_object_new_string(ver));
    json_object_object_add(res, "capabilities", caps);
    json_object_object_add(res, "serverInfo", info);
    return res;
}

int main(void)
{
    char *line = NULL;
    size_t n = 0;
    ssize_t len;
    struct sigaction sa;

    signal(SIGPIPE, SIG_IGN);              /* client died? get EPIPE, do not die */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = alarm_handler;
    sigaction(SIGALRM, &sa, NULL);

    fprintf(stderr, "%s v%s - the hands of JAI online (pid %d)\n",
            SERVER_NAME, SERVER_VERSION, (int)getpid());

    while ((len = getline(&line, &n, stdin)) >= 0) {
        json_object *msg, *id = NULL, *methodo = NULL, *params = NULL;
        const char *method = NULL;
        ssize_t i;
        int blank = 1;

        if (len > (ssize_t)MAX_LINE) { send_error(NULL, -32600, "message too large"); continue; }
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        for (i = 0; i < len; i++)
            if (line[i] != ' ' && line[i] != '\t') { blank = 0; break; }
        if (blank) continue;

        msg = json_tokener_parse(line);
        if (!msg) { send_error(NULL, -32700, "parse error"); continue; }

        json_object_object_get_ex(msg, "id", &id);
        if (json_object_object_get_ex(msg, "method", &methodo))
            method = json_object_get_string(methodo);
        json_object_object_get_ex(msg, "params", &params);

        if (!method) {
            send_error(id, -32600, "invalid request: missing method");
        } else if (!strcmp(method, "initialize")) {
            send_result(id, handle_initialize(params));
        } else if (!strcmp(method, "notifications/initialized")) {
            /* notification - never answered */
        } else if (!strcmp(method, "ping")) {
            send_result(id, json_object_new_object());
        } else if (!strcmp(method, "tools/list")) {
            send_result(id, handle_tools_list());
        } else if (!strcmp(method, "tools/call")) {
            send_result(id, handle_tools_call(params));
        } else if (!strcmp(method, "prompts/list")) {
            json_object *r = json_object_new_object();
            json_object_object_add(r, "prompts", json_object_new_array());
            send_result(id, r);
        } else if (!strcmp(method, "resources/list")) {
            json_object *r = json_object_new_object();
            json_object_object_add(r, "resources", json_object_new_array());
            send_result(id, r);
        } else if (id) {
            send_error(id, -32601, "method not found");
        }
        /* unknown notification without id: silently ignored */
        json_object_put(msg);
    }
    free(line);
    fprintf(stderr, "%s - stdin closed, hands down\n", SERVER_NAME);
    return 0;
}
