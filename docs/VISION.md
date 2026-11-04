# JAI OS — Vision (the north star)

> This document says WHAT we are building and WHY. The architecture docs
> say HOW. If any document disagrees with this one, this one wins.

## One sentence

**JAI OS is a real desktop operating system — like Windows, Ubuntu or
macOS — with an AI woven into the OS itself: it watches the machine,
explains things in plain words, fixes what is safe to fix, and helps
control the system, so things get simpler for the user — while the user
can always just type, click, and use the computer normally.**

## The two laws

### Law 1 — Boundless to hardware

JAI OS runs on ANY hardware it is given. It never REQUIRES power; it
MEASURES the machine and ADAPTS; when something is missing it DEGRADES
gracefully instead of failing.

    THE LADDER — one OS, different faces
    ┌──────────────────────────────────────────────────────────────┐
    │ FULL      16 GB+ RAM · dedicated GPU · 4+ cores              │
    │           full desktop UI · bigger local models · voice      │
    ├──────────────────────────────────────────────────────────────┤
    │ BALANCED  8 GB RAM · iGPU · 2–4 cores        (workshop rig)  │
    │           desktop UI (light) · small local model on GPU/CPU  │
    ├──────────────────────────────────────────────────────────────┤
    │ MINIMAL   2–4 GB RAM · no GPU · any CPU                      │
    │           light UI or headless · rules + tiny model          │
    │           kernel reflexes ALWAYS on — they cost almost zero  │
    └──────────────────────────────────────────────────────────────┘

How it works in practice:

1. At boot, `jai-capabilities` (planned, roadmap 2b) MEASURES the
   machine: CPU cores, RAM, GPU vendor + VRAM, disk, network → writes
   `/etc/jai/capabilities.json`.
2. The measured profile picks itself: minimal / balanced / full.
   Profile decides: which DE, which local model size, GPU offload or
   CPU, voice on/off.
3. Every AI layer has a fallback chain and none of them may block the
   user's machine. The reference pattern (already proven with tier-1):
   **GPU if present → CPU if not → static rules if the model is
   unreachable.** Nothing in the OS is allowed to be weaker than that.

The workshop laptop (AMD Athlon Silver 3050U, 2 cores, 9.7 GB RAM, no
dedicated GPU) decides what we can TEST today. It is the workshop —
NOT the ceiling of the product.

### Law 2 — The user is the boss

The AI helps control the OS; it never takes the keyboard away.

- JAI OS boots to a normal desktop. Apps, browser, terminal, settings —
  everything works like any Ubuntu.
- The assistant is an EXTRA layer: a launcher entry, a panel, a
  terminal command (`jai`), and plain-words notifications.
- Every dangerous action goes through a permission gate: the AI
  PROPOSES, the human (or a pre-approved rule) APPROVES.
- Events and log lines are DATA for the AI to think about, never
  COMMANDS to obey (prompt-injection rule).

## What the AI does for the user (concrete)

- Answers questions about the machine in plain words: "why is my laptop
  slow?", "what is using the disk?", "is ssh running?"
- Watches system health through kernel reflexes and TELLS the user
  early: "disk is 92% full — want me to clean it? [yes/no]"
- Runs maintenance through the hands (read-only tools) and proposes
  fixes through gates (write actions).
- Learns the machine over time (lessons loop) and gets better at its job.

## How we build it (one diagram)

    Ubuntu 24.04 base (borrowed — disposable, replaceable)
      + JAI layer (ours): kernel module, reflex daemon, hands, services
      + AI brain: opencode (cloud, budgeted) + Ollama (local, free)
      build: debootstrap → setup-rootfs.sh → phase2.sh → disk → boot
    DEV BENCH = QEMU/KVM VM on the workshop laptop (safe + cheap)
    PRODUCT   = real metal, any spec, boots to a desktop

## Honest map — where we are vs where we're going

- TODAY: every JAI layer is proven on the bench — kernel reflexes, the
  event stream, the C hands, the pinned brain, tier-1 on the iGPU.
  The bench is headless because the workshop laptop is headless — that
  is a fact about our workshop, not about the product.
- NEXT: capability probe + profile system (2b) → .deb packages (M1) →
  installer (M2) → desktop profile: DE + assistant woven in (M4) →
  real-hardware boots at any spec (Phase 5).

## The one rule that never bends

**Measure the machine. Adapt. Degrade gracefully. Never require.**
