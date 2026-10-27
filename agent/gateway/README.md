# jai-gateway

Phase 4 component: voice / IO front-end that speaks to opencode's HTTP API
at http://127.0.0.1:4096. Planned stack (all RAM-light, event-driven):

- wake word:  openWakeWord (CPU, ~100 MB)
- STT:        whisper.cpp base/small, runs only after wake word fires
- TTS:        Piper (fast on 2 cores)
- transport:  WebSocket to jai-agent; Tailscale for remote sessions

## OpenCode v2 note (verified on 2.2.0)

- default port unchanged: `http://127.0.0.1:4096`
- `serve --service` still exists → jai-agent.service stays valid
- **NEW:** v2 `serve` prints a one-time **server password** to stdout at
  startup (→ systemd journal of jai-agent). The gateway must authenticate
  with it. Options when we build Phase 4: read it from the journal,
  or check for a config/env way to set a fixed credential.

Placeholder until Phase 4.
