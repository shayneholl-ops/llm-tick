$esp = "C:\Users\hxp-n\.espressif\python_env\idf5.5_py3.11_env\Scripts\esptool.exe"
$port = $args[0]
$out = ".scratch\board_app.bin"
Write-Output "Reading app partition (0x10000, 0x300000) from $port ..."
& $esp -p $port --before no_reset --after no_reset read_flash 0x10000 0x300000 $out
Write-Output "read exit: $LASTEXITCODE"
if (Test-Path $out) {
  $bh = (Get-FileHash $out -Algorithm MD5).Hash
  $ph = (Get-Content .scratch/pio_firmware.md5)
  Write-Output "BOARD_APP_MD5=$bh"
  Write-Output "PIO_BUILD_MD5 =$ph"
  Write-Output "MATCH=$(if($bh -eq $ph){'YES — board runs the built bridge firmware'}else{'NO — board does NOT run current build'})"
  # also check bridge strings in the read-back
  $bytes=[System.IO.File]::ReadAllBytes((Resolve-Path $out))
  $txt=[System.Text.Encoding]::ASCII.GetString($bytes)
  Write-Output "readback contains 'NimBLE init done': $($txt.Contains('NimBLE init done'))"
  Write-Output "readback contains '[ble] state': $($txt.Contains('[ble] state'))"
}
