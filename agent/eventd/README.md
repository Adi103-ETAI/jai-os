# jai-eventd - the reflex arc of JAI (Phase 3c)

The daemon that turns `/dev/jai` from "something the agent can *pull*"
into a stream the agent can *read back*: kernel events land in
`/var/lib/jai/events.jsonl` the moment they happen, and the brain answers
"what happened lately?" through the `jai_events` MCP tool - no shell, no
guessing, no human asking first.

## Why a drain-poller (and not a blocking reader)

`jai_dev.c` v0.2.0 semantics make the classic design impossible:

- `read()` **drains** ring bytes oldest-first, and when the ring is empty
  it returns the two-line STATUS frame (`"JAI is awake. ..."`) and then
  EOF. **The device never blocks.** A reader that sits in `read()` would
  instead spin on status frames.
- So `jai-eventd` is a drain-poller: open → drain → close → sleep 500 ms.
  Stateless per tick, tolerant of module reloads (failed open = retry).

## Overflow detection (the part that makes it honest)

The status frame carries a kernel-side monotonic counter:
`events: N total`. eventd diffs "accepted by kernel" vs "captured by me":

- `accepted > captured` → the 4 KiB ring overwrote oldest bytes between
  polls → a `{"gap":K,"note":"~K events lost to ring overflow"}` record
  is written. **The brain sees the hole in the telemetry instead of
  assuming a complete stream.**
- counter went *down* → module was reloaded; new baseline, noted on stderr.

## File format

```json
{"ts":"2026-11-02T10:05:12.480Z","seq":1,"text":"gate opened"}
{"ts":"...","seq":9,"gap":3,"note":"~3 events lost to ring overflow"}
```

Every record is built with json-c (house rule: this file faces untrusted
event text). One open/append/close per record - logrotate `copytruncate`
needs no daemon signaling (policy in `rootfs/overlay/etc/logrotate.d/jai`).

## v0.2.0 protocol limits (accepted, documented)

- An event beginning `"JAI is awake."` is indistinguishable from a status
  frame. Don't write those; `jai_dev` v0.3 would add real framing if it
  ever matters.
- Embedded newlines inside one event appear as several records → gap
  accounting is approximate (counts lines, not events).

## Deploy

| Path | What |
|---|---|
| `agent/eventd/jai-eventd.c` | the daemon (this dir) |
| `rootfs/overlay/etc/systemd/system/jai-eventd.service` | sandboxed unit (User=jai, ProtectSystem=strict, Restart=always) |
| `rootfs/overlay/etc/logrotate.d/jai` | rotation policy |
| `rootfs/overlay/usr/local/bin/jai-heartbeat-beat` | heartbeat enrichment (event delta per beat) |
| `scripts/vm-eventd.sh` | live-VM installer (no re-bake needed) |
| `scripts/phase2.sh` step 5n | bake into future images |

```bash
# in the VM:
curl -fsSL https://raw.githubusercontent.com/Adi103-ETAI/jai-os/main/scripts/vm-eventd.sh | sudo bash
```

## Acceptance (define it before you run it)

1. `systemctl status jai-eventd` → active (running), survives reboot
2. `echo "reflex test" > /dev/jai && sleep 1 && tail -1 /var/lib/jai/events.jsonl`
   → one fresh record containing `reflex test`
3. `jai "Use jai_events to report the last 3 kernel events."` → the brain
   cites the measured records via the hands, no shell fallback
4. beat check: wait for a heartbeat tick → `events_delta=` appears in
   `/var/lib/jai/heartbeat.log`
