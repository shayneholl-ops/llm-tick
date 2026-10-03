# Build + run the host QR test against the REAL library source (plain g++, no board).
#
#   pwsh -File tools\qrtest\build.ps1
#
# Proves the "WIFI:" join URI encodes, which version fits, and how many px/module the
# 172-px-wide Panel can afford — before any Panel drawing code depends on it.

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)

# The library source lives in the PlatformIO libdeps tree after `pio pkg install`.
$lib = Get-ChildItem (Join-Path $root '.pio\libdeps') -Recurse -Directory -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -eq 'QRCode' } | Select-Object -First 1
if (-not $lib) { Write-Error "QRCode lib not found under .pio/libdeps - run: pio pkg install"; exit 1 }
$src = Join-Path $lib.FullName 'src'
if (-not (Test-Path (Join-Path $src 'qrcode.c'))) { $src = $lib.FullName }

Write-Output "[qrtest] library: $src"
& g++ -std=gnu++17 -O1 -Wall -I $src `
  (Join-Path $here 'qrtest.cpp') `
  (Join-Path $src  'qrcode.c') `
  -o (Join-Path $here 'qrtest.exe')
if ($LASTEXITCODE -ne 0) { Write-Error "compile failed"; exit 1 }

& (Join-Path $here 'qrtest.exe')
exit $LASTEXITCODE
