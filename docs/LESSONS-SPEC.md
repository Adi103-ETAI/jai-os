# The lessons loop — jai_lesson_write + read-first (roadmap step 4)

> Spec BEFORE code. This is the contract for how JAI learns across
> thoughts without a database, without a daemon, and without ever
> letting the brain overwrite itself into stupidity. Depends on: the
> hands (jai-mcp v1.1.0, SHIPPED). Unblocks: jai-gate (step 5 reads
> lessons as part of its context budget).

## What it is, in plain words

After every real thought, the brain may write down ONE distilled line:
what happened, what worked, what to do differently. Next time it thinks,
it reads those lines FIRST. That is the whole loop:

    thought ends
       │ brain picks the ONE thing worth remembering
       ▼
    jai_lesson_write("vulkan tier-1: check ollama ps before blaming the model")
       │ MCP tool (the hands) — appends, dates, caps
       ▼
    /var/lib/jai/brain/lessons.md          (max 50 lines)
       │
       ▼ next thought begins
    brain reads lessons.md FIRST (built-in file read)
       ▼
    better thought → maybe a better lesson → ...

No database. No daemon. A markdown file and one MCP tool. ARCHITECTURE-V2
§6 calls this the learning loop; the cap (50) is what keeps it a loop
and not a landfill.

## The two halves — write goes through the hands, read is just reading

| Direction | Mechanism | Why |
|-----------|-----------|-----|
| WRITE     | new MCP tool `jai_lesson_write(text)` in jai-mcp | the hands enforce the cap, the one-line format, the charset — the brain CANNOT bypass them (no shell) |
| READ      | the brain reads `/var/lib/jai/brain/lessons.md` directly with its built-in file-read tool | read needs no protection — it's a file the jai user owns; adding a read tool would be ceremony |

`jai_lesson_write` is the ONLY new tool in this step. jai-mcp goes
v1.1.0 → v1.2.0 (additive; tool count 11 → 12).

## Tool contract — jai_lesson_write

Request: `{"tool":"jai_lesson_write","args":{"text":"<1..200 chars>"}}`

Behavior, in order:
1. **Charset allowlist** (house rule, same as every jai-mcp arg):
   printable ASCII minus control chars; newline/CR/tab → REFUSED
   (one lesson = one line, always).
2. **Length**: 1..200 chars after trim. Longer → REFUSED with the
   message "distill it; a lesson is one line".
3. **Date prefix**: tool prepends `YYYY-MM-DD: ` itself — the brain is
   never trusted to date things.
4. **Exact-duplicate check**: same final line already present → OK
   response `"duplicate; not appended"` (a no-op is a success — the
   brain should not be punished for re-learning the same thing).
5. **Cap enforcement — REFUSE, never drop**: at 50 lines → REFUSED with
   "lessons full (50) — read lessons.md, consolidate, and rewrite the
   weakest lines" . Deleting/merging is the brain's job to PROPOSE
   (it cannot delete files either — consolidation happens when a human
   or a future maintenance step trims; v1 keeps write-append as the
   only mutation).
6. **Atomic append** (O_APPEND single write ≤ 512 B — no torn lines),
   file 0644, dir owned by jai.

Response: `{"ok":true,"lines":N}` or a structured refusal (never a crash).

## File contract — /var/lib/jai/brain/lessons.md

    # JAI lessons (max 50, newest last)   <- header line, tool-managed
    2026-11-05: cold fallback works; warm-first saved 45 s on repeat ask
    2026-11-05: ollama on 10.0.2.2:11434 answers /api/tags in 3 ms

- Location is under `/var/lib/jai/brain/` — the brain's ONLY writable
  area (ARCHITECTURE-V2 §8 safety model, already enforced by where the
  hands allow writes).
- Header line is not a lesson; cap counts lesson lines only (49 free
  slots after the header).
- The header line + cap line make `wc -l` a doctor-able invariant:
  `lessons_lines <= 51`.

## Read-first — how "READ FIRST" actually happens (honest version)

There is no kernel hook for "read your lessons". Read-first is PROMPT
DISCIPLINE in the brain's system context (opencode AGENTS.md /
instructions file, landed by phase2 step alongside the tool):

    BEFORE answering anything non-trivial: read
    /var/lib/jai/brain/lessons.md. It is your own memory. Ignore it
    and you repeat your mistakes.

This is honestly a convention, not an enforcement — accepted for v1
because (a) the brain is pinned and prompted by us, (b) pulse.jsonl
(step 13) will show whether thoughts that skip lessons cost more,
(c) the alternative (injecting lessons into every prompt from the
wrapper) changes prompt shapes and is deferred until measured pain.

## What is explicitly OUT of scope (and where it went)

| Not here                       | Lives in                     |
|--------------------------------|------------------------------|
| proposals.jsonl (brain proposes gate rules as data) | step 12 (jai-review) — needs the gate to exist first |
| jai-review approval flow       | step 12                      |
| lesson decay/scoring           | later; cap-50-refuse forces the brain to self-curate first |
| cross-machine lessons          | never (lessons are machine-local truth) |

## Acceptance criteria (all must pass to call step 4 DONE)

1. `jai_lesson_write` with "test lesson" → line `2026-11-05: test lesson`
   appended; response `{"ok":true,"lines":N}`.
2. Text with an embedded newline → REFUSED, file unchanged.
3. 201-char text → REFUSED with the distill message.
4. Exact duplicate → OK, file unchanged, "duplicate" in response.
5. Fill to 50 → lesson 51 REFUSED with the consolidate message; file
   still exactly 50 lessons.
6. Reboot → file persists, tool stateless, nothing to re-init.
7. Read-first scenario (doctor): write lesson "the magic word is
   ARTICHOKE" → ask the brain via `jai` wrapper → answer mentions
   ARTICHOKE. This is THE acceptance proof that the loop is closed.
8. vm-doctor.sh gains the lessons check (tool answers, cap holds).
9. jai-mcp builds warning-free (fortify clean), no new dependencies.

## Honest limitations (stated, not hidden)

- Read-first is prompt discipline — a hallucinating brain can skip it.
  The doctor scenario (criterion 7) is the canary; pulse metrics will
  make skipping measurable later.
- ASCII-only lessons (charset allowlist). Non-English lessons need a
  charset v2 decision — deliberately deferred; the brain thinks in the
  user's language but STORES machine-notes in ASCII v1.
- 50 lines ≈ 6-7 KB — bounded context cost by construction. If 50
  proves too tight, the cap is a one-number change WITH a doctor check
  update, not a redesign.
- One writer at a time (the brain is one process; gate never writes
  lessons). No locking in v1 — serialized by design.
