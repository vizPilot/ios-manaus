# M6b boot #4: flash the iOS 16 HFS+ root image WITH /etc/fstab to userdata.
#
# DESTRUCTIVE: rewrites the whole userdata partition (GPT untouched).  Run only
# with explicit user approval and the phone in fastboot.  boot_a/vendor_boot_a
# already hold the M6b boot #3 images, so no boot re-flash is needed.
#
#   powershell -File tools/flash-userdata-manaus.ps1
#
# The image is an Android sparse image whose FILL chunks were split below 4 GiB
# (tools/breakfills.py) because the 32-bit MTK bootloader's write_sparse_image
# overflows on any single chunk > 4 GiB.
$ErrorActionPreference = 'Continue'
$fb     = "D:\platform-tools_r34.0.5-windows\platform-tools\fastboot.exe"
$img    = "D:\manaus\ios16-root\ios16-root-20H392-manaus-fstab-bf.simg"
$expect = "0dd03312e862a5decc364e31407fed9b97ca8e0aa5bcd3107d245f568d5f3f16"
$expectSize = 12145759360

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

Write-Host "=== flash userdata (fstab fix, FILL chunks split below 4 GiB) ==="
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $fb flash userdata $img 2>&1
$code = $LASTEXITCODE
$sw.Stop()
Write-Host ("=== exit={0} elapsed={1} ===" -f $code, $sw.Elapsed)
