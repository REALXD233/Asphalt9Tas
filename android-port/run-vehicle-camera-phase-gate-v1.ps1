# Guarded 360-frame vehicle/final-writer + RaceView phase diagnostic.
# OfflineValidate is the default and never contacts ADB.

param(
    [ValidateSet("OfflineValidate", "PrepareFreshProcess", "ExecutePhaseGate")]
    [string]$Mode = "OfflineValidate",
    [ValidateRange(30000, 180000)][int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ObjectHex = "",
    [string]$StateAddressHex = "",
    [string]$ManagerHex = "",
    [string]$ShapeHex = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [string]$OutputDirectory = "",
    [switch]$AcknowledgeFreshProcessPreload,
    [switch]$ExecuteExactlyOneAttempt,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeCurrentLifecycleAndRaceViewAddresses,
    [switch]$AcknowledgeSingleEscAndNoManualInput,
    [switch]$Acknowledge360ReplayAnd4096ReadOnlyPhaseEvents,
    [switch]$AcknowledgeFailureForceStopsFreshProcess,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$buildScript = Join-Path $root "build-vehicle-camera-phase-observer-v1.ps1"
$buildDir = Join-Path $root "build\vehicle-camera-phase-observer-v1"
$payload = Join-Path $buildDir "liba9tas_vehicle_camera_phase_observer_v1_build_only.so"
$transaction = Join-Path $buildDir "a9tas_vehicle_camera_phase_transaction_v1"
$candidate = Join-Path $buildDir "a9tas_vehicle_camera_phase_live_candidate_v1"
$bootstrap = Join-Path $buildDir "liba9tas_bootstrap_vehicle_camera_phase_v1.so"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_vehicle_camera_phase_preload_v1.sh"
$recording = Join-Path $root "evidence\a9tas_lifecycle_source_360f_20260821_203328_152.a9utk1"
$sourceReport = Join-Path $root "evidence\a9tas_lifecycle_source_360f_20260821_203328_152.a9usr5"
$target = Join-Path $root "evidence\a9tas_lifecycle_source_360f_20260821_203328_152.a9fwt1"
$sourceValidator = Join-Path $root "tools\lifecycle_source_recording_v1.py"
$steerValidator = Join-Path $root "tools\lifecycle_steer_drift_recording_v1.py"
$parser = Join-Path $root "tools\parse_unified_executor_report_v8.py"
$pairValidator = Join-Path $root "tools\validate_final_writer_report_pair_v1.py"
$actionValidator = Join-Path $root "tools\validate_action_control_replay_v1.py"
$phaseValidator = Join-Path $root "tools\validate_vehicle_camera_phase_gate_v1.py"
$phaseAnalyzer = Join-Path $root "tools\analyze_vehicle_camera_phase_events_v1.py"
$runnerPolicy = Join-Path $root "tools\test_run_vehicle_camera_phase_gate_policy_v1.py"
$receipt = Join-Path $buildDir "prepared-phase-process-v1.json"

$pins = @{
    $buildScript = "d2330dc3b678fd3ddfff9c7890027756cd49b8dbabbe54fcdcd9fe1ea21629f2"
    $payload = "c4f62383df6255299450fb8e469f7dae965b310f512b1bf57151f51cbdcb9381"
    $transaction = "df73018fddf4c52a16f4472bd76ffe8b0392b2887118f568bb7a664605123456"
    $candidate = "f25f874fc354b923362ac775892ed53b23f64d3237e9e8fb513f1f439cf41c31"
    $bootstrap = "f90a4269e28101288abc6164c2245f5151d26c000fdb2ba023e251987c4287a2"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $helper = "0d67a9e0065b6482de4c752ff65571cca1bb3f57edcdec898de92b57ac94bab3"
    $recording = "16b9411441e0c21e2b8e1166fc18385aae8f79c32a6a715cd165f0b082f9259a"
    $sourceReport = "86f201420ff72adb2a189da252b31e9e2f6150f65a5cde481c9b630cbb199410"
    $target = "eaa4df6cfd1010abcf3c36394757672bd63e53ab4a13b01c14aca0924b7a414d"
    $sourceValidator = "0185f9ceca7b720bdc6d3a9caab20a47c4dba76d272d8b59eaa8b2d0e0937b55"
    $steerValidator = "835c9e0ab3a00c00d5053456083e38b4e262edaaa37b60f30d1cd0c97836218a"
    $parser = "cd929bf2f30c3b82a886a27645d19d6068a53b1e42c9c77c9c8f96e663b71622"
    $pairValidator = "c073a373833451c93effba166de57df82b867a77c3342adcebf109e0fdc77193"
    $actionValidator = "5e59fda8ea174afc19b31272816c337b04f058497489c8f7f61415aa3adaaf02"
    $phaseValidator = "922c5c1546d326918ecf79c81b31b135420989edd688e25238c1bd578cc43c95"
    $phaseAnalyzer = "a73766380716eb9f3433599db93867bd886566c581240e7248f57bd773ba8b6c"
    $runnerPolicy = "02af9f0410e9f0ed588d51e6ff396114c322695dc494c18398a3bf8deae65a33"
}

$remotePayload = "/data/local/tmp/liba9tas_vehicle_camera_phase_observer_v1_build_only.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_vehicle_camera_phase_v1.so"
$remoteInjector = "/data/local/tmp/a9tas_injector_vehicle_camera_phase_v1"
$remoteHelper = "/data/local/tmp/run_vehicle_camera_phase_preload_v1.sh"
$remoteTransaction = "/data/local/tmp/a9tas_vehicle_camera_phase_transaction_v1"
$remoteCandidate = "/data/local/tmp/a9tas_vehicle_camera_phase_live_candidate_v1"
$writerAck = "I_ACCEPT_FINAL_WRITER_UNIFIED_REVIEW_ONLY_V1"
$phaseAck = "I_ACCEPT_VEHICLE_CAMERA_PHASE_SINGLE_SLOT_V1"
$maximumEvents = 4096

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments 2>&1) | Out-String).Trim()
}
function Invoke-AdbChecked([string[]]$Arguments, [string]$Label) {
    $result = & $AdbPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$Label failed with exit code ${exitCode}: $($result -join ' ')"
    }
    return (($result | Out-String).Trim())
}
function Get-GamePid {
    $value = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'pidof $package'")
    if ($value -match '^\d+$') { return [int]$value }
    return 0
}
function Get-StartTicks([int]$GamePid) {
    $stat = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed process stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Process start ticks unavailable"
    }
    return [string]$fields[19]
}
function Get-TracerPid([int]$GamePid) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($line -notmatch '^TracerPid:\s*(\d+)$') { throw "Malformed TracerPid" }
    return [int]$matches[1]
}
function Get-GameBaseHex([int]$GamePid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/maps'"
    $values = @(@(foreach ($line in $maps) {
        if ($line -match 'libAsphalt9\.so' -and
            $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) {
            $matches[1].ToLowerInvariant()
        }
    }) | Sort-Object -Unique)
    if ($values.Count -ne 1) { throw "Expected one game library base" }
    return [string]$values[0]
}
function Normalize-Hex([string]$Value, [string]$Label) {
    $result = $Value.Trim()
    if ($result.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
        $result = $result.Substring(2)
    }
    if ($result -notmatch '^[0-9a-fA-F]{8,16}$') { throw "$Label is invalid" }
    return $result.ToLowerInvariant()
}
function Assert-RemoteHash([string]$Path, [string]$Expected) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Path'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Path"
    }
}
function Wait-RemoteFile([string]$Path, [int]$Milliseconds) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $Path; echo `$?'")) -eq '0') {
            return $true
        }
        Start-Sleep -Milliseconds 25
    }
    return $false
}
function Start-Remote([string]$Command) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $AdbPath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in @('-s', $Device, 'shell', "su -c '$Command'")) {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    if (-not $process.Start()) { throw "Failed to start remote process" }
    return [pscustomobject]@{
        Process = $process
        Stdout = $process.StandardOutput.ReadToEndAsync()
        Stderr = $process.StandardError.ReadToEndAsync()
    }
}
function Read-RemoteResult($Handle) {
    return [pscustomobject]@{
        ExitCode = $Handle.Process.ExitCode
        Stdout = $Handle.Stdout.GetAwaiter().GetResult()
        Stderr = $Handle.Stderr.GetAwaiter().GetResult()
    }
}

foreach ($path in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Sha $path) -ne $pins[$path]) {
        throw "Missing or unreviewed phase Gate input: $path"
    }
}
python -B $sourceValidator $sourceReport $recording
if ($LASTEXITCODE -ne 0) { throw "Lifecycle source validation failed" }
python -B $steerValidator $sourceReport $recording
if ($LASTEXITCODE -ne 0) { throw "Steer/drift source validation failed" }
python -B $runnerPolicy
if ($LASTEXITCODE -ne 0) { throw "Phase Gate runner policy failed" }

if ($Mode -eq 'OfflineValidate') {
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Phase-observer build failed" }
    foreach ($path in $pins.Keys) {
        if ((Get-Sha $path) -ne $pins[$path]) { throw "Post-build pin mismatch: $path" }
    }
    Write-Output "VEHICLE_CAMERA_PHASE_RUNNER_OFFLINE passed=1 frames=360 max_events=4096 lifecycle_2_to_3=1 camera_write=0 deployed=0 device_access=0"
    return
}

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
if ((Invoke-AdbText @('devices')) -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") {
    throw "ADB device is not connected"
}

if ($Mode -eq 'PrepareFreshProcess') {
    if (-not $AcknowledgeFreshProcessPreload) { throw "Must acknowledge fresh-process preload" }
    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") 'force-stop old game' | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Phase-observer build failed" }
    foreach ($pair in @(
        @($payload, $remotePayload), @($bootstrap, $remoteBootstrap),
        @($injector, $remoteInjector), @($helper, $remoteHelper)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) 'push phase preload artifact' | Out-Null
        Assert-RemoteHash $pair[1] $pins[$pair[0]]
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteInjector $remoteHelper'") 'chmod phase preload artifacts' | Out-Null
    $preloadOut = Join-Path $buildDir 'phase-preload.stdout.txt'
    $preloadErr = Join-Path $buildDir 'phase-preload.stderr.txt'
    $preload = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $preloadOut `
        -RedirectStandardError $preloadErr
    Start-Sleep -Milliseconds 300
    Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") 'start fresh game' | Out-Null
    if (-not $preload.WaitForExit(45000)) {
        Stop-Process -Id $preload.Id -Force
        throw "Phase preload timed out"
    }
    $preloadText = (Get-Content -Raw $preloadOut) + "`n" + (Get-Content -Raw $preloadErr)
    if ($preload.ExitCode -ne 0 -or $preloadText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
        throw "Phase preload did not reach armed stage: $preloadText"
    }
    $gamePid = Get-GamePid
    if ($gamePid -le 0) { throw "Fresh game PID unavailable" }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $mapped = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
        if ($maps.Contains($remotePayload) -and $maps.Contains($remoteBootstrap)) {
            $mapped = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $mapped -or (Get-TracerPid $gamePid) -ne 0) {
        throw "Phase payload mapping or tracer proof failed"
    }
    $startTicks = Get-StartTicks $gamePid
    [ordered]@{
        version=1; device=$Device; pid=$gamePid; start_ticks=$startTicks;
        payload_sha256=$pins[$payload]; bootstrap_sha256=$pins[$bootstrap]
    } | ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
    Write-Output "VEHICLE_CAMERA_PHASE_PREPARE_PASSED pid=$gamePid start_ticks=$startTicks payload_mapped=1 TracerPid=0"
    return
}

