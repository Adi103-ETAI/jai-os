# Layer-0 — the fast path (roadmap step 6: "disk?" in < 1 s, 0 tokens)

> Spec BEFORE code. This is the contract for the question router's
> first rung: pattern → tool → template answer, in under a second,
> spending nothing. Depends on: 2b capabilities.json (SHIPPED — the
> one truth file finally pays its rent), jai-mcp tool set (SHIPPED,
> for the answer sources). Unblocks: nothing — this is the payoff step
> of the whole system chain (3→4→5→6).

## What it is, in plain words

Most questions a user asks a machine are NOT hard:

    "disk?"            "how much RAM?"       "is ssh running?"
    "what profile?"    "am I online?"        "any kernel events?"

They deserve tool-speed answers, not LLM-speed answers. Layer-0 is a
tiny C matcher inside the `jai` wrapper chain:

    YOU: jai "disk?"
       │
       ▼
    [1] jai-layer0  ── match? ──► YES: read tools + template ──► answer
       │                            (< 1 s, zero tokens, zero network)
       │ NO match (exit 1)
       ▼
    [2] warm brain (wrapper v2 behavior, unchanged)     ← quota path
       │
       ▼
    [3] cold fallback (opencode standalone)             ← last resort

ARCHITECTURE-V2 §2 predicts ~70% of daily questions die at [1]. Every
one that does is a quota saved and 45+ seconds never spent.

## The rule table — /etc/jai/layer0-rules.conf

    # keyword<TAB>action         (keyword = lowercase substring match,
    disk<TAB>disk_root            first match wins)
    space<TAB>disk_root
    ram<TAB>memory
    memory<TAB>memory
    profile<TAB>profile
    gpu<TAB>accel
    online<TAB>net
    internet<TAB>net
    uptime<TAB>uptime
    events<TAB>events_recent
    ssh<TAB>service_ssh
    running<TAB>service_ssh

- **No free text into exec — ever.** A rule maps a keyword to a FIXED
  action name. User words never touch a shell, a path, or a command
  line (the jai-mcp security posture, applied one layer earlier: here
  there is not even an argument to allowlist — there are no arguments).
- Unknown action name in conf → line skipped + stderr note.
- Missing conf → compiled-in default table (fallback-chain law).
- Match = case-insensitive substring on the whole question. Longest
  keyword wins if several match (deterministic tiebreak: longest, then
  first in file).

## The fixed action set (v1: 10 actions, all read-only)

| action        | data source                                    | answer template (plain words, always numbers) |
|---------------|------------------------------------------------|------------------------------------------------|
| disk_root     | statvfs("/")                                   | `Root disk: 5 GB total, 2.8 GB free (56% used) — fine.` (warn at ≥85%: "— getting tight") |
| memory        | /proc/meminfo MemAvailable/MemTotal            | `RAM: 3.9 GB usable, 2.1 GB free right now.` |
| profile       | /etc/jai/capabilities.json → profile + notes[0]| `Profile: minimal (1 core + 0.9 GB RAM). Below the 2 GB floor → rules-only mode; kernel reflexes stay on.` |
| accel         | capabilities.json → accel, gpu                 | `Compute: CPU (no usable GPU). Tier-1 runs on CPU.` / vulkan variant names the GPU |
| net           | capabilities.json → net.online + live check fallback (connect to the kernel-link? NO — reuse probe logic: /sys/class/net operstate) | `Offline right now — cloud tier off, local rules + local model only.` |
| uptime        | sysinfo(2)                                     | `Up 3 days 4:12, load 0.15.` |
| events_recent | tail /var/lib/jai/events.jsonl (≤5 lines, text only) | `Last events: doctor ping; gate opened.` |
| service_ssh   | systemctl is-active ssh (fork+execvp, fixed argv, no user text) | `SSH is active (running).` / `SSH is inactive.` |
| capabilities  | capabilities.json whole-file summary           | the notes[] block, verbatim — the probe already explains the machine |
| help          | none                                           | lists the questions Layer-0 knows (honesty about limits IS an answer) |

