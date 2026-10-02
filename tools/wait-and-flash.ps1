# wait-and-flash.ps1 — flash the llm-tick firmware, with a LONG window and live
# port-change reporting so a BOOT+RESET can be done at leisure.
#
# Usage:  pwsh -File tools\wait-and-flash.ps1 [-WaitSeconds 180] [-Port COM4]
#
# Prints every COM-port change as it happens. When a port that was not present at
# start appears, it flashes all four images to it.

param(
  [int]$WaitSeconds = 180,
  [string]$PrefPort = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$esp  = 'C:\Users\hxp-n\.espressif\python_env\idf5.5_py3.11_env\Scripts\esptool.exe'
$build = Join-Path $root '.pio\build\esp32s3'
$bootApp0 = Join-Path $env:USERPROFILE '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'

$imgs = @{
  bootloader = Join-Path $build 'bootloader.bin'
  partitions = Join-Path $build 'partitions.bin'
  firmware   = Join-Path $build 'firmware.bin'
  boot_app0  = $bootApp0
}
foreach ($k in $imgs.Keys) {
  if (-not (Test-Path $imgs[$k])) { Write-Error "missing $k at $($imgs[$k])"; exit 1 }
}

function Get-Ports { @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object) }

$before = Get-Ports
Write-Output "[flash] ports at start: $(if ($before) { $before -join ', ' } else { '(none)' })"
Write-Output "[flash] waiting up to $WaitSeconds s for a NEW port (the ROM USB-Serial/JTAG port)."
Write-Output "[flash] >>> HOLD BOOT, TAP RESET, RELEASE BOOT."
if ($PrefPort) { Write-Output "[flash] (will prefer $PrefPort if it appears)" }
Write-Output ""

$port = $null
$last = $before
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while ((Get-Date) -lt $deadline) {
  $now = Get-Ports
  if (($now -join ',') -ne ($last -join ',')) {
    $added   = $now | Where-Object { $last -notcontains $_ }
    $removed = $last | Where-Object { $now -notcontains $_ }
    $msg = "[flash] t+{0,3}s ports: {1}" -f [int]((Get-Date) - $deadline.AddSeconds($WaitSeconds)).TotalSeconds, ($now -join ', ')
    if ($added)   { $msg += "   (+$($added -join ',+'))" }
    if ($removed) { $msg += "   (-$($removed -join ',-'))" }
    Write-Output $msg
    if ($PrefPort -and $now -contains $PrefPort) { $port = $PrefPort; break }
    if ($added) { $port = $added[0]; break }
  }
  $last = $now
  Start-Sleep -Milliseconds 250
}

if (-not $port) {
  Write-Error "no new COM port in $WaitSeconds s. The board is unchanged. See HANDOFF item 26."
  exit 2
}

Write-Output ""
Write-Output "[flash] ROM port = $port  -> writing 4 images"
& $esp --chip esp32s3 --port $port --baud 921600 write_flash --flash_mode qio --flash_freq 80m `
    0x0     $imgs.bootloader `
    0x8000  $imgs.partitions `
    0xe000  $imgs.boot_app0 `
    0x10000 $imgs.firmware
if ($LASTEXITCODE -ne 0) { Write-Error "esptool failed (exit $LASTEXITCODE)"; exit 3 }
Write-Output "[flash] DONE. Unplug/replug USB-C (or tap RESET) to boot the new firmware."
