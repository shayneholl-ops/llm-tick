# Build + run the offline wxScene preview (host g++, no board needed).
#   pwsh -File tools/wxpreview/build.ps1 [-Fam 5] [-Day 0] [-T 12345]
param([int]$Fam = 5, [int]$Day = 0, [int]$T = 12345)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here "..\..")
Copy-Item (Join-Path $root "src\wxscene.cpp") (Join-Path $here "wxscene_gen.cpp") -Force
& g++ -std=gnu++17 -O1 -I $here -I (Join-Path $root "src") `
    (Join-Path $here "wxscene_gen.cpp") (Join-Path $here "main.cpp") `
    -o (Join-Path $here "wxpreview.exe")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $here "wxpreview.exe") $Fam $Day $T (Join-Path $here "wx.ppm")
