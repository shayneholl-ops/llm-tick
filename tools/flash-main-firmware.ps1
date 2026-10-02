# flash-main-firmware.ps1 — flash the main llm-tick firmware (the quiet one)
# over the ROM download port, replacing whatever is on the board right now
# (e.g. the spike v3 self-test that auto-types "hello from llm-tick usb").
#
# Why not `pio run -t upload`: the board currently enumerates as a TinyUSB
# composite (CDC console + HID keyboard) under ARDUINO_USB_MODE=0, so its app
# CDC port cannot re-enter download mode. The ROM USB-Serial-JTAG port appears
# only while BOOT is held at reset — exactly what this script waits for.
#
# Usage:  pwsh -File tools\flash-main-firmware.ps1
#   1. Note the ports listed under "current ports" (should be everything but
#      the board's JTAG port).
#   2. Hold BOOT (GPIO0), tap RESET once, then release BOOT.  A NEW COM port
#      (ROM USB-Serial/JTAG) must appear — the script polls 40 s for it.
#   3. esptool writes bootloader + partitions + boot_app0 + firmware.
#   4. Unplug/replug the cable (or tap RESET) to boot the fresh firmware.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$esp = 'C:\Users\hxp-n\.espressif\python_env\idf5.5_py3.11_env\Scripts\esptool.exe'
$build = Join-Path $root '.pio\build\esp32s3'
$bootApp0 = Join-Path $env:USERPROFILE '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'

$needed = @{
  esptool     = $esp
  bootloader  = Join-Path $build 'bootloader.bin'
  partitions  = Join-Path $build 'partitions.bin'
  firmware    = Join-Path $build 'firmware.bin'
  boot_app0   = $bootApp0
}
foreach ($k in $needed.Keys) {
  if (-not (Test-Path $needed[$k])) { Write-Error "missing $k at $($needed[$k])"; exit 1 }
}
Write-Output "[flash] esptool = $esp"

function Get-Ports { [System.IO.Ports.SerialPort]::GetPortNames() }

$before = @(Get-Ports)
Write-Output "[flash] current ports: $(if ($before) { $before -join ', ' } else { '(none)' })"
Write-Output "[flash] >>> Hold BOOT, tap RESET once, release BOOT — I am waiting for the ROM download port (up to 60 s)."
Write-Output "[flash] (port changes appear below as they happen; the ROM JTAG port is a NEW COM that was not listed above)"

$port = $null
$lastSeen = @(Get-Ports)
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
  $now = @(Get-Ports)
  if (($now -join ',') -ne ($lastSeen -join ',')) {
    Write-Output "[flash] ports now: $(if ($now) { $now -join ', ' } else { '(none)' }) (was: $($lastSeen -join ', '))"
  }
  $lastSeen = $now
  foreach ($p in $now) { if ($before -notcontains $p) { $port = $p; break } }
  if ($port) { break }
  Start-Sleep -Milliseconds 400
}
if (-not $port) { Write-Error "no new COM port appeared in 60 s. Re-check: BOOT must be HELD while RESET is tapped, then released. If a port flashed by but esptool missed it, just re-run; the board is still in the old firmware."; exit 2 }
Write-Output "[flash] ROM download port found: $port"

# --- flash all four images at their huge_app.csv offsets ---
# NOTE (2026-10-02): do NOT add --flash_mode / --flash_freq / --flash_size here. On this
# unit those flags OVERRIDE the image header rather than acting as a no-op: a requested
# `--flash_mode qio` landed on flash as 0x00 (DIO) and `--flash_size 16MB` rewrote the
# size nibble, leaving a bootloader whose mode byte disagreed with the build — the board
# then sat in ROM download mode. A plain write_flash writes the header byte-for-byte
# correctly (verified by read-back: 0xE9 0x03 0x02 0x3F == bootloader.bin).
& $esp --chip esp32s3 --port $port --baud 921600 write_flash `
   0x0      $needed.bootloader `
   0x8000   $needed.partitions `
   0xe000   $needed.boot_app0 `
   0x10000  $needed.firmware
if ($LASTEXITCODE -ne 0) { Write-Error "esptool failed (exit $LASTEXITCODE)"; exit 3 }
Write-Output "[flash] verifying all four images..."
& $esp --chip esp32s3 --port $port --baud 921600 verify_flash `
   0x0      $needed.bootloader `
   0x8000   $needed.partitions `
   0xe000   $needed.boot_app0 `
   0x10000  $needed.firmware

Write-Output "[flash] DONE — firmware written + verified."
Write-Output "[flash] NOTE: the S3 may stay in ROM download mode; the app can take ~20 s to print"
Write-Output "[flash]       '[tick] WiFi UP' on its console. LISTEN before assuming failure."
Write-Output "[flash] Console is COM4 under ARDUINO_USB_MODE=1: python tools/monitor-com.py COM4"