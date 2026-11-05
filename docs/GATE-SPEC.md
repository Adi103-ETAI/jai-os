# jai-gate — the wake policy (roadmap step 5, the "6b gate")

> Spec BEFORE code. This is the contract for how JAI decides when the
> brain wakes up, when it just takes a note, and when it ignores — and
> how the OS never burns quota on nothing. Depends on: step 3 decoder
> (kind+severity), step 4 lessons, 6a warm brain (HTTP endpoint).
> Unblocks: Layer-0 fast path (step 6) and every budget decision after.

## What it is, in plain words

Every event that lands in decoded.jsonl asks a question: "does the
brain need to hear this NOW?" Waking the brain costs money (cloud
quota) and seconds. The gate is the bouncer:

    decoded.jsonl (kind, severity)
         │
         ▼
    jai-gate (daemon, C, json-c)  ── reads /etc/jai/gate-rules.conf
         │
         ├─ Class 0  ignore   → pulse line only. Brain blind. FREE.
         ├─ Class 1  note     → append to inbox.jsonl. Brain pulls
         │                      it next time it's awake. FREE.
         └─ Class 2  wake     → POST to the warm brain NOW.
                                SPENDS BUDGET (rate-limited).

Three promises this enforces (ARCHITECTURE-V2 §1):
1. answers in seconds when spoken to (user questions are NEVER gated),
2. reacts to danger even when idle (sev-2 events wake the brain),
3. never burns quota on nothing (budget + dedup + serialization).

## The one refinement this spec adds to ARCHITECTURE-V2 §4

The v2 table lists "user question" as a Class-2 trigger. THAT WAS
WRONG-SCOPED: user questions arrive through the `jai` wrapper and go
straight to the brain (Law 2: the user is the boss — the gate never
stands between the user and an answer). The gate governs AUTONOMOUS
wakes only: events the machine generated on its own. This keeps the
gate from ever becoming a tax on the user.

| Trigger                        | Routed by | Gated? |
|--------------------------------|-----------|--------|
| user question (`jai "..."`)    | wrapper   | NO — always allowed |
| decoded event severity 2       | gate      | YES — budgeted wake |
| decoded event severity 1       | gate      | note (free) |
| decoded event severity 0       | gate      | ignore (free) |

## Classification — severity → class, then rules may override

Default: `severity 0→Class 0, 1→Class 1, 2→Class 2`.

`/etc/jai/gate-rules.conf` (human-editable; later the output of
jai-review in step 12) overrides per kind:

    # kind<TAB>class<TAB>reason          (kind from the decoder)
    test<TAB>0<TAB>doctor pings are noise
    auth<TAB>2<TAB>new ssh session is worth waking for
    disk<TAB>2<TAB>disk pressure is always urgent
    kernel<TAB>2<TAB>oom never waits

- Missing conf → pure severity mapping (fallback-chain law: the gate
  works with zero config).
- Unknown kind → severity mapping, never an error.
- File mtime watched (or checked per decision — it's a tiny file);
  edits apply to the NEXT decision. No reload protocol, no daemon
  restart needed.

## Budget — /etc/jai/gate.conf

    # wake budget (autonomous Class-2 wakes only)
    max_wakes_per_hour=4
    daily_cap=20
    dedup_seconds=300

Enforcement order per Class-2 candidate:
1. **Dedup**: same `kind` woke the brain within dedup_seconds →
   downgrade to note + pulse `{"suppressed":"dedup"}`.
2. **Hourly cap**: sliding 60-min window of recorded wakes ≥ max →
   downgrade to note + pulse `{"suppressed":"hourly"}`.
3. **Daily cap**: UTC-day wake count ≥ daily_cap → downgrade to note +
   pulse `{"suppressed":"daily"}`.
4. **In-flight lock**: another wake already in flight → the event waits
   as a Class-1 note (the brain pulls the inbox; it will see it).

Every downgrade writes a pulse line with the reason. The gate is never
silent: if the brain didn't wake, pulse.jsonl says exactly why.

## The wake — one HTTP POST, zero new dependencies

- POST to the warm brain endpoint, URL + path from gate.conf
  (`brain_url=http://127.0.0.1:4096`, `brain_path=/api/...` — exact
  path pinned during 6a bring-up; it is CONFIG, not compiled code, so
  swapping agents stays a 2-file change, ARCHITECTURE-V2 §11).
- Body (json-c, never hand-concatenated):
      {"source":"jai-gate","kind":"disk","severity":2,
       "text":"disk 92% full on /","ts":"...",
       "inbox":[{"kind":"auth","text":"...", "ts":"..."}]}
  (inbox = pending Class-1 notes, max 10, then truncated — the notes
  stay in inbox.jsonl regardless).
- Implementation: one raw-socket HTTP/1.1 write + short read for the
  status line. NO libcurl — the gate has exactly one HTTP thing to do.
- Timeout 5 s, 2 retries (1 s apart), then **downgrade to note** +
  pulse `{"suppressed":"brain-down"}`. A dead brain degrades to a
  full inbox, never to a crash loop (fallback-chain law).
- In-flight marker `/var/lib/jai/gate.state` (ts + kind, atomic write).
  On gate start: marker older than 120 s → stale, clear + pulse it.
  This is the kill -9 recovery path.

## Serialization + memory

One thought in flight, enforced in-process (single-threaded daemon —
there is no concurrency to lock). Multi-wake bursts queue as notes and
go out with the NEXT wake's inbox. After a wake completes (response or
timeout), pulse records the round trip ms.

