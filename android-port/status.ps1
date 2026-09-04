# Quick status + one-shot sequence test flow.
# Usage:
#   powershell -File status.ps1              # show payload status
#   powershell -File status.ps1 -Seq seq.txt # push a sequence and arm it
#   powershell -File status.ps1 -Watch       # refresh every 2 seconds
#   powershell -File status.ps1 -Watch 5     # refresh every 5 seconds
param(
    [string]$Seq = "",
    [switch]$Watch,
    [int]$Interval = 2,
    [string]$Device = ""
)
$adb = "D:\leidian\LDPlayer9\adb.exe"
$deviceId = "emulator-5554"
$st = "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-status"
$pkg = "com.aligames.kuang.kybc.aligames"

# Resolve device selection.
$devices = & $adb devices 2>$null | Select-String "\sdevice$"
if ($Device -ne "") {
    $deviceId = $Device
    $match = $devices | Where-Object { $_.ToString() -match "^$([regex]::Escape($deviceId))\s+device$" }
    if (-not $match) {
        throw "Requested device '$deviceId' is not in adb devices output."
    }
} elseif ($devices.Count -eq 1) {
    $deviceId = ($devices[0].ToString().Trim() -split "\s+")[0]
} elseif ($devices.Count -gt 1) {
    throw "Multiple ADB devices detected. Please pass -Device <id>."
} else {
    throw "No ADB device found. Start LDPlayer first or pass -Device <id>."
}

if ($Interval -lt 1) {
    throw "Interval must be at least 1 second."
}

if ($Seq -ne "") {
    & $adb -s $deviceId push $Seq /data/local/tmp/a9tas-inject-seq | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "seq push failed (exit=$LASTEXITCODE)" }
    Write-Host "seq pushed: $Seq" -ForegroundColor Green
}

if ($Watch) {
    Write-Host "Watching $deviceId (Ctrl+C to stop)..." -ForegroundColor Cyan
    while ($true) {
        $ts = Get-Date -Format "HH:mm:ss"
        $status = & $adb -s $deviceId shell "su -c 'cat $st'" 2>$null
        Write-Host "[$ts] $status"
        Start-Sleep -Seconds $Interval
    }
} else {
    & $adb -s $deviceId shell "su -c 'cat $st'"
}
