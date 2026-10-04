# Host build + run for the Provisioning verify seam (src/provverify.cpp).
#
# Mirrors tools\configtest\build.ps1. Deliberately does NOT link Arduino: the seam is
# Arduino-free by design, and if it ever grows an Arduino dependency this harness stops
# compiling, which is the signal that the boundary has been breached.
$ErrorActionPreference = 'Stop'
# Two levels up: tools\provverifytest -> tools -> repo root.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
Set-Location $root

$exe = 'tools\provverifytest\provverify_test.exe'
if (Test-Path $exe) { Remove-Item $exe -Force }

& g++ -std=gnu++17 -O1 -Wall -Wextra `
    -I src `
    tools\provverifytest\provverify_test.cpp `
    src\provverify.cpp `
    -o $exe
if ($LASTEXITCODE -ne 0) { Write-Error "compile failed"; exit 1 }

& ".\$exe"
exit $LASTEXITCODE