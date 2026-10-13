# capture-boot-rev.ps1 — one-shot: open COM4, DTR low, RTS-pulse reset (chip
# reboots into the APP), then capture ~45 s of boot serial to a log file.
# Usage: pwsh -File tools\capture-boot-rev.ps1 [seconds] [logfile]
param(
    [int]$Secs = 45,
    [string]$Log = 'C:\Users\hxp-n\llm-tick-repo\tools\boot-rev-serial-log.txt'
)

$sp = New-Object System.IO.Ports.SerialPort 'COM4', 115200, 'None', 8, 'Parity.None', 'StopBits.One'
$sp.ReadTimeout = 200
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()

Add-Content -Path $Log -Value "`n===== capture start $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (RTS reset, DTR low) ====="
# RTS pulse at DTR low = app boot (established kill-switch behaviour)
$sp.RtsEnable = $true
Start-Sleep -Milliseconds 150
$sp.RtsEnable = $false
Add-Content -Path $Log -Value '===== RTS reset pulse sent ====='

$end = (Get-Date).AddSeconds($Secs)
while ((Get-Date) -lt $end) {
    try {
        $line = $sp.ReadLine()
        if ($line) { Add-Content -Path $Log -Value $line }
    } catch [System.TimeoutException] { }
}
$sp.Close()
Add-Content -Path $Log -Value '===== capture end ====='
Write-Output "wrote $Log"
