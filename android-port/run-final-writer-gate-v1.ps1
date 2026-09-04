# Guarded Ancient Ruins + ZL1 final-writer gate runner.
# Default mode is offline-only. Live modes require explicit, separate consent.

param(
    [ValidateSet("OfflineValidate", "PrepareFreshProcess", "ReadOnlyGate", "ExecuteGate")]
    [string]$Mode = "OfflineValidate",
    [ValidateSet(30, 360, 900)][int]$GateFrames = 30,
    [string]$RecordingPath = "",
    [string]$RecordingSha256 = "",
    [string]$TargetPath = "",
    [string]$TargetSha256 = "",
    [int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputDirectory = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [switch]$AcknowledgeFreshProcessPreload,
    [switch]$AcknowledgeAncientRuinsZl1Countdown3Paused,
    [switch]$AcknowledgeSingleEscapeAndNoManualInput,
    [switch]$AcknowledgeExactFixedDeltaAndControlWrites,
    [switch]$AcknowledgeFinalWriterConditional64Plus12,
    [switch]$AcknowledgePtraceStallRollbackAndCrashRisk,
    [switch]$ExecuteExactlyOneAttempt
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$buildScript = Join-Path $root "build-final-writer-live-gate-v1.ps1"
$payload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$bootstrap = Join-Path $root "build\final-writer-live-gate-v1\liba9tas_bootstrap_final_writer_v1.so"
$candidate = Join-Path $root "build\final-writer-live-gate-v1\a9tas_final_writer_live_candidate_v1"
$candidateDisassembly = Join-Path $root "build\final-writer-live-gate-v1\a9tas_final_writer_live_candidate_v1.disasm.txt"
$reviewObject = Join-Path $root "build\final-writer-unified-v1\final_writer_unified_v1_review_only.o"
$injector = Join-Path $root "build\a9tas_injector"
$helper = Join-Path $root "tools\run_final_writer_preload_v1.sh"
$parser = Join-Path $root "tools\parse_unified_executor_report_v8.py"
$pairValidator = Join-Path $root "tools\validate_final_writer_report_pair_v1.py"
$receipt = Join-Path $root "build\final-writer-live-gate-v1\prepared-process-v1.json"
$ack = "I_ACCEPT_FINAL_WRITER_UNIFIED_REVIEW_ONLY_V1"

$pins = @{
    $payload = "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763"
    $reviewObject = "f3a8b4d7a31613305c2c246fad9aa134a5a110202f4b7c28d5bc5608023c74c6"
    $bootstrap = "14ab98be3f8cbd6f2a4d15ec98b957779059ca2c3f3119dde9467982053ad82a"
    $injector = "b1422ad15955cf21563c6be7ea721cf841da16897ee9f052ccef97d0ae415528"
    $buildScript = "d4d4fc0faeb43506d7bc297055379b0c9480707840c7afd8bb60a2429ed85ba1"
    $helper = "bb329376fb4da6ff237c372086075b91a8b93fe0e0fd0299f386a2cf6ea5c662"
    $parser = "cd929bf2f30c3b82a886a27645d19d6068a53b1e42c9c77c9c8f96e663b71622"
    $pairValidator = "c073a373833451c93effba166de57df82b867a77c3342adcebf109e0fdc77193"
}
$gateArtifactPins = @{
    30 = @{ recording = "8659dceddf0edfb60d3c990ad2739f6e69e776d66eb86450754b7c6d0ba6e528";
            target = "c005cdc616917eb07914786ff7ef5427c008d9ce55876810fa088c5f49730c58" }
    360 = @{ recording = "e7dc4bf453012627fe94e007e3db88214e5714e696c335df19b726305fe67ba6";
             target = "17ff6a87d5b99f78ac2bdd7afb6148bc8b013a8de1063d80624a76eca0d18b21" }
    900 = @{ recording = "850e8c64fc55e291ae362cb6cf483c13807bf946de0679293419f122f33137ec";
             target = "42a09b5664a495f74ebe5ef536bfc8d6d0a57b7659175d8c0ecb5b9906c0213f" }
}
$remotePayload = "/data/local/tmp/liba9tas_final_writer_replay_v1_build_only.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_final_writer_v1.so"
$remoteCandidate = "/data/local/tmp/a9tas_final_writer_live_candidate_v1"
$remoteInjector = "/data/local/tmp/a9tas_injector_final_writer_v1"
$remoteHelper = "/data/local/tmp/run_final_writer_preload_v1.sh"

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Invoke-AdbText([string[]]$Arguments) {
    return ((& $AdbPath @Arguments) | Out-String).Trim()
}
function Invoke-AdbChecked([string[]]$Arguments, [string]$Label) {
    $result = & $AdbPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$Label failed: $($result -join ' ')" }
    return $result
}
function Get-GamePid {
    $value = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'pidof $package'")
    if ($value -match '^\d+$') { return [int]$value }
    return 0
}
function Get-StartTicks([int]$GamePid) {
    $stat = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$GamePid/stat'")
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed /proc/$GamePid/stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Process start-time field unavailable"
    }
    return [string]$fields[19]
}
function Get-TracerPid([int]$GamePid) {
    $value = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'grep ^TracerPid: /proc/$GamePid/status'")
    if ($value -notmatch '^TracerPid:\s*(\d+)$') { throw "Malformed TracerPid: $value" }
    return [int]$matches[1]
}
function Assert-CleanTracer([int]$GamePid) {
    $tracer = Get-TracerPid $GamePid
    if ($tracer -ne 0) { throw "Game is already traced or tracer cleanup failed: $tracer" }
}
function Assert-ArmedTracer([int]$GamePid, [int]$ExpectedTracer) {
    $tracer = Get-TracerPid $GamePid
    if ($ExpectedTracer -le 0 -or $tracer -ne $ExpectedTracer) {
        throw "Armed tracer proof mismatch: observed=$tracer expected=$ExpectedTracer"
    }
}
function Get-LibraryBaseHex([int]$GamePid) {
    $maps = & $AdbPath -s $Device shell "su -c 'cat /proc/$GamePid/maps'"
    $baseCandidates = @(foreach ($line in $maps) {
        if ($line -match 'libAsphalt9\.so' -and
            $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2], 16) -eq 0) { $matches[1].ToLowerInvariant() }
    })
    # Keep the sorted result as an array even when exactly one base exists.
    # Otherwise PowerShell scalarizes it and $bases[0] becomes the first
    # character of the address (the live FW-0 failure observed as base=0x7).
    $bases = @($baseCandidates | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base" }
    return [string]$bases[0]
}
function Assert-RemoteHash([string]$Remote, [string]$Expected) {
    $line = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'sha256sum $Remote'")
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Remote"
    }
}

