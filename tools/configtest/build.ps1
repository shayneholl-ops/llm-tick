# Build + run the host test for the Config seam (plain g++, no board).
#
# The Config seam is deliberately a separate translation unit with NO Arduino
# networking includes, so it can be compiled and tested on the host. That is what
# lets us pin down the "unset vs empty" distinction without flashing.
#
#   pwsh -File tools\configtest\build.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
$src  = Join-Path $root 'src'

# Factory defaults for the TEST build. These are literals, deliberately: the test has
# no secrets.h, so the -D list is the test's own default set rather than a duplicate of
# what config_defaults.h produces. The IP carries SPACES on purpose — that is the shape
# a real secrets.h used to produce via CFG_STR of a comma-separated macro, and the
# parser must keep tolerating it now that secrets.h emits dots.
$defines = @(
  '-DCFG_HOST_TEST=1',
  '-DCFG_DEF_WIFI_SSID="factory-ssid"',
  '-DCFG_DEF_WIFI_PASS="factory-pass"',
  '-DCFG_DEF_SERVER_HOST="factory-host"',
  '-DCFG_DEF_SERVER_IP="192, 168, 1, 99"',
  '-DCFG_DEF_SERVER_PORT=8266',
  '-DCFG_DEF_WEATHER_LOCATION="factory-place"'
)

Write-Output "[configtest] compiling..."
& g++ -std=gnu++17 -O1 -Wall -Wextra -I $src -I $here `
  $defines `
  (Join-Path $here 'config_test.cpp') `
  (Join-Path $src  'config.cpp') `
  -o (Join-Path $here 'config_test.exe')
if ($LASTEXITCODE -ne 0) { Write-Error "compile failed"; exit 1 }

Write-Output "[configtest] running...`n"
& (Join-Path $here 'config_test.exe')
exit $LASTEXITCODE
