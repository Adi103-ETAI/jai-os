# jai-gateway

Phase 4 component: voice / IO front-end that speaks to opencode's HTTP API
on the VM/host loopback. Planned stack (all RAM-light, event-driven):

- wake word:  openWakeWord (CPU, ~100 MB)
- STT:        whisper.cpp base/small, runs only after wake word fires
- TTS:        Piper (fast on 2 cores)
- transport:  WebSocket to jai-agent; Tailscale for remote sessions

## OpenCode v2 server contract (empirically verified on 2.2.0, 2026-10-27)

- `serve` binds a RANDOM port when unconfigured (first VM boot picked
  49374, NOT 4096). phase2.sh pins it via v2's own managed-service CLI:
  `opencode service set port 4096` → written to
  `/var/lib/jai/.config/opencode/service.json` (HOME=/var/lib/jai).
  Valid keys: disabled | remote | hostname | port | password | cors | env.
- The JSON API ALWAYS requires auth — even on 127.0.0.1 (no-auth → 401
  `{"_tag":"UnauthorizedError"}`).
- Auth scheme = HTTP **Basic**, any username, the pinned password:
  `curl -u opencode:jai http://127.0.0.1:4096/openapi.json` → 200
  (Bearer is NOT accepted; browsers get a cookie path automatically).
- `service set password <pw>` pins the credential (v2 otherwise
  auto-generates one and stores it in the same file). phase2.sh pins
  `jai` by default — override at bake time with
  `sudo env JAI_SERVER_PASSWORD=<secret> ./scripts/phase2.sh`.
- `serve --service` unchanged → jai-agent.service stays valid.
- Human-friendly docs UI: `http://127.0.0.1:4096/doc` (OpenAPI JSON at
  `/openapi.json`).

Gateway TODO (Phase 4): read the password from
`/var/lib/jai/.config/opencode/service.json` at runtime — single source of
truth, never hardcode it twice. Replace the dev password `jai` with a
generated secret before Phase 5 (real hardware).

Placeholder until Phase 4.
