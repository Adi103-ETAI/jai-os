/*
 * jai-eventd.c - the reflex arc of JAI (Phase 3c): kernel events -> JSONL
 *
 * jai_dev.c v0.2.0 semantics (the contract this daemon is built on):
 *
 *   - write() to /dev/jai pushes <=254 bytes of text into a 4 KiB ring
 *     buffer AND mirrors it to the kernel log ("jai: event: ...").
 *   - read() DRAINS pending ring bytes (oldest first). When the ring is
 *     empty, read() returns a two-line STATUS frame and then EOF:
 *       "JAI is awake. Kernel link established."
 *       "JAI core v0.2.0 | events: N total | opens: M"
 *   - The device NEVER blocks. That kills the classic "blocking reader"
 *     design, so eventd is a DRAIN-POLLER instead: every POLL_MS it opens
 *     the device, drains whatever is pending (and usually a status frame
 *     at the end of the drain), closes, and sleeps. Stateless, cheap,
 *     tolerant of module reloads (open failure = retry next tick).
 *
 * The status frame is not just noise: "events: N total" is a kernel-side
 * monotonic counter of accepted writes. eventd diffs it against what it
 * actually captured - if the kernel accepted more than eventd saw, the
 * ring overflowed between polls and a GAP marker is written to the JSONL.
 * Telemetry may have holes; the brain is TOLD about them, never left to
 * guess that the stream is complete.
 *
 * Output: one JSON object per line at /var/lib/jai/events.jsonl, e.g.
 *   {"ts":"2026-11-02T10:05:12.480Z","seq":1,"text":"gate opened"}
 *   {"ts":"...","seq":9,"gap":3,"note":"~3 events lost to ring overflow"}
 * Every object is built with json-c - never hand-concatenated strings
 * (same house rule as jai-mcp.c: this file faces untrusted event text).
 *
 * Known v0.2.0 protocol limits (documented in DECISIONS.md):
 *   - an event whose text begins "JAI is awake." is indistinguishable
 *     from a status frame - don't write those (jai_dev v0.3 would add
 *     real framing if this ever matters)
 *   - an event containing embedded newlines appears as several lines,
 *     so gap accounting is approximate
 *
 * Build: gcc -O2 -Wall -Wextra -o jai-eventd jai-eventd.c -ljson-c
 */

#define _GNU_SOURCE
#include <json-c/json.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define EVENTD_VERSION "1.0.0"
#define DFL_DEV        "/dev/jai"
#define DFL_OUT        "/var/lib/jai/events.jsonl"
#define DFL_MS         500
#define RING_MAX       4096              /* jai_dev.c v0.2.0 ring size  */
#define STATUS_PREFIX  "JAI is awake."

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void iso_ts(char *b, size_t n)
{
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    strftime(b, n, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(b + strlen(b), n - strlen(b), ".%03uZ",
             (unsigned)(ts.tv_nsec / 1000000));
}

/* append one json-c object as a JSONL line (open/append/close per record:
 * survives logrotate copytruncate, never holds the file open) */
static void emit(const char *path, json_object *obj, uint64_t *seq)
{
    const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);

    if (fd >= 0) {
        if (write(fd, s, strlen(s)) < 0 || write(fd, "\n", 1) < 0)
            fprintf(stderr, "jai-eventd: write %s: %s\n", path, strerror(errno));
        close(fd);
    } else {
        fprintf(stderr, "jai-eventd: open %s: %s\n", path, strerror(errno));
    }
    (*seq)++;
    json_object_put(obj);
}

static void emit_event(const char *path, uint64_t *seq, const char *text)
{
    char ts[40];
    json_object *o = json_object_new_object();

    iso_ts(ts, sizeof ts);
    json_object_object_add(o, "ts",  json_object_new_string(ts));
    json_object_object_add(o, "seq", json_object_new_int64((int64_t)*seq));
    json_object_object_add(o, "text", json_object_new_string(text));
    emit(path, o, seq);
}

