# jai-decode — the event decoder (roadmap step 3, the "4b decoder")

> Spec BEFORE code. This document is the contract: the decoded-record
> schema, the rule-table format, the state/resume rules. The C code must
> obey this file. This step UNBLOCKS the whole critical path (4 → 5 → 6).

## What it is, in plain words

The kernel already feels things and `jai-eventd` already writes them
down — but events.jsonl is just raw text lines: `"doctor ping"`,
`"disk 92% full"`. A brain cannot budget attention on raw noise.

    kernel reflexes (jai_dev.ko)
         │  events ≤ 254 B each, 4 KiB ring
         ▼
    jai-eventd          raw JSONL scribe (SHIPPED, v1.0.0)
         │  /var/lib/jai/events.jsonl
         ▼
    jai-decode  ◄── THIS SPEC — classify + enrich, never drop
         │  /var/lib/jai/decoded.jsonl
         ├──► jai-gate (step 5)   reads kind+severity → wake or note
         └──► brain (MCP/reads)   reads MEANING, not noise

The decoder is a translator, not a judge: it adds "what kind of thing
is this and how much should anyone care" to every record. Deciding
whether the brain wakes up is the GATE's job (step 5). Keeping those
two apart is the design: decoding is deterministic and free, waking is
lossy and budgeted.

## Ground truth (the contracts this code sits on)

| Fact                                      | Source (read, not guessed)                        |
|-------------------------------------------|---------------------------------------------------|
| event record `{ts,seq,text}`              | jai-eventd.c `emit_event()` — ts = ISO-8601 ms UTC, seq = eventd-LOCAL counter (starts 0 per eventd run; NOT the kernel counter) |
| gap record `{ts,seq,gap,note}`            | jai-eventd.c `emit_gap()` — `note` = "~K events lost to ring overflow" |
| text ≤ 254 B, always `\n`-terminated      | jai_dev.c `JAI_EVENT_MAX` 254, write path adds newline |
| records may interleave in one 500 ms poll | eventd drain-poll loop; ordering within a poll = ring order (oldest first) |
| counter reset possible (module reload)    | eventd logs it; seq does NOT reset — safe to keep streaming |
| events are DATA, never COMMANDS           | VISION.md Law 2 + ARCHITECTURE-V2 §8 — decoder output is classification only, no shell, no interpretation as instructions |

## Output schema — decoded.jsonl v1 (additive, self-contained)

Every line copies the raw record and ADDS exactly two fields. Nothing
is removed, reordered, or suppressed (lossless rule):

    {"ts":"2026-11-05T18:05:07.410Z","seq":1,"text":"doctor ping 18:05:07",
     "kind":"test","severity":0}

    gap lines pass through, kind=gap, severity=1 (a hole in telemetry is
    itself worth attention):
    {"ts":"...","seq":9,"gap":3,"note":"~3 events lost to ring overflow",
     "kind":"gap","severity":1}

- Adding a key later = allowed. Removing/renaming = schema v2 (bump).
- decoded.jsonl is APPEND-ONLY like events.jsonl (open/append/close per
  record — survives copytruncate, never holds the file open).

## Classification rules — table-driven, no recompiles

`/etc/jai/decode-rules.conf` (admin-editable; missing file = compiled-in
defaults — the fallback-chain law applies to rules too):

    # prefix<TAB>kind<TAB>severity      (first match wins; case-sensitive)
    disk<TAB>disk<TAB>2
    oom<TAB>kernel<TAB>2
    ssh<TAB>auth<TAB>1
    service<TAB>service<TAB>1
    gate<TAB>brain<TAB>1
    doctor ping<TAB>test<TAB>0

- v1 matches FIXED PREFIXES only (`strncmp`). No regex in the parser —
  auditable by eye, fast, and injection-proof. Fuzzy matching is
  tier-1's job, not a C string parser's.
- No match → `kind:"generic", severity:0`. Unknown is never dropped.
- Rule edits apply to NEW records only; old lines are never re-decoded.

## Runtime shape

- One small C daemon `agent/decode/jai-decode.c` (same recipe as
  eventd/mcp/capabilities: C99 + json-c, gcc -O2 -Wall -Wextra, zero
  hard deps beyond json-c).
- Tail-follows events.jsonl by persisted byte offset
  (`/var/lib/jai/decode.state`: offset + schema version, atomic
  tmp+rename write, 0644). Poll every 500 ms like eventd.
- Truncation/shrink detection (logrotate copytruncate): offset > size →
  reset to 0, emit one pulse-style stderr note, continue. No loss
  because copytruncate keeps the file, it just shrinks it.
- `--once` mode: drain what's pending and exit (doctor + tests).
- systemd `jai-decode.service`: `After=jai-eventd.service`,
  `Before=jai-gate.service`, `Restart=always`, hardened like eventd.
- Rejected for now: folding decoding INTO eventd (one fewer daemon).
  Trigger to revisit: measured CPU/RSS of the 3-daemon chain hurts on
  MINIMAL. Composability wins first; measurement may merge them later.

## Consumers

| Consumer          | How it reads decoded.jsonl                    |
|-------------------|-----------------------------------------------|
| jai-gate (step 5) | tail-follow + kind/severity → class decision  |
| brain             | reads the file directly (jai user owns it); optional MCP tool `jai_decoded` if turn-economy demands it — decided at implementation review, NOT speced as required |
| vm-doctor.sh      | writes a test event, expects the decoded line |

## Acceptance criteria (all must pass to call step 3 DONE)

1. `echo "disk 92% full on /" | sudo tee /dev/jai` → decoded line with
   `kind:"disk","severity":2` within 1 s.
2. Unknown text → `kind:"generic","severity":0`, line present (never dropped).
3. Force a gap (pause eventd, flood writes, resume) → gap record decoded
   as `kind:"gap","severity":1`, `gap` number preserved.
4. `kill -9` the decoder → systemd restarts → resume from offset: zero
   duplicates, zero loss (diff events vs decoded counts by seq).
5. Restart decoder twice → decoded.jsonl does NOT grow (state file works).
6. Truncate events.jsonl to 0 bytes while running → decoder recovers,
   keeps decoding (criterion: copytruncate survival).
7. `--once` mode: processes backlog and exits 0.
8. Idle cost: < 1% CPU, RSS < 5 MB on the 1 GB VM (measure with
   `systemd-cgtop` / `ps -o rss`).
9. `vm-doctor.sh` gains a decoder check (event in → decoded out).
10. Same recipe discipline: warning-free build (-Wall -Wextra, fortify
    clean), fork-free (no subprocesses at all), json-c only.

## Honest limitations (stated, not hidden)

- Prefix matching only — "DISK" (uppercase) or "the disk is..." do NOT
  match "disk". Writers (doctor, future reflexes) should emit tagged
  text; the rule table is the contract between writers and decoder.
- seq is eventd-local: decoded seq values restart if eventd restarts
  with a wiped state. Ordering within one eventd lifetime is exact;
  across restarts, `ts` is the tiebreaker.
- No regex, no numbers extracted (v1). "92" stays inside `text`.
  If extraction is ever needed, that's a v2 with a version bump.
- The decoder trusts eventd's file format; a malformed JSON line is
  passed through with `kind:"generic","severity":0` and a stderr note —
  never crashes, never skips the offset.