foreach ($gate in @(
    @($ExecuteExactlyOneAttempt, 'exactly one attempt'),
    @($AcknowledgeAncientRuinsZl1Countdown3Paused, 'Ancient Ruins + ZL1 countdown-3 paused'),
    @($AcknowledgeCurrentLifecycleAndRaceViewAddresses, 'current lifecycle and RaceView manager/shape addresses'),
    @($AcknowledgeSingleEscAndNoManualInput, 'one automatic ESC and no manual input'),
    @($Acknowledge360ReplayAnd4096ReadOnlyPhaseEvents, '360 replay frames and at most 4096 read-only phase events'),
    @($AcknowledgeFailureForceStopsFreshProcess, 'force-stop fresh process after uncertain failure'),
    @($AcknowledgePtraceStallRollbackAndCrashRisk, 'ptrace stall, rollback, and crash risk')
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Prepared phase-process receipt missing" }

$object = Normalize-Hex $ObjectHex 'ObjectHex'
$state = Normalize-Hex $StateAddressHex 'StateAddressHex'
$manager = Normalize-Hex $ManagerHex 'ManagerHex'
$shape = Normalize-Hex $ShapeHex 'ShapeHex'
if ([Convert]::ToUInt64($state, 16) -ne
    [Convert]::ToUInt64($object, 16) + 0x2d8) {
    throw "StateAddressHex must equal ObjectHex + 0x2D8"
}
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid optional address" }
}
$prepared = Get-Content -Raw $receipt | ConvertFrom-Json
$gamePid = Get-GamePid
$startTicks = if ($gamePid -gt 0) { Get-StartTicks $gamePid } else { '' }
if ($prepared.version -ne 1 -or $prepared.device -ne $Device -or
    [int]$prepared.pid -ne $gamePid -or
    [string]$prepared.start_ticks -ne $startTicks -or
    $prepared.payload_sha256 -ne $pins[$payload] -or
    $prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or
    (Get-TracerPid $gamePid) -ne 0) {
    throw "Prepared process identity mismatch"
}
$maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) {
    throw "Prepared phase mappings missing"
}
$base = Get-GameBaseHex $gamePid
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
if ($OutputDirectory -eq '') { $OutputDirectory = Join-Path $root 'evidence' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$remoteRecording = "/data/local/tmp/a9tas_phase_${gamePid}_$stamp.a9utk1"
$remoteTarget = "/data/local/tmp/a9tas_phase_${gamePid}_$stamp.a9fwt1"
$remoteUnified = "/data/local/tmp/a9tas_phase_${gamePid}_$stamp.a9uer8"
$remoteWriterReport = "/data/local/tmp/a9tas_phase_${gamePid}_$stamp.a9fwr1"
$remotePhaseInstall = "/data/local/tmp/a9tas_phase_install_${gamePid}_$stamp.a9vcp1"
$remotePhaseReport = "/data/local/tmp/a9tas_phase_${gamePid}_$stamp.a9vcp1"
$writerReady = "/data/local/tmp/a9tas_final_writer_ready_$gamePid"
$localUnified = Join-Path $OutputDirectory "a9tas_phase_360f_$stamp.a9uer8"
$localWriterReport = Join-Path $OutputDirectory "a9tas_phase_360f_$stamp.a9fwr1"
$localPhaseReport = Join-Path $OutputDirectory "a9tas_phase_360f_$stamp.a9vcp1"
$localPhaseAnalysis = Join-Path $OutputDirectory "a9tas_phase_360f_$stamp.analysis.json"

$mutationStarted = $false
$success = $false
$writerHandle = $null
try {
    foreach ($pair in @(
        @($transaction, $remoteTransaction, $pins[$transaction]),
        @($candidate, $remoteCandidate, $pins[$candidate]),
        @($recording, $remoteRecording, $pins[$recording]),
        @($target, $remoteTarget, $pins[$target])
    )) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) 'push phase Gate artifact' | Out-Null
        Assert-RemoteHash $pair[1] $pair[2]
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 700 $remoteTransaction $remoteCandidate'") 'chmod phase Gate executables' | Out-Null
    $installCommand = "$remoteTransaction install $gamePid $startTicks $base $manager $shape $maximumEvents $remotePhaseInstall $phaseAck"
    # The install transaction configures payload memory before changing the
    # RaceView slot.  Treat the process as mutated before launching it so every
    # partial failure retires this exact fresh process.
    $mutationStarted = $true
    $installText = Invoke-AdbChecked @('-s', $Device, 'shell', "su -c '$installCommand'") 'install RaceView phase slot'
    if ($installText -notmatch 'VEHICLE_CAMERA_PHASE_TRANSACTION action=1 ' -or
        (Get-TracerPid $gamePid) -ne 0) {
        throw "RaceView phase-slot install proof failed: $installText"
    }
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $remotePhaseInstall'") 'remove phase install receipt' | Out-Null

    $writerCommand = "$remoteCandidate $gamePid $base $TimeoutMs $startTicks $remoteRecording $remoteTarget $remoteUnified $remoteWriterReport $writerAck $object $state $PhysicsContextHex $MainObjectHex $FinalOwnerHex"
    $writerHandle = Start-Remote $writerCommand
    if (-not (Wait-RemoteFile $writerReady 20000)) { throw "Phase final writer did not reach armed READY" }
    $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $writerReady'")
    if ($readyText -notmatch '^READY_ARMED_RACE_LIFECYCLE_FINAL_WRITER_V1 ' -or
        $readyText -notmatch 'state=2 .*all_target_threads_frozen=1 host_resume_gate=marker_removal$' -or
        $readyText -notmatch 'controller_pid=(\d+)') {
        throw "Phase final-writer READY proof mismatch: $readyText"
    }
    $writerTracer = [int]$matches[1]
    if ((Get-TracerPid $gamePid) -ne $writerTracer) { throw "Writer tracer identity mismatch" }
    $esc = Start-Process -FilePath $AdbPath `
        -ArgumentList @('-s', $Device, 'shell', 'input', 'keyevent', '111') `
        -WindowStyle Hidden -PassThru
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $writerReady'") 'release phase writer' | Out-Null
    if (-not $esc.WaitForExit(10000) -or $esc.ExitCode -ne 0) { throw "Single resume ESC failed" }
    if (-not $writerHandle.Process.WaitForExit($TimeoutMs + 15000)) { throw "Phase final writer timed out" }
    $writerResult = Read-RemoteResult $writerHandle
    if ($writerResult.Stdout) { $writerResult.Stdout.TrimEnd() | Write-Host }
    if ($writerResult.Stderr) { $writerResult.Stderr.TrimEnd() | Write-Warning }
    if ($writerResult.ExitCode -ne 0 -or
        $writerResult.Stdout -notmatch 'AUTHORITATIVE_RACE_START_V1 state=3 events=1' -or
        (Get-TracerPid $gamePid) -ne 0) {
        throw "Lifecycle-bound phase replay failed closed"
    }

    $finalizeCommand = "$remoteTransaction finalize $gamePid $startTicks $base $manager $shape $maximumEvents $remotePhaseReport $phaseAck"
    $finalizeText = Invoke-AdbChecked @('-s', $Device, 'shell', "su -c '$finalizeCommand'") 'finalize RaceView phase slot'
    if ($finalizeText -notmatch 'VEHICLE_CAMERA_PHASE_TRANSACTION action=3 ' -or
        (Get-TracerPid $gamePid) -ne 0) {
        throw "RaceView phase-slot finalization proof failed: $finalizeText"
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 644 $remoteUnified $remoteWriterReport $remotePhaseReport'") 'chmod phase reports' | Out-Null
    foreach ($pair in @(
        @($remoteUnified, $localUnified),
        @($remoteWriterReport, $localWriterReport),
        @($remotePhaseReport, $localPhaseReport)
    )) {
        Invoke-AdbChecked @('-s', $Device, 'pull', $pair[0], $pair[1]) 'pull phase Gate report' | Out-Null
    }
    python -B $parser $localUnified
    if ($LASTEXITCODE -ne 0) { throw "Phase A9UER8 validation failed" }
    python -B $pairValidator $localUnified $localWriterReport $target $payload
    if ($LASTEXITCODE -ne 0) { throw "Phase final-writer pair validation failed" }
    python -B $actionValidator $recording $localUnified
    if ($LASTEXITCODE -ne 0) { throw "Phase exact steer/drift replay validation failed" }
    python -B $phaseValidator $localPhaseReport --frames 360 --maximum-events $maximumEvents
    if ($LASTEXITCODE -ne 0) { throw "Phase event transaction validation failed" }
    python -B $phaseAnalyzer $localPhaseReport --transaction-report --output $localPhaseAnalysis
    if ($LASTEXITCODE -ne 0) { throw "Phase event analysis failed" }
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks -or
        (Get-TracerPid $gamePid) -ne 0) {
        throw "Phase Gate final process identity failed"
    }
    Remove-Item -LiteralPath $receipt -Force
    $success = $true
    Write-Output "VEHICLE_CAMERA_PHASE_LIVE_PASSED frames=360 max_events=4096 lifecycle_events=1 camera_slot_restored=1 TracerPid=0"
    Write-Output "Unified report: $localUnified"
    Write-Output "Final-writer report: $localWriterReport"
    Write-Output "Phase report: $localPhaseReport"
    Write-Output "Phase analysis: $localPhaseAnalysis"
} finally {
    & $AdbPath -s $Device shell "su -c 'rm -f $writerReady'" 2>$null | Out-Null
    if (-not $success -and $mutationStarted) {
        & $AdbPath -s $Device shell "am force-stop $package" 2>$null | Out-Null
        if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt -Force }
        Write-Warning "Phase Gate failed after callback-slot mutation; exact fresh game process was force-stopped"
    }
}
