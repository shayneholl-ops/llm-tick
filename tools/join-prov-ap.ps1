# Join the Board's Setup AP from the PC. RZ616 needs a few attempts; that is the
# adapter, not the Board. The WPA2 profile is created once if missing.
$ErrorActionPreference = 'Continue'

if (-not (netsh wlan show profiles | Select-String 'llm-tick-prov')) {
    $xml = @"
<?xml version="1.0"?>
<wlan-profile xmlns="http://schemas.microsoft.com/WiFi/configuration/2011-01">
  <name>llm-tick-prov</name>
  <SSIDString>llm-tick-prov</SSIDString>
  <connectionType>ESS</connectionType>
  <autoStart>false</autoStart>
  <wlan>
    <auth>WPA2</auth>
    <encryption>CCMP</encryption>
    <keyUsage>usage</keyUsage>
    <sharedKey index="1">
      <keyType>passPhrase</keyType>
      <passPhrase>setup1234</passPhrase>
    </sharedKey>
  </wlan>
</wlan-profile>
"@
    $xml | Out-File -Encoding UTF8 "$env:TEMP\llm-tick-prov-profile.xml"
    netsh wlan add profile filename="$env:TEMP\llm-tick-prov-profile.xml" name="all user profiles" | Out-Null
}

$connected = $false
for ($i = 1; $i -le 4; $i++) {
    netsh wlan connect name=llm-tick-prov 2>&1 | Out-Null
    Start-Sleep 3
    $st = netsh wlan show interfaces | Select-String '^\s*State'
    if ($st -match 'connected') {
        Write-Host "CONNECTED on attempt $i"
        $connected = $true
        break
    }
    Write-Host "attempt $i : not connected yet"
}
if (-not $connected) {
    Write-Host "NOT CONNECTED after 4 attempts (adapter flakiness, not a Board fault)"
    exit 1
}
netsh wlan show interfaces | Select-String 'SSID|State|Auth'
