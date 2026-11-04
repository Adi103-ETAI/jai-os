# Tier-1 Quick Judge — small local model (LIVE on GPU)

Goal: free, unlimited, ~1 s classification/judgment so cloud quota is
spent only on real thinking (Layer 2), never on triage.

Status 2026-11-04: **SERVING LIVE — 100% GPU** (Ollama + Vulkan on the
host's Vega iGPU). llama.cpp standalone is RETIRED. Full unlock story:
[GPU-ACCEL.md](GPU-ACCEL.md).

## Stack decision — why Ollama replaced llama.cpp

|               | llama.cpp (old plan)        | Ollama (shipped)              |
|---------------|-----------------------------|-------------------------------|
| Install       | cmake + manual compile      | systemd service, one script   |
| Model mgmt    | manual GGUF + curl -fL trap | `ollama pull qwen3:0.6b`      |
| GPU           | manual backend choice       | CUDA/ROCm/Vulkan auto-discovery |
| Ops           | hand-run process            | systemd unit + drop-ins       |
| Why it died   | build broke; CPU appetite dented SSH | —                    |

## Serving layout (Kali host)

    host (bare metal, 9.7 GB RAM, headless)
      ollama.service → qwen3:0.6b · 926 MB · 100% GPU (Vulkan/RADV RAVEN2)
                       keep_alive 24h · context 4096
      binds 127.0.0.1:11434        (TODO: 0.0.0.0 for VM access — GPU-ACCEL.md)
    VM → host over slirp: http://10.0.2.2:11434

Ollama exposes BOTH the native `/api/chat` and the OpenAI-compatible
`/v1/chat/completions` — the gate keeps its original JSON contract; only
the URL/port changes.

## Runbook (host)

    ollama ps                       # PROCESSOR must say 100% GPU
    ollama run qwen3:0.6b "hi"      # smoke test
    curl -s http://127.0.0.1:11434/v1/chat/completions \
      -H 'Content-Type: application/json' \
      -d '{"model":"qwen3:0.6b","temperature":0,"max_tokens":300,"messages":[{"role":"user","content":"Rate importance 0-2. Reply JSON only: {\"i\":N}. Event: disk 92% full on /"}]}'

PASS = JSON like `{"i":2}` back in ~1 s, PROCESSOR stays GPU.

## Integration spec (Phase 6b) — unchanged except URL

- Gate → POST `http://10.0.2.2:11434/v1/chat/completions` from the VM,
  `http://127.0.0.1:11434/...` for host-side tests (needs
  `OLLAMA_HOST=0.0.0.0:11434` first — see GPU-ACCEL.md TODO)
- max_tokens 300; temperature 0; require JSON; timeout 3 s
- Failure → static severity table (never block; tier-1 is an
  accelerator, not a dependency)

## Lessons (accumulated, still true)

1. cmake missing → build silently never ran (llama.cpp era).
2. `curl` without `-f` saved a 15-byte error page AS the model file.
   Always: `curl -fL` + size check.
3. `vulkaninfo` permission-denied / llvmpipe over SSH = red herring
   (SSH session lacks render group + seat). The service user's view is
   what counts.
4. The one-line fix was `OLLAMA_IGPU_ENABLE=1` — Ollama parks iGPUs by
   default and TELLS you in the journal. Read the logs before hacking.
