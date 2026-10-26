# jai-gateway

Phase 4 component: voice / IO front-end that speaks to opencode's HTTP API
at http://127.0.0.1:4096. Planned stack (all RAM-light, event-driven):

- wake word:  openWakeWord (CPU, ~100 MB)
- STT:        whisper.cpp base/small, runs only after wake word fires
- TTS:        Piper (fast on 2 cores)
- transport:  WebSocket to jai-agent; Tailscale for remote sessions

Placeholder until Phase 4.
