# One-click deploy: compile payload -> push -> restart game -> inject.
# Usage: powershell -File deploy.ps1 [-Device emulator-5554]
# Safety: verifies emulator is online and exactly one device is connected.
param(
    [string]$Device = "",
    [int]$BootTimeoutSec = 60
)
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$tool = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin"
$adb = "D:\leidian\LDPlayer9\adb.exe"
$payloadSrc = Join-Path $projectRoot "src\payload_phase_a13.cpp"
$payloadOut = Join-Path $projectRoot "build\staging\liba9tas_payload_a13.so"
$pkg = "com.aligames.kuang.kybc.aligames"

function Invoke-Checked {
    param([scriptblock]$Block, [string]$Description)
    & $Block
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed (exit=$LASTEXITCODE)"
    }
}

# --- Step 0: Verify emulator is online and device is unique ---
Write-Host "[0/6] verifying device connectivity..."
$devices = & $adb devices 2>$null | Select-String "device$"
if ($devices.Count -eq 0) {
    Write-Host "    waiting for emulator (up to ${BootTimeoutSec}s)..."
    $waited = 0
    while ($waited -lt $BootTimeoutSec) {
        Start-Sleep -Seconds 2
        $waited += 2
        $devices = & $adb devices 2>$null | Select-String "device$"
        if ($devices.Count -gt 0) { break }
    }
}
if ($devices.Count -eq 0) {
    throw "No ADB device found. Start LDPlayer first."
}
if ($devices.Count -gt 1 -and $Device -eq "") {
    throw "Multiple ADB devices detected ($($devices.Count)). Specify -Device <id>."
}

if ($Device -ne "") {
    $deviceId = $Device
    $match = $devices | Where-Object { $_.ToString() -match "^$([regex]::Escape($deviceId))\s+device$" }
    if (-not $match) {
        throw "Requested device '$deviceId' is not in adb devices output."
    }
} else {
    $deviceLine = $devices[0].ToString().Trim()
    $deviceId = ($deviceLine -split "\s+")[0]
}
Write-Host "    device=$deviceId online"

# --- Step 1: Compile payload ---
Write-Host "[1/6] compiling payload..."
$compiler = Join-Path $tool "aarch64-linux-android24-clang++.cmd"
$compileArgs = @(
    "-shared", "-fPIC", "-O2", "-std=c++20",
    "-static-libstdc++", "-fvisibility=hidden",
    "-fno-exceptions", "-fno-rtti",
    "-Wl,--build-id=sha1", "-Wl,--gc-sections",
    "-llog", "-ldl",
    $payloadSrc, "-o", $payloadOut
)
& $compiler @compileArgs
if ($LASTEXITCODE -ne 0) { throw "compile failed (exit=$LASTEXITCODE)" }
$hash = (Get-FileHash $payloadOut -Algorithm SHA256).Hash.ToLower()
Write-Host "    hash=$hash"

# --- Step 2: Push payload ---
Write-Host "[2/6] pushing payload..."
& $adb -s $deviceId push $payloadOut /data/local/tmp/liba9tas_payload.so
if ($LASTEXITCODE -ne 0) { throw "push failed (exit=$LASTEXITCODE)" }

# --- Step 3: Clean control files ---
Write-Host "[3/6] cleaning control files..."
& $adb -s $deviceId shell "su -c 'chmod 644 /data/local/tmp/liba9tas_payload.so; rm -f /data/local/tmp/a9tas-record-ctl /data/local/tmp/a9tas-inject-seq /data/local/tmp/a9tas-sample-ctl /data/local/tmp/a9tas-inject-key /data/local/tmp/a9tas-dev-sample-ctl /data/local/tmp/a9tas-interval-override'"
if ($LASTEXITCODE -ne 0) { throw "cleanup failed (exit=$LASTEXITCODE)" }

# --- Step 4: Restart game ---
Write-Host "[4/6] clearing logcat and restarting game..."
& $adb -s $deviceId logcat -c
if ($LASTEXITCODE -ne 0) { throw "logcat clear failed (exit=$LASTEXITCODE)" }
& $adb -s $deviceId shell "am force-stop $pkg"
if ($LASTEXITCODE -ne 0) { throw "force-stop failed (exit=$LASTEXITCODE)" }
Start-Sleep -Seconds 1
& $adb -s $deviceId shell "am start -n $pkg/$pkg.MainActivity"
if ($LASTEXITCODE -ne 0) { throw "am start failed (exit=$LASTEXITCODE)" }

# --- Step 5: Inject bootstrap ---
Write-Host "[5/6] injecting bootstrap..."
& $adb -s $deviceId shell "su -c '/data/local/tmp/a9tas_injector --wait $pkg /data/local/tmp/liba9tas_bootstrap.so'"
if ($LASTEXITCODE -ne 0) { throw "inject failed (exit=$LASTEXITCODE)" }

# --- Step 6: Status ---
Write-Host "[6/6] deployed. Wait ~115s for the payload worker, then check:"
Write-Host "  $adb -s $deviceId shell su -c 'cat /data/user/0/$pkg/files/a9tas-status'"
Write-Host "  $adb -s $deviceId logcat -s A9TAS_PAYLOAD"
