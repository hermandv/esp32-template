# Run the milestone-2 network tests with this PC joined to the board's config AP, then return to the normal network.
# The PC is offline (no internet) while the tests run. The normal network is reconnected in `finally`, even on errors.
#
#   . C:\projects\esp-idf\export-v6.0.3.ps1
#   powershell -File test\hil\run_via_ap.ps1 -Mac 7c:df:a1:f2:50:30 -HomeSsid 10X
param(
    [Parameter(Mandatory)] [string] $Mac,
    [Parameter(Mandatory)] [string] $HomeSsid,
    [string] $ApPassword = "configure",
    [ValidateSet("recovery", "app")] [string] $Target = "recovery",
    [string] $Script = "hil_m2_net.py",
    [string] $Tests = ""
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$py = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
$id = ($Mac -replace "[^0-9a-fA-F]", "").Substring(6).ToUpper()
$apSsid = if ($Target -eq "app") { "ESP32S3-CFG-$id" } else { "ESP32S3-REC-$id" }
$profileXml = Join-Path $env:TEMP "devlink_ap_profile.xml"

function Log($m) { Write-Output ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $m) }

# 1. Board into recovery over USB while still online.
Log "putting the board into $Target"
& $py -c "import sys; sys.path[:0]=[r'$root\test\hil', r'$root\tools']; import devlink_tools as dl; from hil_m1 import Board; from hil_m2 import enter_recovery; b=Board(dl.find_port(None, '$Mac')); b.con.open(); b.con.drain(0.5); enter_recovery(b) if '$Target' == 'recovery' else b.ensure_app_normal()"

try {
    # 2. Temporary profile for the board's AP (manual connect, no auto-switch), then connect.
    @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$apSsid</name>
  <SSIDConfig><SSID><name>$apSsid</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$ApPassword</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>
"@ | Set-Content -Encoding UTF8 $profileXml
    netsh wlan add profile filename="$profileXml" user=current | Out-Null
    Log "connecting to $apSsid"
    netsh wlan connect name="$apSsid" ssid="$apSsid" | Out-Null

    $ok = $false
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        $ip = (Get-NetIPAddress -InterfaceAlias Wi-Fi -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress
        if ($ip -like "192.168.4.*") { $ok = $true; break }
    }
    if (-not $ok) {
        Log "FAILED: no 192.168.4.x address on Wi-Fi (got '$ip')"
    } else {
        Log "joined $apSsid as $ip"
        # 3. Tests.
        $pyArgs = @("-u", "$root\test\hil\$Script", "--mac", $Mac, "--url", "http://192.168.4.1", "--ap", $apSsid)
        if ($Tests) { $pyArgs += $Tests.Split(" ") }
        & $py @pyArgs
        Log "tests exit code $LASTEXITCODE"
    }
}
finally {
    # 4. Back to the normal network, remove the temporary profile.
    Log "reconnecting to $HomeSsid"
    netsh wlan disconnect | Out-Null
    netsh wlan delete profile name="$apSsid" | Out-Null
    Remove-Item $profileXml -ErrorAction SilentlyContinue
    netsh wlan connect name="$HomeSsid" | Out-Null
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 500
        $ip = (Get-NetIPAddress -InterfaceAlias Wi-Fi -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress
        if ($ip -and ($ip -notlike "192.168.4.*") -and ($ip -notlike "169.254.*")) { break }
    }
    Log "Wi-Fi now: $ip"
}
