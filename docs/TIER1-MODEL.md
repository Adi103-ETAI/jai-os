# Tier-1 Quick Judge — small local model plan

Goal: free, unlimited, ~1 s classification/judgment so cloud quota is
spent only on real thinking (Layer 2), never on triage.

## Model choice

| Candidate          | Reality check                       | Verdict      |
|--------------------|-------------------------------------|--------------|
| "Qwen3.5 0.8B"     | name drift; real = Qwen3-0.6B       | WINNER       |
| "Gemma 4 E2B"      | real = Gemma 3n E2B, ~2 GB RAM      | heavier      |
| Qwen3 0.6B         | smallest practical                  | same family  |
| Llama 3.2 1B       | decent                              | fine         |

Pick: Qwen3-0.6B, GGUF Q4_K_M (~450-600 MB) via llama.cpp.
RAM: ~0.8 GB (model + context). Home = Kali host (7 GB free);
VM reaches host at 10.0.2.2 (QEMU user networking). VM hosting
possible after -m 2G bump, for testing only.

## Pilot runbook (Kali host)

    cd ~/llama.cpp
    cmake -B build && cmake --build build -j --target llama-server
    curl -fL -o qwen3-0.6b-q4.gguf \
      https://huggingface.co/lmstudio-community/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q4_K_M.gguf
    ls -lh qwen3-0.6b-q4.gguf    # ~450-600 MB; 15 bytes = wrong URL, use -f!
    ./build/bin/llama-server -m qwen3-0.6b-q4.gguf --host 127.0.0.1 --port 8081

    # second terminal — the exact gate job:
    curl -s http://127.0.0.1:8081/v1/chat/completions \
      -H 'Content-Type: application/json' \
      -d '{"messages":[{"role":"user","content":"Rate importance 0-2. Reply JSON only: {\"i\":N}. Event: disk 92% full on /"}]}'

PASS = JSON like {"i":2} returned in ~1 s.

## Integration spec (Phase 6b)

- Gate -> POST http://10.0.2.2:8081/v1/chat/completions (or 127.0.0.1 in VM tests)
- max_tokens: 300; temperature: 0; require JSON output (llama.cpp grammar)
- Timeout 3 s -> on failure, fall back to static severity table (never block)
- Known-good backup URLs:
  bartowski/Qwen_Qwen3-0.6B-GGUF  (file: Qwen_Qwen3-0.6B-Q4_K_M.gguf)
  official listing: https://huggingface.co/api/models/Qwen/Qwen3-0.6B-GGUF/tree/main

## Lessons learned during first pilot

1. cmake was missing -> build silently never ran; always re-run after install.
2. curl without -f saved a 15-byte "Entry not found" body AS the model file.
   Always: curl -fL + ls -lh size check before use.
