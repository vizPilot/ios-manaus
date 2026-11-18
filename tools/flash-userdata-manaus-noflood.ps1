# M6b boot #7: flash the iOS 16 HFS+ root image with the console-flooding daemons
# removed (option B).  Removed from launchd.plist: the whole Bluetooth family
# (bluetoothd, bluetoothuserd, BluetoothUIService, BTServer.*, BlueTool) and
# fairplayd.H2 (KeepAlive=true tight loop).  SpringBoard/backboardd/mediaserverd
# and everything else are kept, so the SpringBoard CRASH text should now stay
# visible on the panel instead of being scrolled away.
#
# DESTRUCTIVE: rewrites the whole userdata partition (GPT untouched).  Run only
# with explicit user approval and the phone in fastboot.  boot_a/vendor_boot_a
# already hold the M6b boot #5 images, so no boot re-flash is needed.
#
#   powershell -File D:\manaus\flash-noflood.ps1
$ErrorActionPreference = 'Continue'
$fb     = "D:\platform-tools_r34.0.5-windows\platform-tools\fastboot.exe"
$img    = "D:\manaus\ios16-root\ios16-root-20H392-manaus-noflood-bf.simg"
$expect = "3d9fde991f5e95af0973ef42b83cffde7690108a2b92118400c1c18041873869"
$expectSize = 12145771676

Write-Host "=== device ==="
& $fb devices 2>&1
& $fb getvar current-slot 2>&1

Write-Host "=== image ==="
$i = Get-Item $img
Write-Host ("{0}  {1:N0} bytes" -f $i.FullName, $i.Length)
if ($expectSize -ne 0 -and $i.Length -ne $expectSize) {
    Write-Host ("SIZE MISMATCH (expected {0:N0}) -- aborting" -f $expectSize) -ForegroundColor Red
    exit 1
}

Write-Host "=== sha256 (reads ~12 GB, ~1 min) ==="
$h = (Get-FileHash -Algorithm SHA256 $img).Hash.ToLower()
Write-Host "sha256=$h"
if ($expect -ne "PLACEHOLDER_SHA256" -and $h -ne $expect) {
    Write-Host "SHA256 MISMATCH (expected $expect) -- aborting" -ForegroundColor Red
    exit 1
}

Write-Host "=== flash userdata (noflood root, FILL chunks split below 4 GiB) ==="
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $fb flash userdata $img 2>&1
$code = $LASTEXITCODE
$sw.Stop()
Write-Host ("=== exit={0} elapsed={1} ===" -f $code, $sw.Elapsed)
