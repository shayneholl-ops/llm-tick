# Build + run the host test for the provisioning FORM (plain g++, no board).
#
# src/provform.cpp is a separate translation unit with NO Arduino includes, for the same
# reason src/config.cpp is: the rules worth testing here (unset vs empty, the password
# never reaching the page, a refused submit storing nothing) are pure logic, and testing
# them off-Board turns a 20-minute flash cycle into a two-second one.
#
#   pwsh -File tools\provformtest\build.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$src  = Join-Path $root 'src'

# Same Factory defaults as the configtest harness, so both suites agree on what an
# UNSET field resolves to. cfgTestWipe() returns every field to these.
$defines = @(
  '-DCFG_HOST_TEST=1',
  '-DCFG_DEF_WIFI_SSID="factory-ssid"',
  '-DCFG_DEF_WIFI_PASS="factory-pass"',
  '-DCFG_DEF_SERVER_HOST="factory-host"',
  '-DCFG_DEF_SERVER_IP="192, 168, 1, 99"',
  '-DCFG_DEF_SERVER_PORT=8266',
  '-DCFG_DEF_WEATHER_LOCATION="factory-place"'
)

Write-Output "[provformtest] compiling..."
& g++ -std=gnu++17 -O1 -Wall -Wextra -I $src -I $here `
  $defines `
  (Join-Path $here 'provform_test.cpp') `
  (Join-Path $src  'provform.cpp') `
  (Join-Path $src  'config.cpp') `
  -o (Join-Path $here 'provform_test.exe')
if ($LASTEXITCODE -ne 0) { Write-Error "compile failed"; exit 1 }

Write-Output "[provformtest] running...`n"
& (Join-Path $here 'provform_test.exe')
exit $LASTEXITCODE