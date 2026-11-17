# M6b boot #6: flash the iOS 16 HFS+ root image WITH the full SWEET bring-up
# recipe (tools/sweet-root-tweaks.py: migration skip, containers, launchd
# _PanicOnCrash rename + CommCenter removal, Setup skip, BKNoWatchdogs,
# VirtualAudio off, MediaToolbox StartRecorderServer off).
#
# DESTRUCTIVE: rewrites the whole userdata partition (GPT untouched).  Run only
# with explicit user approval and the phone in fastboot.  boot_a/vendor_boot_a
# already hold the M6b boot #5 images, so no boot re-flash is needed.
#
#   powershell -File D:\manaus\flash-tweaks.ps1
$ErrorActionPreference = 'Continue'
$fb     = "D:\platform-tools_r34.0.5-windows\platform-tools\fastboot.exe"
$img    = "D:\manaus\ios16-root\ios16-root-20H392-manaus-tweaks-bf.simg"
$expect = "2d35601cb701766fcca8b9846246ddd93f408d406b6b4b8d38837935bd911d1e"
$expectSize = 12145771676

Write-Host "=== device ==="
& $fb devices 2>&1
& $fb getvar current-slot 2>&1

Write-Host "=== image ==="
$i = Get-Item $img
Write-Host ("{0}  {1:N0} bytes" -f $i.FullName, $i.Length)
if ($i.Length -ne $expectSize) {
    Write-Host ("SIZE MISMATCH (expected {0:N0}) -- aborting" -f $expectSize) -ForegroundColor Red
    exit 1
}

Write-Host "=== sha256 (reads ~12 GB, ~1 min) ==="
$h = (Get-FileHash -Algorithm SHA256 $img).Hash.ToLower()
Write-Host "sha256=$h"
if ($h -ne $expect) {
    Write-Host "SHA256 MISMATCH (expected $expect) -- aborting" -ForegroundColor Red
    exit 1
}

Write-Host "=== flash userdata (sweet bring-up recipe, FILL chunks split below 4 GiB) ==="
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $fb flash userdata $img 2>&1
$code = $LASTEXITCODE
$sw.Stop()
Write-Host ("=== exit={0} elapsed={1} ===" -f $code, $sw.Elapsed)