## Pulse — every decision is a line

`/var/lib/jai/brain/pulse.jsonl` (owner: wrapper + gate per the memory
map), one line per decision:

    {"ts":"...","actor":"gate","kind":"disk","severity":2,"class":2,
     "action":"wake","suppressed":null,"reason":"budget-ok","ms":1820}
    {"ts":"...","actor":"gate","kind":"test","severity":0,"class":0,
     "action":"ignore","suppressed":null,"reason":"severity-0"}

This is the audit trail for every quota cent and every silence.

## Runtime shape

- `agent/gate/jai-gate.c` — C99 + json-c, same recipe (gcc -O2 -Wall
  -Wextra, fortify-clean, no shell, no popen, fixed buffers).
- Tail-follows decoded.jsonl by byte offset (`/var/lib/jai/gate.state`
  also carries budget counters: hour-window array + day stamp — all
  crash-safe because a lost counter can only make the gate MORE
  conservative after reboot: counters reset → budget resets → wake
  may happen once extra. Accepted, documented).
- systemd `jai-gate.service`: `After=jai-decode.service
  jai-brain.service`, `Before=multi-user.target`, `Restart=on-failure`,
  hardened like the rest.
- Config load at start + per-decision mtime check. Bad line in a conf
  → skip that line + stderr note, never die (partial config beats no
  config; the fallback mapping always exists).

## Acceptance criteria (all must pass to call step 5 DONE)

1. sev-2 event → POST hits a mock listener (`nc -l`) within 2 s, body
   parses as JSON with kind/text/inbox fields.
2. Budget: fire max_wakes_per_hour+1 distinct sev-2 kinds → last ones
   suppressed with `{"suppressed":"hourly"}` in pulse.
3. Dedup: same kind twice within dedup_seconds → second is a note.
4. Serialization: 5 sev-2 events at once → 1 wake + 4 inbox notes.
5. Brain down (nothing listening) → 2 retries → note fallback, gate
   alive, pulse says brain-down. Gate uptime unaffected.
6. User path untouched: `jai "hello"` never consults gate state.
7. kill -9 gate mid-wake → restart → stale marker cleared → system
   consistent (no double-wake of the same event; inbox intact).
8. gate-rules.conf edit → next decision uses new class (no restart).
9. Idle cost: < 1% CPU, RSS < 5 MB on the 1 GB VM.
10. vm-doctor.sh gains the gate check (test event → pulse line shows
    the decision; budget counters readable).
11. Warning-free build, json-c only, all state files atomic 0644.

## Honest limitations (stated, not hidden)

- Budget counters reset on reboot (crash-safety tradeoff documented
  above): a rebooting machine can spend up to one extra wake per hour.
  Accepted for v1; persisting counters is a v2 with a version bump.
- The gate trusts the decoder's severity. A writer that tags everything
  severity-2 can drain the budget — the defense is dedup + caps, and
  writers are OUR code (the doctor, the wrapper, future reflexes).
- "Wake" today = brain POST + pulse + response logged. Telling the
  USER ("disk is 92% — want me to clean it?") is the M3/M4 notification
  path, not the gate's job. The gate's reply-to-user surface is a
  later milestone; until then, gate-initiated thoughts land in
  events/pulse where the user can read them via jai_events.
- Class decisions are instantaneous; there is no "hold and batch"
  window. If batching is ever needed (event storms), that's a v2
  debounce parameter — v1 keeps the decision loop trivial.
