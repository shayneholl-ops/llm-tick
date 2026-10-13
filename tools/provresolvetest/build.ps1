# Build + run the host test for the ticket #9 resolve seam (plain g++, no board).
#
# src/provresolve.cpp is a separate translation unit with NO board includes — the
# decision (no-such-place vs service-unreachable vs a name to echo) is pure logic,
# and testing it off-Board turns a flash cycle into a two-second one.
#
#   pwsh -File tools\provresolvetest\build.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$src  = Join-Path $root 'src'
# The libdeps package root only holds a thin ArduinoJson.h shim; the real
# headers (ArduinoJson/Array/...) live in the package's src/ subdirectory.
$json = Join-Path $root '.pio\libdeps\esp32s3\ArduinoJson\src'

if (-not (Test-Path $json)) { Write-Error "ArduinoJson not found at $json (run a pio build once)"; exit 1 }

Write-Output "[provresolvetest] compiling..."
& g++ -std=gnu++17 -O1 -Wall -Wextra -I $src -I $json -I $here `
  (Join-Path $here 'provresolve_test.cpp') `
  (Join-Path $src  'provresolve.cpp') `
  -o (Join-Path $here 'provresolve_test.exe')
if ($LASTEXITCODE -ne 0) { Write-Error "compile failed"; exit 1 }

Write-Output "[provresolvetest] running...`n"
& (Join-Path $here 'provresolve_test.exe')
exit $LASTEXITCODE