- capabilities.json unreadable/missing → profile/accel answer honestly:
  `Capabilities file missing — the probe did not run this boot?` and
  exit 1 (fall through to the brain). Never invent facts.
- Sources are the SAME primitives jai-mcp uses (statvfs, /proc, fixed
  systemctl argv) — Layer-0 is the hands' read-only knowledge baked
  into a shortcut, not a second security domain.

## Wrapper v3 — the chain becomes three-deep

    #!/bin/sh   (POSIX sh, ~25 lines, /usr/local/bin/jai)
    ans=$(jai-layer0 "$@" 2>/dev/null) && { echo "$ans"; exit 0; }
    [curl warm-brain check] && exec opencode run --server ... "$@"
    exec opencode run --standalone "$@"

- Wrapper stays dumb: Layer-0 is just the first exec. If jai-layer0
  prints anything, that IS the answer; exit 1 = silent fall-through.
- Every Layer-0 hit appends a pulse line (wrapper side):
      {"ts":"...","actor":"wrapper","via":"layer0","q":"disk?","ms":41}
  — so step 13 pulse metrics can measure the 70% claim against reality.
- `jai --brain "..."` escape hatch: skip Layer-0 on purpose (user is
  the boss — they can always demand the expensive path).

## Runtime shape

- `agent/layer0/jai-layer0.c` — C99 + json-c (capabilities.json needs
  a parser; this is why Layer-0 is C and not shell), same recipe:
  gcc -O2 -Wall -Wextra, fortify-clean, fork+execvp only (ssh action),
  fixed buffers, no popen, no shell.
- One-shot process (no daemon): invoked per question by the wrapper,
  answers, exits. Cold cost ≈ a statvfs — nothing to keep warm.
- `--rules <path>` and `JAI_ETC_DIR` overrides for TESTS only
  (same pattern as jai-capabilities).

## Acceptance criteria (all must pass to call step 6 DONE)

1. `time jai "disk?"` → template answer, total < 1 s on the 1 GB VM
   (measured with `time`, median of 5).
2. Zero tokens: run with the brain DOWN → "disk?", "profile?", "is ssh
   running?" still answer (this is THE criterion — Layer-0 works when
   everything expensive is dead).
3. `jai "what is the meaning of life"` → Layer-0 exits 1, wrapper falls
   through to the warm brain (existing golden path intact).
4. `jai --brain "disk?"` → reaches the brain even though Layer-0 knew
   the answer (escape hatch works).
5. Rules edit: add `swap<TAB>memory` → `jai "swap?"` answers memory.
   No restart exists to perform (one-shot proves config is read fresh).
6. Injected text is inert: `jai "disk?; rm -rf /"` → matches disk_root,
   the `; rm -rf /` bytes never reach any exec (audit: fixed argv).
7. Missing capabilities.json → honest "missing" answer + fall-through,
   no crash, no invented facts.
8. Pulse: each Layer-0 hit adds exactly one pulse line with ms.
9. vm-doctor.sh gains the Layer-0 check (3 golden questions, latency).
10. Warning-free build; json-c only; RSS trivial (one-shot).

## Honest limitations (stated, not hidden)

- Fixed question set. "Why is my laptop slow?" does NOT match — that
  is a real-thought question and belongs to the brain. Layer-0 is for
  facts, not diagnosis. (Fuzzy routing via tier-1 classification is a
  LATER step and stays unbuilt until the fixed set measurably misses.)
- English keywords only (v1). The template ANSWERS are plain words; a
  localized rule table is a conf file away, but not shipped v1.
- Substring matching has false-positive risk ("disk" inside a longer
  question like "which disk does the brain write lessons to?" answers
  disk facts instead of waking the brain). Accepted v1 tradeoff: the
  user has `--brain` and can always rephrase; a min-question-length or
  multi-keyword heuristic can land later WITH a doctor test each.
- One-shot per question = a fresh process each time (≈ms — fine). If
  pulse ever shows wrapper overhead mattering, the fix is a resident
  matcher, and that would need its own spec — not smuggled in.
