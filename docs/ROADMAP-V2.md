# JAI OS Roadmap v2 (unified: system + learning + product)

Rule: system first, product second; every step has acceptance criteria.

| #  | Step                                   | Track   | Status | Done when |
|----|----------------------------------------|---------|--------|-----------|
| 0  | Revoke leaked token; VM 2G + 512M swap | base    | OPEN ⚠ token still alive (2026-11-04) | free -m shows 2G+swap; token dead |
| 1  | 6a warm brain (unit + wrapper v2)      | system  | BUILD BLOCK SENT | BRAIN-UP on boot; kill -9 -> 3s recovery; cold fallback proven |
| 2  | Tier-1 serving (Qwen3-0.6B, Ollama)    | system  | LIVE ON GPU ✅ (2026-11-04) | gate JSON round-trip via 10.0.2.2:11434 |
| 2a | Tier-1 GPU offload (Vulkan iGPU unlock)| system  | DONE 2026-11-04 ✅ | ollama ps = 100% GPU; survives reboot ✅ |
| 2b | jai-capabilities probe (measure machine → profile) | system | CODE LANDED (2026-11-04) — VM test pending | spec = CAPABILITIES-SPEC.md; capabilities.json at boot; minimal/balanced/full picks itself |
| 3  | 4b decoder + jai_events tool           | system  | BLOCKED (needs: 254B struct, jai-mcp.c tail, phase2.sh) | decoded.jsonl populated; brain can read events |
| 4  | lessons loop (jai_lesson_write, cap 50) | learning| after 3 | lessons.md grows; read-first verified |
| 5  | 6b jai-gate (classes+budget)           | system  | after 3+4 | Class 2 wakes budgeted; 0/1 free |
| 6  | Layer-0 fast path (rules+MCP answers)  | system  | after 5 | "disk?" answered <1 s, 0 tokens |
| 7  | M1 .deb packages (dkms module, eventd, mcp, agent) | product | after 6 | apt install jai-os works on clean 24.04 |
| 8  | M2 installer (autoinstall seed / ISO)  | product | after 7 | bare-metal install without manual SSH |
| 9  | M3 config TUI + read-only REST API     | product | after 8 | jai-config works; API serves status |
| 10 | M4 desktop integration: DE + JAI assistant woven in (on-box UI) | product | after 9 | JAI OS boots to a desktop; assistant answers/helps in plain words |
| 11 | M5 signed apt repo + update channel    | product | later | upgrades with rollback |
| 12 | Reflex proposals + jai-review          | learning| later | brain proposes, human approves, gate hot-reloads |
| 13 | Pulse metrics everywhere               | system  | cheap  | pulse.jsonl counts thoughts/quota/latency |

Dependency chain: 0 -> 1 -> (3 -> 4 -> 5 -> 6) -> 7 -> 8 -> 9 -> 10 -> 11.
Tier-1 serving (2) is LIVE on GPU; only the gate round-trip (2's done-when)
remains, behind OLLAMA_HOST=0.0.0.0.
Product direction (VISION.md): hardware-boundless desktop OS — Law 1 =
adapt + degrade, never require; Law 2 = user is the boss.

Next unblocked actions (owner, on Kali):
1. Step 0 — revoke the token (STILL pending) + VM 2G/swap bump
2. Expose Ollama to the VM: OLLAMA_HOST=0.0.0.0:11434 drop-in; test
   10.0.2.2:11434 from inside the VM
3. Step 1 (run 6a block inside VM, paste outputs)
4. Paste the three 4b inputs (event struct, jai-mcp.c tail, phase2.sh)
5. 2b: review done → code LANDED (probe + unit + phase2 5o +
   vm-capabilities.sh + doctor check). Run in the VM:
   `curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-capabilities.sh | sudo bash`
   then vm-doctor.sh → capabilities.json valid + profile sane = 2b DONE
