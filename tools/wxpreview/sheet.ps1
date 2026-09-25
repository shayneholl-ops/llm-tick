# Render every Vancouver background variant (day + night) and assemble a contact
# sheet.  Row 1 = daytime, row 2 = night; columns 0..5 are
# lions-gate, anchorage, skyline, seaplane, orca, blossom.
#   pwsh -File tools/wxpreview/sheet.ps1 [-Fam 5] [-T 12345] [-Scale 2]
param([int]$Fam = 5, [int]$T = 12345, [int]$Scale = 2)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here "wxpreview.exe"
$ff   = "C:\Users\hxp-n\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.2-full_build\bin\ffmpeg.exe"
$out  = "C:\Users\hxp-n\llm-tick-diag"

$files = @()
foreach ($day in 1, 0) {
  foreach ($bg in 0..5) {
    $p = Join-Path $here ("v{0}_{1}.ppm" -f $day, $bg)
    & $exe $Fam $day $T $p $bg | Out-Null
    $files += $p
  }
}

# inputs 0-5 = day, 6-11 = night
$args = @()
foreach ($p in $files) { $args += @("-i", $p) }
$fc = "[0][1][2][3][4][5]hstack=inputs=6[d];[6][7][8][9][10][11]hstack=inputs=6[n];[d][n]vstack=inputs=2,scale=iw*${Scale}:ih*${Scale}:flags=neighbor"
$args += @("-filter_complex", $fc)
$sheet = Join-Path $out "vancouver-variants.png"
& $ff @args -y $sheet 2>&1 | Select-Object -Last 3
Write-Host "sheet -> $sheet"