if ($TimeoutMs -lt 10000 -or $TimeoutMs -gt 180000) { throw "TimeoutMs must be 10000..180000" }
foreach ($value in @($PhysicsContextHex, $MainObjectHex, $FinalOwnerHex)) {
    if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid address: $value" }
}
if ($RecordingPath -eq "") {
    $RecordingPath = Join-Path $root "evidence\a9tas_final_writer_gate_${GateFrames}f_20260821.a9utk1"
}
if ($TargetPath -eq "") {
    $TargetPath = Join-Path $root "evidence\a9tas_final_writer_gate_${GateFrames}f_20260821.a9fwt1"
}
$RecordingPath = [IO.Path]::GetFullPath($RecordingPath)
$TargetPath = [IO.Path]::GetFullPath($TargetPath)
foreach ($path in @($buildScript, $payload, $reviewObject, $injector, $helper,
                    $parser, $pairValidator, $RecordingPath, $TargetPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing gate input: $path" }
}
foreach ($path in @($payload, $reviewObject, $injector, $buildScript,
                    $helper, $parser, $pairValidator)) {
    if ((Get-Sha $path) -ne $pins[$path]) { throw "Reviewed artifact hash mismatch: $path" }
}
$actualRecordingHash = Get-Sha $RecordingPath
$actualTargetHash = Get-Sha $TargetPath
$expectedGateArtifacts = $gateArtifactPins[$GateFrames]
if ($actualRecordingHash -ne $expectedGateArtifacts.recording -or
    $actualTargetHash -ne $expectedGateArtifacts.target) {
    throw "Gate artifacts differ from the reviewed Ancient Ruins + ZL1 prefix pair"
}
if ($RecordingSha256 -ne "" -and $RecordingSha256.ToLowerInvariant() -ne $actualRecordingHash) {
    throw "Recording SHA-256 mismatch"
}
if ($TargetSha256 -ne "" -and $TargetSha256.ToLowerInvariant() -ne $actualTargetHash) {
    throw "Target SHA-256 mismatch"
}
$recordingBytes = [IO.File]::ReadAllBytes($RecordingPath)
$targetBytes = [IO.File]::ReadAllBytes($TargetPath)
if ($recordingBytes.Length -ne 96 + $GateFrames * 144 -or
    [BitConverter]::ToUInt32($recordingBytes, 20) -ne $GateFrames -or
    [BitConverter]::ToUInt32($recordingBytes, 24) -ne 16667) {
    throw "A9UTK1 gate recording shape mismatch"
}
if ($targetBytes.Length -ne 128 + $GateFrames * 80 -or
    [BitConverter]::ToUInt32($targetBytes, 20) -ne $GateFrames) {
    throw "A9FWT1 gate target shape mismatch"
}
$recordingDigest = [Convert]::FromHexString($actualRecordingHash)
for ($index = 0; $index -lt 32; ++$index) {
    if ($targetBytes[40 + $index] -ne $recordingDigest[$index]) {
        throw "A9FWT1 is not SHA-bound to A9UTK1"
    }
}

Push-Location (Join-Path $root "tools")
try {
    python -B -m unittest test_make_final_writer_prefix_v1 `
        test_parse_unified_executor_report_v8 `
        test_validate_final_writer_report_pair_v1
    $offlineCode = $LASTEXITCODE
} finally { Pop-Location }
if ($offlineCode -ne 0) { throw "Final-writer gate offline tests failed" }
if ($Mode -eq "OfflineValidate") {
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Final-writer guarded build failed" }
    if ((Get-Sha $bootstrap) -ne $pins[$bootstrap]) { throw "Bootstrap hash mismatch after build" }
    Write-Output "FINAL_WRITER_GATE_OFFLINE_VALID passed=1 frames=$GateFrames controller_emitted=0 device_access=0"
    Write-Output "recording_sha256=$actualRecordingHash target_sha256=$actualTargetHash"
    return
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found" }
$devices = Invoke-AdbText @('devices')
if ($devices -notmatch "(?m)^$([regex]::Escape($Device))\s+device$") { throw "ADB device is not connected" }

if ($Mode -eq "PrepareFreshProcess") {
    if (-not $AcknowledgeFreshProcessPreload) { throw "Must acknowledge fresh-process preload" }
    Invoke-AdbChecked @('-s', $Device, 'shell', "am force-stop $package") "force-stop old game" | Out-Null
    Start-Sleep -Milliseconds 300
    if ((Get-GamePid) -ne 0) { throw "Old game process did not stop" }
    & $buildScript | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Final-writer bootstrap build failed" }
    if ((Get-Sha $bootstrap) -ne $pins[$bootstrap]) { throw "Bootstrap hash mismatch after build" }
    foreach ($pair in @(@($payload, $remotePayload), @($bootstrap, $remoteBootstrap),
                        @($injector, $remoteInjector), @($helper, $remoteHelper))) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) "push $($pair[1])" | Out-Null
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteInjector $remoteHelper'") "chmod preload files" | Out-Null
    Assert-RemoteHash $remotePayload $pins[$payload]
    Assert-RemoteHash $remoteBootstrap $pins[$bootstrap]
    Assert-RemoteHash $remoteInjector $pins[$injector]
    Assert-RemoteHash $remoteHelper $pins[$helper]
    $preloadOut = Join-Path (Split-Path -Parent $receipt) "preload.stdout.txt"
    $preloadErr = Join-Path (Split-Path -Parent $receipt) "preload.stderr.txt"
    $preload = Start-Process -FilePath $AdbPath -ArgumentList @('-s', $Device, 'shell', 'sh', $remoteHelper) `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $preloadOut -RedirectStandardError $preloadErr
    Start-Sleep -Milliseconds 300
    Invoke-AdbChecked @('-s', $Device, 'shell', "am start -n $activity") "start fresh game" | Out-Null
    if (-not $preload.WaitForExit(45000)) {
        Stop-Process -Id $preload.Id -Force
        throw "Final-writer preload injector timed out"
    }
    $preloadText = (Get-Content -Raw $preloadOut) + "`n" + (Get-Content -Raw $preloadErr)
    $preloadText | Write-Host
    if ($preload.ExitCode -ne 0 -or $preloadText -notmatch 'bootstrap_verified=1 status=1 stage=2') {
        throw "Final-writer preload did not reach armed stage"
    }
    $gamePid = Get-GamePid
    if ($gamePid -le 0) { throw "Fresh game PID unavailable" }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $mapped = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
        if ($maps.Contains($remotePayload) -and $maps.Contains($remoteBootstrap)) { $mapped = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $mapped) { throw "Final-writer payload/bootstrap not mapped" }
    Assert-CleanTracer $gamePid
    $startTicks = Get-StartTicks $gamePid
    [ordered]@{ version=1; device=$Device; pid=$gamePid; start_ticks=$startTicks;
        payload_sha256=$pins[$payload]; bootstrap_sha256=$pins[$bootstrap] } |
        ConvertTo-Json | Set-Content -LiteralPath $receipt -Encoding utf8
    Write-Output "FINAL_WRITER_PREPARE_PASSED pid=$gamePid start_ticks=$startTicks payload_mapped=1 TracerPid=0"
    return
}

$requiredGates = @(
    @($ExecuteExactlyOneAttempt, "exactly one attempt"),
    @($AcknowledgeAncientRuinsZl1Countdown3Paused, "Ancient Ruins + ZL1 countdown-3 paused"),
    @($AcknowledgePtraceStallRollbackAndCrashRisk, "ptrace stall, rollback and crash risk")
)
if ($Mode -eq "ExecuteGate") {
    $requiredGates += @(
        @($AcknowledgeSingleEscapeAndNoManualInput, "one ESC and no manual input"),
        @($AcknowledgeExactFixedDeltaAndControlWrites, "$GateFrames delta and $($GateFrames * 2) control-pair writes"),
        @($AcknowledgeFinalWriterConditional64Plus12, "conditional in-callback 64+12 writes")
    )
}
foreach ($gate in $requiredGates) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { throw "Prepared-process receipt missing" }
$prepared = Get-Content -Raw $receipt | ConvertFrom-Json
$gamePid = Get-GamePid
if ($gamePid -le 0 -or [int]$prepared.pid -ne $gamePid -or
    [string]$prepared.device -ne $Device -or
    [string]$prepared.start_ticks -ne (Get-StartTicks $gamePid) -or
    [string]$prepared.payload_sha256 -ne $pins[$payload] -or
    [string]$prepared.bootstrap_sha256 -ne $pins[$bootstrap]) {
    throw "Prepared process identity mismatch"
}
Assert-CleanTracer $gamePid
$maps = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat /proc/$gamePid/maps'")
if (-not $maps.Contains($remotePayload) -or -not $maps.Contains($remoteBootstrap)) {
    throw "Prepared final-writer modules are not mapped"
}
$baseHex = Get-LibraryBaseHex $gamePid
$startTicks = [string]$prepared.start_ticks
$readyPath = "/data/local/tmp/a9tas_final_writer_ready_$gamePid"
if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -ne '1') {
    throw "Stale final-writer READY marker exists"
}
& $buildScript -EmitOneShotLiveCandidate -ExpectedReviewObjectSha256 $pins[$reviewObject] | Out-Host
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $candidate)) { throw "One-shot candidate build failed" }
if ((Get-Sha $bootstrap) -ne $pins[$bootstrap]) { throw "Bootstrap hash mismatch after one-shot build" }
$candidateHash = Get-Sha $candidate
$stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($OutputDirectory -eq "") { $OutputDirectory = Join-Path $root "evidence" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$localUnified = Join-Path $OutputDirectory "a9tas_final_writer_${GateFrames}f_${stamp}.a9uer8"
$localPayloadReport = Join-Path $OutputDirectory "a9tas_final_writer_${GateFrames}f_${stamp}.a9fwr1"
$remoteRecording = "/data/local/tmp/a9tas_final_writer_${GateFrames}f_$stamp.a9utk1"
$remoteTarget = "/data/local/tmp/a9tas_final_writer_${GateFrames}f_$stamp.a9fwt1"
$remoteUnified = "/data/local/tmp/a9tas_final_writer_${GateFrames}f_$stamp.a9uer8"
$remotePayloadReport = "/data/local/tmp/a9tas_final_writer_${GateFrames}f_$stamp.a9fwr1"
try {
    foreach ($pair in @(@($candidate, $remoteCandidate, $candidateHash),
                        @($RecordingPath, $remoteRecording, $actualRecordingHash),
                        @($TargetPath, $remoteTarget, $actualTargetHash))) {
        Invoke-AdbChecked @('-s', $Device, 'push', $pair[0], $pair[1]) "push gate artifact" | Out-Null
        Assert-RemoteHash $pair[1] $pair[2]
    }
    Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'chmod 700 $remoteCandidate'") "chmod candidate" | Out-Null
    $candidateTimeoutMs = if ($Mode -eq "ReadOnlyGate") { 30000 } else { $TimeoutMs }
    $remoteModePrefix = if ($Mode -eq "ReadOnlyGate") { "A9TAS_FINAL_WRITER_READ_ONLY_GATE_V1=1 " } else { "" }
    $remoteCommand = "$remoteModePrefix$remoteCandidate $gamePid $baseHex $candidateTimeoutMs $startTicks $remoteRecording $remoteTarget $remoteUnified $remotePayloadReport $ack $PhysicsContextHex $MainObjectHex $FinalOwnerHex"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $AdbPath; $startInfo.UseShellExecute = $false; $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true; $startInfo.RedirectStandardError = $true
    foreach ($argument in @('-s', $Device, 'shell', "su -c '$remoteCommand'")) { $startInfo.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Failed to launch final-writer candidate" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $releaseCommitted = $false
    $forcedTracerTermination = $false
    $primaryFailure = ""
    try {
        $ready = $false; $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
            if ((Invoke-AdbText @('-s', $Device, 'shell', "su -c 'test -e $readyPath; echo `$?'")) -eq '0') { $ready = $true; break }
            Start-Sleep -Milliseconds 100
        }
        if (-not $ready) { throw "Final-writer candidate did not publish READY" }
        $readyText = Invoke-AdbText @('-s', $Device, 'shell', "su -c 'cat $readyPath'")
        if ($readyText -notmatch "pid=$gamePid start_ticks=$startTicks " -or
            (Get-StartTicks $gamePid) -ne $startTicks) { throw "READY identity proof mismatch" }
        if ($Mode -eq "ReadOnlyGate") {
            if ($readyText -notmatch '^READY_NO_ATTACH_FINAL_WRITER_V1 ' -or
                $readyText -notmatch 'target_threads_attached=0 gameplay_writes=0 payload_mapped=1') {
                throw "Read-only READY proof mismatch"
            }
            Assert-CleanTracer $gamePid
            Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $readyPath'") "acknowledge read-only READY" | Out-Null
            $releaseCommitted = $true
            if (-not $process.WaitForExit(10000)) { throw "Read-only gate did not exit cleanly" }
            $stdout = $stdoutTask.GetAwaiter().GetResult(); $stderr = $stderrTask.GetAwaiter().GetResult()
            if ($stdout) { Write-Host $stdout.TrimEnd() }; if ($stderr) { Write-Host $stderr.TrimEnd() }
            if ($process.ExitCode -ne 0 -or $stdout -notmatch 'FINAL_WRITER_READ_ONLY_COMPLETE' -or
                (Get-GamePid) -ne $gamePid -or
                (Get-StartTicks $gamePid) -ne $startTicks) {
                throw "Read-only gate lifetime proof failed"
            }
            Assert-CleanTracer $gamePid
            $remoteReports = Invoke-AdbText @('-s', $Device, 'shell',
                "su -c 'if [ -e $remoteUnified ] || [ -e $remotePayloadReport ]; then echo EXISTS; fi'")
            if ($remoteReports -ne "") { throw "Read-only gate unexpectedly emitted a runtime report" }
            Write-Output "FINAL_WRITER_FW0_PASSED pid=$gamePid start_ticks=$startTicks attached=0 gameplay_writes=0 reports=0 TracerPid=0"
            return
        }
        if ($readyText -notmatch '^READY_ARMED_FINAL_WRITER_V1 ' -or
            $readyText -notmatch 'target_threads_attached=[1-9][0-9]* vptr_writes=1 gameplay_state_writes=0' -or
            $readyText -notmatch 'all_target_threads_frozen=1') {
            throw "Armed READY proof mismatch"
        }
        if ($readyText -notmatch 'controller_pid=(\d+) ') {
            throw "Armed controller identity missing"
        }
        $remoteControllerPid = [int]$matches[1]
        Assert-ArmedTracer $gamePid $remoteControllerPid
        Write-Host "FINAL_WRITER_HOST_READY frames=$GateFrames releasing_frozen_threads=1"
        Invoke-AdbChecked @('-s', $Device, 'shell', "su -c 'rm -f $readyPath'") "remove READY marker" | Out-Null
        $releaseCommitted = $true
        Write-Host "FINAL_WRITER_HOST_RELEASED frames=$GateFrames sending_single_ESC=1"
        Invoke-AdbChecked @('-s', $Device, 'shell', 'input keyevent 111') "single ESC" | Out-Null
        if (-not $process.WaitForExit($TimeoutMs + 15000)) { throw "Final-writer candidate timed out; no retry" }
    } catch {
        $primaryFailure = $_.Exception.Message
    }
    if ($primaryFailure -ne "" -and -not $process.HasExited) {
        $rollbackWaitMs = if ($releaseCommitted) { 5000 } else { 25000 }
        [void]$process.WaitForExit($rollbackWaitMs)
    }
    if ($primaryFailure -ne "" -and -not $process.HasExited) {
        $liveTracer = 0
        try { if ((Get-GamePid) -eq $gamePid) { $liveTracer = Get-TracerPid $gamePid } } catch { $liveTracer = 0 }
        if ($liveTracer -gt 0) {
            $forcedTracerTermination = $true
            & $AdbPath -s $Device shell "su -c 'kill -TERM $liveTracer'" | Out-Null
            [void]$process.WaitForExit(5000)
        }
    }
    if ($primaryFailure -ne "") { & $AdbPath -s $Device shell "su -c 'rm -f $readyPath'" | Out-Null }
    if (-not $process.HasExited) {
        try { $process.Kill($true) } catch {}
        [void]$process.WaitForExit(5000)
    }
    if (-not $process.HasExited) { throw "$primaryFailure; candidate cleanup did not complete" }
    $stdout = $stdoutTask.GetAwaiter().GetResult(); $stderr = $stderrTask.GetAwaiter().GetResult()
    if ($stdout) { Write-Host $stdout.TrimEnd() }; if ($stderr) { Write-Warning $stderr.TrimEnd() }
    if ($primaryFailure -ne "") {
        $cleanupTracer = if ((Get-GamePid) -eq $gamePid) { Get-TracerPid $gamePid } else { -1 }
        if ($cleanupTracer -ne 0) { throw "$primaryFailure; cleanup TracerPid=$cleanupTracer; restart game process" }
        if ($forcedTracerTermination) { throw "$primaryFailure; forced tracer termination used; restart game process" }
        throw "$primaryFailure; rollback complete; no retry"
    }
    if ((Get-GamePid) -ne $gamePid -or (Get-StartTicks $gamePid) -ne $startTicks) { throw "Game process changed" }
    Assert-CleanTracer $gamePid
    if ($process.ExitCode -ne 0) {
        Write-Host "FINAL_WRITER_FAILED_CLEAN pid=$gamePid start_ticks=$startTicks TracerPid=0"
        throw "Final-writer candidate failed closed (exit=$($process.ExitCode)); no retry"
    }
    Invoke-AdbChecked @('-s', $Device, 'shell',
        "su -c 'chmod 644 $remoteUnified $remotePayloadReport'") "chmod runtime reports" | Out-Null
    foreach ($pair in @(@($remoteUnified, $localUnified), @($remotePayloadReport, $localPayloadReport))) {
        Invoke-AdbChecked @('-s', $Device, 'pull', $pair[0], $pair[1]) "pull report" | Out-Null
    }
    python -B $parser $localUnified
    if ($LASTEXITCODE -ne 0) { throw "A9UER8 validation failed" }
    python -B $pairValidator $localUnified $localPayloadReport $TargetPath $payload
    if ($LASTEXITCODE -ne 0) { throw "A9UER8/A9FWR1 cross-validation failed" }
    Write-Output "FINAL_WRITER_GATE_PASSED frames=$GateFrames single_ESC=1 TracerPid=0"
    Write-Output "Unified report: $localUnified"
    Write-Output "Payload report: $localPayloadReport"
} finally {
    if (Test-Path -LiteralPath $candidate) { Remove-Item -LiteralPath $candidate -Force }
    if (Test-Path -LiteralPath $candidateDisassembly) { Remove-Item -LiteralPath $candidateDisassembly -Force }
}
