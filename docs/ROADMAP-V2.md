# JAI OS Roadmap v2 (unified: system + learning + product)

Rule: system first, product second; every step has acceptance criteria.

| #  | Step                                   | Track   | Status | Done when |
|----|----------------------------------------|---------|--------|-----------|
| 0  | Revoke leaked token; VM 2G + 512M swap | base    | OPEN   | free -m shows 2G+swap; token dead |
| 1  | 6a warm brain (unit + wrapper v2)      | system  | BUILD BLOCK SENT | BRAIN-UP on boot; kill -9 -> 3s recovery; cold fallback proven |
| 2  | Tier-1 pilot (Qwen3-0.6B, llama.cpp)   | system  | IN PROGRESS (host) | {"i":N} JSON in ~1 s from llama-server |
| 3  | 4b decoder + jai_events tool           | system  | BLOCKED (needs: 254B struct, jai-mcp.c tail, phase2.sh) | decoded.jsonl populated; brain can read events |
| 4  | 2b lessons (jai_lesson_write, cap 50)  | learning| after 3 | lessons.md grows; read-first verified |
| 5  | 6b jai-gate (classes+budget)           | system  | after 3+4 | Class 2 wakes budgeted; 0/1 free |
| 6  | Layer-0 fast path (rules+MCP answers)  | system  | after 5 | "disk?" answered <1 s, 0 tokens |
| 7  | M1 .deb packages (dkms module, eventd, mcp, agent) | product | after 6 | apt install jai-os works on clean 24.04 |
| 8  | M2 installer (autoinstall seed / ISO)  | product | after 7 | bare-metal install without manual SSH |
| 9  | M3 config TUI + read-only REST API     | product | after 8 | jai-config works; API serves status |
| 10 | M4 web/desktop UI (off-box)            | product | parallel | dashboard shows pulse, gate, lessons |
| 11 | M5 signed apt repo + update channel    | product | later | upgrades with rollback |
| 12 | Reflex proposals + jai-review          | learning| later | brain proposes, human approves, gate hot-reloads |
| 13 | Pulse metrics everywhere               | system  | cheap  | pulse.jsonl counts thoughts/quota/latency |

Dependency chain: 0 -> 1 -> (3 -> 4 -> 5 -> 6) -> 7 -> 8 -> 9 -> 10 -> 11.
Tier-1 pilot (2) is independent and unblocked NOW.

Next unblocked actions (owner, on Kali):
1. Step 0 (token revoke + 2G bump)
2. Step 1 (run 6a block inside VM, paste outputs)
3. Step 2 (finish llama-server build + test, paste timing)
4. Paste the three 4b inputs (event struct, jai-mcp.c tail, phase2.sh)
