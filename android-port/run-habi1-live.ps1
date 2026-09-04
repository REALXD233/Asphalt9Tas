param(
    [ValidateSet("OfflineValidate", "ExecuteOnce")]
    [string]$Mode = "OfflineValidate",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [ValidateRange(10000, 120000)][int]$TimeoutMs = 60000,
    [switch]$AcknowledgeDeviceAccessAndFixedArtifactPush,
    [switch]$AcknowledgeFreshDisposableGameProcess,
    [switch]$AcknowledgeNativeBridgeCallbackAndSinglePtraceCall,
    [switch]$AcknowledgeProcessTerminationOnSuccessOrUncertainty,
    [switch]$ExecuteExactlyOneAttempt
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildScript = Join-Path $root "build-hook-abi-selftest-habi1.ps1"
$baselineVerifier = Join-Path $root "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $root "baselines\known_good_900_exact_interval_v2.json"
$manifest = Join-Path $root "build\habi-1-offline\habi1_manifest_f22b4083c94fd41a.json"
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$remoteReceipt = "/data/user/0/$package/cache/a9tas-hook-abi-selftest-habi1.status"
$remoteReceiptExport = "/data/local/tmp/a9tas-hook-abi-selftest-habi1.status.export"
$remoteCarrierLog = "/data/local/tmp/a9tas-habi1-early-carrier.log"
$remoteControllerLog = "/data/local/tmp/a9tas-habi1-one-shot.log"

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments 2>&1) | Out-String).Trim()
}
function Invoke-AdbChecked([string[]]$Arguments, [string]$Label) {
    $result = & $AdbPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Label failed: $($result -join ' ')" }
    return (($result | Out-String).Trim())
}
function Invoke-Root([string]$Command, [string]$Label) {
    return Invoke-AdbChecked @('-s',$Device,'shell',"su -c '$Command'") $Label
}
function Get-GamePid {
    $value = Invoke-AdbText @('-s',$Device,'shell',"su -c 'pidof $package'")
    if ($value -match '^\d+$') { return [int]$value }
    return 0
}
function Get-StartTicks([int]$GameProcessId) {
    $stat = Invoke-Root "cat /proc/$GameProcessId/stat" "read process identity"
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed process stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Process start ticks unavailable"
    }
    return [string]$fields[19]
}
function Assert-RemoteHash([string]$Path, [string]$Expected) {
    $line = Invoke-Root "sha256sum $Path" "hash $Path"
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Path"
    }
}
function Assert-RemoteMode([string]$Path, [string]$Expected) {
    $mode = Invoke-Root "stat -c '%a' $Path" "mode $Path"
    if ($mode -ne $Expected) {
        throw "Remote mode mismatch: $Path expected=$Expected observed=$mode"
    }
}
function Wait-Until([scriptblock]$Condition, [string]$Label) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out: $Label"
}

foreach ($path in @($buildScript,$baselineVerifier,$baseline)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing HABI-1 runner input: $path"
    }
}

& $buildScript | Out-Host
if ($LASTEXITCODE -ne 0) { throw "HABI-1 unified offline build failed" }
python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "Known-good 900-frame baseline drift" }

$m = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if ($m.schema -ne 'a9tas-habi1-offline-manifest' -or $m.revision -ne 3 -or
    $m.status.dynamic -ne 'NOT_RUN' -or $m.status.device_access -ne 0) {
    throw "HABI-1 manifest is not offline-ready revision 3"
}
$payload = Join-Path $root ($m.payload.path -replace '^android-port/','')
$bootstrap = Join-Path $root ($m.bootstrap.path -replace '^android-port/','')
$carrier = Join-Path $root ($m.carrier.path -replace '^android-port/','')
$controller = Join-Path $root ($m.controller.path -replace '^android-port/','')
foreach ($entry in @(
    @($payload,$m.payload.sha256), @($bootstrap,$m.bootstrap.sha256),
    @($carrier,$m.carrier.sha256), @($controller,$m.controller.sha256))) {
    if (-not (Test-Path -LiteralPath $entry[0] -PathType Leaf) -or
        (Get-Sha $entry[0]) -ne $entry[1]) {
        throw "HABI-1 manifest artifact drift: $($entry[0])"
    }
}

if ($Mode -eq 'OfflineValidate') {
    Write-Output "HABI1_LIVE_RUNNER_OFFLINE passed=1 device_access=0 deployed=0 dynamic=NOT_RUN"
    exit 0
}

if (-not ($AcknowledgeDeviceAccessAndFixedArtifactPush -and
          $AcknowledgeFreshDisposableGameProcess -and
          $AcknowledgeNativeBridgeCallbackAndSinglePtraceCall -and
          $AcknowledgeProcessTerminationOnSuccessOrUncertainty -and
          $ExecuteExactlyOneAttempt)) {
    throw "ExecuteOnce requires all five explicit acknowledgements"
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB executable not found: $AdbPath"
}