static void emit_gap(const char *path, uint64_t *seq, uint64_t lost)
{
    char ts[40], note[96];
    json_object *o = json_object_new_object();

    iso_ts(ts, sizeof ts);
    snprintf(note, sizeof note, "~%" PRIu64 " events lost to ring overflow", lost);
    json_object_object_add(o, "ts",   json_object_new_string(ts));
    json_object_object_add(o, "seq",  json_object_new_int64((int64_t)*seq));
    json_object_object_add(o, "gap",  json_object_new_int64((int64_t)lost));
    json_object_object_add(o, "note", json_object_new_string(note));
    emit(path, o, seq);
}

static int looks_like_status(const char *s)
{
    return strncmp(s, STATUS_PREFIX, strlen(STATUS_PREFIX)) == 0;
}

/* "JAI core v0.2.0 | events: N total | opens: M" */
static void parse_status_line(const char *s, uint64_t *total, long *opens)
{
    const char *e = strstr(s, "events:");
    const char *o = strstr(s, "opens:");

    if (e) *total = strtoull(e + 7, NULL, 10);
    if (o) *opens = strtol(o + 6, NULL, 10);
}

int main(int argc, char **argv)
{
    const char *dev = DFL_DEV;
    const char *out = DFL_OUT;
    long ms = DFL_MS;
    long opens_dummy = 0;
    uint64_t seq = 0;                 /* eventd-local record counter  */
    uint64_t last_total = 0;          /* kernel accepted-writes       */
    int have_total = 0;

    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("jai-eventd v%s - the reflex arc (kernel events -> JSONL)\n",
               EVENTD_VERSION);
        return 0;
    }
    if (argc > 1) dev = argv[1];
    if (argc > 2) out = argv[2];
    if (argc > 3) {
        ms = atol(argv[3]);
        if (ms < 50)    ms = 50;      /* do not hot-spin the 1 GB VM  */
        if (ms > 60000) ms = 60000;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGHUP, SIG_IGN);          /* survive terminal hangups     */

    mkdir("/var/lib/jai", 0755);      /* best effort; unit also guards */
    fprintf(stderr, "jai-eventd v%s online: %s -> %s every %ldms (pid %d)\n",
            EVENTD_VERSION, dev, out, ms, (int)getpid());

    while (!g_stop) {
        char buf[RING_MAX + 512];     /* ring dump + trailing status  */
        size_t acc = 0;
        int fd;

        fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            /* module not loaded yet / being reloaded - systemd
             * Restart=always is the net; say it once, keep quiet after */
            static int said = 0;
            if (!said) {
                fprintf(stderr, "jai-eventd: open %s: %s (lsmod | grep jai_dev?)"
                                " - retrying\n", dev, strerror(errno));
                said = 1;
            }
            goto sleep_tick;
        }

        while (acc < sizeof buf - 1) {
            ssize_t r = read(fd, buf + acc, sizeof buf - 1 - acc);
            if (r > 0) { acc += (size_t)r; continue; }
            break;                    /* 0 = EOF (drain done) or error */
        }
        close(fd);

        if (acc == 0)
            goto sleep_tick;
        buf[acc] = '\0';

        /* line-walk: emit events, harvest the status counter when present */
        {
            uint64_t lines = 0, total_seen = 0;
            int have_seen = 0;
            char *p = buf;
            char *end = buf + acc;

            while (p < end) {
                char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

                p[len] = '\0';
                if (!strncmp(p, "JAI core v", 10)) {
                    parse_status_line(p, &total_seen, &opens_dummy);
                    have_seen = 1;
                } else if (!looks_like_status(p)) {
                    if (len > 0) { emit_event(out, &seq, p); lines++; }
                }
                if (!nl) break;
                p = nl + 1;
            }

            if (have_seen) {
                if (have_total) {
                    if (total_seen < last_total)
                        fprintf(stderr, "jai-eventd: kernel counter reset "
                                "(%" PRIu64 " -> %" PRIu64 ", module reload?)\n",
                                last_total, total_seen);
                    else if (total_seen - last_total > lines)
                        emit_gap(out, &seq, total_seen - last_total - lines);
                }
                last_total = total_seen;
                have_total = 1;
            }
        }

sleep_tick:
        {
            struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
            while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !g_stop) {}
        }
    }

    fprintf(stderr, "jai-eventd: stopped at seq %" PRIu64 "\n", seq);
    return 0;
}
