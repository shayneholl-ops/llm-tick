# Ground-truth BLE advertisement sniffer using the host Windows Bluetooth adapter.
# Captures each advertisement packet relayed by the Watcher and prints ADV type, MAC,
# flags, name, HID-service presence, and the raw LEGACY_IND/EXT flags — so we can see
# exactly how (or whether) the Keychron broadcasts and why the ESP32 scanner misses it.
param(
  [int]$Seconds = 20,
  [string]$Match = 'Keychron'
)

Add-Type -AssemblyName System.Runtime.WindowsRuntime -ErrorAction Stop

# Helper to call awaitable WinRT methods via the AsTask interop.
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
  $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1
})[0]

# Async method invoker
function Await([object]$op) {
  $task = $asTaskGeneric.MakeGenericMethod([bool]).Invoke($null, @($op))
  $task.Wait() | Out-Null
  $task.Result
}

# Load WinRT namespaces
$null = [Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]
$null = [Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType=WindowsRuntime]

$watcher = New-Object Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher
$watcher.ScanningMode = [Windows.Devices.Bluetooth.Advertisement.BluetoothLEScanningMode]::Active

# Attach the Received handler. WinRT event needs a delegate; use the supported pattern:
Register-ObjectEvent -InputObject $watcher -EventName Received -Action {
  # $EventArgs is BluetoothLEAdvertisementReceivedEventArgs
  $args = $EventArgs
  $adv  = $args.Advertisement
  $mac  = $args.BluetoothAddress.ToString('X12')
  $rssi = $args.RawSignalStrengthInDBm
  $type = $args.AdvertisementType

  $name = $adv.LocalName
  $svcs = @()
  foreach ($s in $adv.ServiceUuids) { $svcs += $s.ToString() }
  $hid = if ($svcs -contains '00001812-0000-1000-8000-00805f9b34fb') { 'HID' } else { 'no-hid' }

  $line = "[sniff] {0} rssi={1} type={2} name='{3}' svcs=[{4}] {5}" -f `
    $mac, $rssi, $type, $name, ($svcs -join ','), $hid

  $matchOk = $Match -eq '' -or $name -like "*$Match*"
  if ($matchOk) {
    Write-Output $line
    # print raw adv bytes if present
    try {
      $raw = [System.BitConverter]::ToString($adv.GetManufacturerData()) 
      if ($raw) { Write-Output ("         mfrbytes: " + $raw) }
    } catch {}
  }
} | Out-Null

Write-Output "Sniffing BLE ads for $Seconds s (match '$Match') ..."
$watcher.Start()
Start-Sleep -Seconds $Seconds
$watcher.Stop()
Write-Output 'Done sniffing.'