$remotePayload = [string]$m.payload.expected_device_path
$remoteBootstrap = "/data/local/tmp/$([IO.Path]::GetFileName($bootstrap))"
$remoteCarrier = "/data/local/tmp/$([IO.Path]::GetFileName($carrier))"
$remoteController = "/data/local/tmp/$([IO.Path]::GetFileName($controller))"
$artifacts = @(
    @($payload,$remotePayload,[string]$m.payload.sha256),
    @($bootstrap,$remoteBootstrap,[string]$m.bootstrap.sha256),
    @($carrier,$remoteCarrier,[string]$m.carrier.sha256),
    @($controller,$remoteController,[string]$m.controller.sha256)
)

$devices = Invoke-AdbChecked @('devices') "enumerate ADB devices"
if ($devices -notmatch "(?m)^$([Regex]::Escape($Device))\s+device$") {
    throw "Expected ready device not found: $Device"
}
foreach ($entry in $artifacts) {
    Invoke-AdbChecked @('-s',$Device,'push',$entry[0],$entry[1]) "push $($entry[1])" | Out-Null
}
Invoke-Root "chmod 700 $remoteCarrier $remoteController; chmod 644 $remotePayload $remoteBootstrap" "set fixed permissions" | Out-Null
Assert-RemoteMode $remoteCarrier "700"
Assert-RemoteMode $remoteController "700"
Assert-RemoteMode $remotePayload "644"
Assert-RemoteMode $remoteBootstrap "644"
foreach ($entry in $artifacts) { Assert-RemoteHash $entry[1] $entry[2] }
Assert-RemoteHash ([string]$m.carrier.libc_device_path) ([string]$m.carrier.libc_sha256)

$timestamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$evidenceDir = Join-Path $root "evidence\habi1-live-$timestamp"
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null
$completed = $false
try {
    Invoke-Root "am force-stop $package" "force-stop prior game process" | Out-Null
    Wait-Until { (Get-GamePid) -eq 0 } "prior process termination"
    Invoke-Root "rm -f $remoteReceipt $remoteReceiptExport $remoteCarrierLog $remoteControllerLog" "clear HABI-1 receipts" | Out-Null
    Invoke-Root "nohup $remoteCarrier >$remoteCarrierLog 2>&1 </dev/null &" "arm fixed carrier" | Out-Null
    Invoke-Root "am start -n $activity" "start fresh disposable game" | Out-Null

    Wait-Until {
        $text = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat $remoteCarrierLog 2>/dev/null'")
        return $text -match 'HABI1_EARLY_CARRIER passed=[01]'
    } "fixed carrier completion"
    $carrierResult = Invoke-Root "cat $remoteCarrierLog" "read carrier result"
    if ($carrierResult -notmatch 'HABI1_EARLY_CARRIER passed=1') {
        throw "Fixed early carrier failed: $carrierResult"
    }

    $gameProcessId = Get-GamePid
    if ($gameProcessId -le 0) { throw "Fresh game process disappeared after carrier" }
    $startTicks = Get-StartTicks $gameProcessId
    Wait-Until {
        $maps = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat /proc/$gameProcessId/maps'")
        return $maps -match [Regex]::Escape($remotePayload)
    } "normal guest payload load"
    $nonce = [Security.Cryptography.RandomNumberGenerator]::GetInt32(1,2147483647)
    $controllerResult = Invoke-Root "$remoteController $gameProcessId $startTicks $nonce >$remoteControllerLog 2>&1; cat $remoteControllerLog" "execute one HABI-1 guest call"
    if ($controllerResult -notmatch 'HABI1_ONE_SHOT passed=1') {
        throw "HABI-1 one-shot controller failed: $controllerResult"
    }
    Wait-Until { (Get-GamePid) -eq 0 } "disposable process success termination"
    Invoke-Root "cp $remoteReceipt $remoteReceiptExport; chmod 644 $remoteReceiptExport" "export payload receipt" | Out-Null
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteCarrierLog,(Join-Path $evidenceDir 'carrier.log')) "pull carrier log" | Out-Null
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteControllerLog,(Join-Path $evidenceDir 'controller.log')) "pull controller log" | Out-Null
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteReceiptExport,(Join-Path $evidenceDir 'payload.status')) "pull payload receipt" | Out-Null
    $completed = $true
    Write-Output "HABI1_LIVE passed=1 attempts=1 guest_calls=1 process_terminated=1 evidence=$evidenceDir"
}
finally {
    if (-not $completed) {
        try { Invoke-Root "am force-stop $package" "terminate uncertain disposable process" | Out-Null } catch { }
        try { Invoke-Root "cp $remoteReceipt $remoteReceiptExport; chmod 644 $remoteReceiptExport" "export payload failure receipt" | Out-Null } catch { }
        try { Invoke-AdbChecked @('-s',$Device,'pull',$remoteCarrierLog,(Join-Path $evidenceDir 'carrier-failure.log')) "pull carrier failure log" | Out-Null } catch { }
        try { Invoke-AdbChecked @('-s',$Device,'pull',$remoteControllerLog,(Join-Path $evidenceDir 'controller-failure.log')) "pull controller failure log" | Out-Null } catch { }
        try { Invoke-AdbChecked @('-s',$Device,'pull',$remoteReceiptExport,(Join-Path $evidenceDir 'payload-failure.status')) "pull payload failure receipt" | Out-Null } catch { }
    }
}
