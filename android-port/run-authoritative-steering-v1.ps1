# GUARDED A9AST1 runner. This file is not runtime authorization.
param(
    [int]$TimeoutMs = 120000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [string]$PhysicsContextHex = "0",
    [string]$MainObjectHex = "0",
    [string]$FinalOwnerHex = "0",
    [int]$ExpectedFrameCount = 0,
    [int]$ExpectedPairWriteCount = 0,
    [switch]$OfflineValidateOnly,
    [switch]$ExecuteExactlyOneLiveAttempt,
    [switch]$AcknowledgeReviewedHashPinnedCandidate,
    [switch]$AcknowledgeNaturallyRunningRaceNoManualInput,
    [switch]$AcknowledgeSearchHasZeroGameplayWrites,
    [switch]$AcknowledgeRecordingDeclaredFixedDeltaWrites,
    [switch]$AcknowledgeTwiceFrameCountSteeringPairWrites,
    [switch]$AcknowledgeAllOtherCapabilitiesAbsent,
    [switch]$AcknowledgeLongPtraceStallAndCrashRisk
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\authoritative-steering-v1\a9tas_hwbp_authoritative_steering_v1"
$object = Join-Path $root "build\authoritative-steering-v1\hwbp_authoritative_steering_v1.o"
$source = Join-Path $root "src\hwbp_authoritative_steering_v1.cpp"
$semantic = Join-Path $root "tools\test_authoritative_steering_executor_v1.py"
$policy = Join-Path $root "tools\test_authoritative_steering_cpp_policy_v1.py"
$validator = Join-Path $root "tools\validate_authoritative_steering_v1.py"
$recording = Join-Path $root "evidence\a9tas_authoritative_steering_projection_438f_20260821.a9utk1"
$anchor = Join-Path $root "evidence\a9tas_authoritative_steering_projection_438f_20260821.a9npa1"
$readelf = Join-Path $root "..\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe"
$objdump = Join-Path $root "..\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-objdump.exe"
$pins = @{
    $binary="eb08b960fb34a06fbab069292b0f4a2e8f67896918ce3473214310f91329cf1f"
    $object="89130f46c73c1854978fd60332a64f3190f9cb9ed5e50261fc8d7670553e2991"
    $source="d1f33332ff66427b94d08a7679077aac42994e71023a4a93a4514d8952a45798"
    $semantic="9777b1592dfac4096beb665cc2f1e940db02575bcb4b81b3a5d37a6dae3fe392"
    $policy="bfaba6b26fcec304804c15e4ee757a3e5b9d543545c711bf41cd46f8c4a7326b"
    $validator="1eae53cbc459da77d8cb7bfad07cece3639e668a1106b093dac184f59335dd4e"
    $recording="ee3d710a2ed68f464d85485a26935f04c49a6632c00f517ced34b817f9078e3e"
    $anchor="9ac31fa042f56e1fb4dc5af45df8ddb61e9ea229c850c2fbe8b974778ef4a986"
}
function Get-Sha([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower() }
foreach ($path in $pins.Keys) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Sha $path) -ne $pins[$path]) { throw "Reviewed hash mismatch: $path" }
}
foreach ($tool in @($readelf,$objdump)) { if (-not (Test-Path $tool)) { throw "Missing review tool: $tool" } }
if ($TimeoutMs -lt 1000 -or $TimeoutMs -gt 300000) { throw "TimeoutMs must be 1000..300000" }
foreach ($value in @($PhysicsContextHex,$MainObjectHex,$FinalOwnerHex)) { if ($value -notmatch '^(0x)?[0-9a-fA-F]+$') { throw "Invalid address: $value" } }
$recordingBytes=[IO.File]::ReadAllBytes($recording)
if($recordingBytes.Length -lt 96){throw "Recording header is truncated"}
$frameCount=[BitConverter]::ToUInt32($recordingBytes,20)
$pairCount=$frameCount*2
if($frameCount -lt 1 -or $frameCount -gt 36000){throw "Recording frame count outside 1..36000: $frameCount"}
Push-Location (Join-Path $root "tools")
try { python -B -m unittest test_authoritative_steering_executor_v1; $testCode=$LASTEXITCODE } finally { Pop-Location }
if ($testCode -ne 0) { throw "Semantic tests failed" }
python -B $policy $binary $object $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Artifact policy failed" }
python -B $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "Validator selftest failed" }
if ($OfflineValidateOnly) {
    Write-Host "AUTHORITATIVE_STEERING_OFFLINE_VALIDATION_OK"
    Write-Host "frames=$frameCount delta=$frameCount pairs=$pairCount search_writes=0 deployed=0 candidate_sha256=$($pins[$binary])"
    return
}
foreach ($gate in @(
    @($ExecuteExactlyOneLiveAttempt,"exactly one live attempt"),
    @($AcknowledgeReviewedHashPinnedCandidate,"the reviewed hash-pinned candidate"),
    @($AcknowledgeNaturallyRunningRaceNoManualInput,"a naturally running race with no manual input"),
    @($AcknowledgeSearchHasZeroGameplayWrites,"zero gameplay writes during anchor search"),
    @($AcknowledgeRecordingDeclaredFixedDeltaWrites,"the recording-declared fixed-delta write count"),
    @($AcknowledgeTwiceFrameCountSteeringPairWrites,"twice-the-frame-count steering-pair writes"),
    @($AcknowledgeAllOtherCapabilitiesAbsent,"all other capabilities absent"),
    @($AcknowledgeLongPtraceStallAndCrashRisk,"long ptrace stall and crash risk")
)) { if (-not $gate[0]) { throw "Must acknowledge $($gate[1])" } }
if($ExpectedFrameCount -ne $frameCount -or $ExpectedPairWriteCount -ne $pairCount){throw "Expected counts must exactly equal recording: frames=$frameCount pairs=$pairCount"}
if (-not (Test-Path $AdbPath)) { throw "ADB not found: $AdbPath" }
$deviceLine=& $AdbPath devices | Where-Object { $_ -match "^$([regex]::Escape($Device))\s+device$" }
if (-not $deviceLine) { throw "ADB device not connected: $Device" }
$package="com.aligames.kuang.kybc.aligames"
$pidText=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if ($pidText -notmatch '^\d+$') { throw "Game PID unavailable/ambiguous: $pidText" }
$gamePid=[int]$pidText
function Get-Tracer { return (& $AdbPath -s $Device shell "su -c 'grep ^TracerPid: /proc/$gamePid/status'").Trim() }
if ((Get-Tracer) -notin @("TracerPid:`t0","TracerPid: 0")) { throw "Game already traced" }
$maps=& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
$bases=@(foreach($line in $maps) { if($line -match 'libAsphalt9\.so' -and $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and [Convert]::ToUInt64($matches[2],16) -eq 0){[Convert]::ToUInt64($matches[1],16)}})
$bases=@($bases | Sort-Object -Unique)
if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base, found $($bases.Count)" }
$baseHex=$bases[0].ToString("x")
$stamp=Get-Date -Format "yyyyMMdd_HHmmss_fff"
if ($OutputPath -eq "") { $OutputPath=Join-Path $root "evidence\a9tas_authoritative_steering_$stamp.a9ast1" }
$OutputPath=[IO.Path]::GetFullPath($OutputPath)
if (Test-Path $OutputPath) { throw "Output already exists: $OutputPath" }
New-Item -ItemType Directory -Path (Split-Path -Parent $OutputPath) -Force | Out-Null
$remoteBinary="/data/local/tmp/a9tas_hwbp_authoritative_steering_v1"
$remoteRecording="/data/local/tmp/a9tas_authoritative_steering_${frameCount}.a9utk1"
$remoteAnchor="/data/local/tmp/a9tas_authoritative_steering_${frameCount}.a9npa1"
$remoteReport="/data/local/tmp/a9tas_authoritative_steering_${gamePid}_$stamp.a9ast1"
foreach($pair in @(@($binary,$remoteBinary),@($recording,$remoteRecording),@($anchor,$remoteAnchor))) {
    & $AdbPath -s $Device push $pair[0] $pair[1] | Out-Host
    if($LASTEXITCODE -ne 0){throw "Push failed: $($pair[0])"}
    $remoteHash=((& $AdbPath -s $Device shell "su -c 'sha256sum $($pair[1])'")|Out-String).Trim()
    if($remoteHash -notmatch '^([0-9a-fA-F]{64})\s+' -or $matches[1].ToLower() -ne $pins[$pair[0]]){throw "Remote hash mismatch: $($pair[1])"}
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'" | Out-Null
$ack="I_ACCEPT_RECORDING_DECLARED_DELTA_AND_2X_STEERING_WRITES_V2"
Write-Host "AUTHORITATIVE_STEERING_START pid=$gamePid frames=$frameCount delta=$frameCount pairs=$pairCount"
& $AdbPath -s $Device shell "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteAnchor $remoteReport $ack $PhysicsContextHex $MainObjectHex $FinalOwnerHex'" | Out-Host
$runCode=$LASTEXITCODE
$pidAfter=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if($pidAfter -ne $pidText){throw "Game process exited or changed PID"}
if((Get-Tracer) -notin @("TracerPid:`t0","TracerPid: 0")){throw "Candidate left tracer attached"}
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'" | Out-Null
& $AdbPath -s $Device pull $remoteReport $OutputPath | Out-Host
if($LASTEXITCODE -ne 0){throw "Report pull failed"}
if($runCode -ne 0){
    python -B $validator --diagnostic $OutputPath
    if($LASTEXITCODE -ne 0){throw "A9AST1 candidate failed closed and diagnostic report was invalid (exit=$runCode)"}
    throw "A9AST1 candidate failed closed (exit=$runCode); diagnostic report=$OutputPath"
}
python -B $validator $OutputPath
if($LASTEXITCODE -ne 0){throw "Strict A9AST1 validation failed"}
if((Get-Tracer) -notin @("TracerPid:`t0","TracerPid: 0")){throw "Validation post-check found tracer"}
Write-Host "AUTHORITATIVE_STEERING_LIVE_PASSED report=$OutputPath TracerPid=0"
