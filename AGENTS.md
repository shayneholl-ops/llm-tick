# llm-tick — agent notes

A Waveshare **ESP32-S3-LCD-1.47B** displaying live LLM token usage. See `README.md`
for hardware truth and `HANDOFF.md` for the current state of the board; `CONTEXT.md`
holds the project glossary — use its vocabulary.

## Agent skills

### Issue tracker

Issues live as GitHub issues on `shayneholl-ops/llm-tick`, with native blocking
dependencies. **The repo is PUBLIC — sanitise network details before publishing.**
See `docs/agents/issue-tracker.md`.

### Triage labels

Five canonical roles (`needs-triage`, `needs-info`, `ready-for-agent`,
`ready-for-human`, `wontfix`). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `CONTEXT.md` at the repo root, decisions in `docs/adr/`, specs in
`docs/spec/`. See `docs/agents/domain.md`.

## Repo-specific gotchas

These have each cost real time; don't rediscover them.

- **Flashing**: never pass `--flash_mode` / `--flash_size` / `--flash_freq` to esptool.
  On this unit they *override* the image header rather than acting as a no-op, and the
  board then sits in ROM download mode. A plain `write_flash` writes it correctly.
- **A silent serial port is not a failed flash.** After flashing, the S3 can take
  ~20 s to print `[tick] WiFi UP`. Listen before reaching for a replug.
- **Console port**: the app console is the USB-Serial/JTAG port under
  `ARDUINO_USB_MODE=1`. It renumbers; check `[System.IO.Ports.SerialPort]::GetPortNames()`.
- **Poll the Server gently.** This board has a documented WiFi-RX heap-corruption crash
  under bursty load; keep fetches well under ~10/min. See `HANDOFF.md` item 5.
- **The Panel has per-unit quirks** (a luminance meander at one RAM-row end, a
  cool-cast backlight). `HANDOFF.md` documents the workarounds; don't "fix" them blind.
- **`secrets.h` is gitignored.** Never echo it, commit it, or paste from it.
