# GUARDED Gate 10 brake-only runner. This file is not authorization.
param(
    [int]$TimeoutMs=10000,
    [string]$Device="emulator-5554",
    [string]$AdbPath="D:\leidian\LDPlayer9\adb.exe",
    [string]$ReportOutputPath="",
    [switch]$OfflineValidateOnly,
    [switch]$Gate9Validated,
    [switch]$NaturalReplayValidated,
    [switch]$AcknowledgeRunningRaceNoManualInput,
    [switch]$AcknowledgeOneFixedDeltaWrite,
    [switch]$AcknowledgeTwoBrakePairWrites,
    [switch]$AcknowledgeRawNegativeOneBrake,
    [switch]$AcknowledgeTransformAndOtherActionsSkipped,
    [switch]$AcknowledgeShortPtraceStallRisk
)
$ErrorActionPreference="Stop"
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$binary=Join-Path $root "build\staging\a9tas_hwbp_unified_brake_v1"
$source=Join-Path $root "src\hwbp_unified_tick_executor_v1.cpp"
$input=Join-Path $root "evidence\a9tas_gate10_brake_only_1f_draft_20260817.a9utk1"
$parser=Join-Path $root "tools\parse_unified_executor_report_v6.py"
$verifier=Join-Path $root "tools\verify_unified_brake_gate_v1.py"
$pins=@{
 $binary="8f21c4820900876c2e7aa5b0c11e7df3768210f81188b27f2bbb8a17736b736f"
 $source="65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4"
 $input="d66d6772c48b86be17c67ffd935ca954ca2a74323b18ad9bf905de71d2310f45"
 $parser="9ea083241553ff30bb454ebb38f099ef85c9998dbf0bd24834441e6df209474c"
 $verifier="adfe3640515ba8fd67fdf940637cde4ee77fd1a5c5e1e1ac500637cf0d248523"
}
function Get-Sha([string]$Path){$s=[IO.File]::OpenRead($Path);try{$h=[Security.Cryptography.SHA256]::Create();try{return -join($h.ComputeHash($s)|ForEach-Object{$_.ToString('x2')})}finally{$h.Dispose()}}finally{$s.Dispose()}}
foreach($item in $pins.Keys){if(-not(Test-Path -LiteralPath $item -PathType Leaf)){throw "Missing artifact: $item"};if((Get-Sha $item)-ne$pins[$item]){throw "Reviewed artifact hash mismatch: $item"}}
python -B (Join-Path $root "tools\unified_tick_recording_v1.py") $input;if($LASTEXITCODE-ne 0){throw "Gate 10 input invalid"}
Push-Location (Join-Path $root "tools");try{python -B -m unittest test_unified_brake_gate_v1 test_unified_tick_recording_v1 test_unified_tick_executor_semantics_v1;$testCode=$LASTEXITCODE}finally{Pop-Location};if($testCode-ne 0){throw "Gate 10 offline tests failed"}
if($OfflineValidateOnly){Write-Host "GATE10_BRAKE_OFFLINE_VALIDATION_OK";return}
foreach($gate in @(
 @($Gate9Validated,"Gate 9 evidence"),@($NaturalReplayValidated,"natural replay evidence"),
 @($AcknowledgeRunningRaceNoManualInput,"a naturally running race with no manual input"),
 @($AcknowledgeOneFixedDeltaWrite,"one fixed-delta write"),
 @($AcknowledgeTwoBrakePairWrites,"two brake pair writes"),
 @($AcknowledgeRawNegativeOneBrake,"raw -1.0 brake target"),
 @($AcknowledgeTransformAndOtherActionsSkipped,"transform and all other actions skipped"),
 @($AcknowledgeShortPtraceStallRisk,"the ptrace stall risk")
)){if(-not$gate[0]){throw "Must acknowledge $($gate[1])"}}
if($TimeoutMs-lt 1000-or$TimeoutMs-gt 30000){throw "TimeoutMs must be 1000..30000"}
if(-not(Test-Path -LiteralPath $AdbPath -PathType Leaf)){throw "ADB not found"}
$package="com.aligames.kuang.kybc.aligames";$ack="I_ACCEPT_UNIFIED_TICK_EXECUTOR_V1"
$pidText=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim();if($pidText-notmatch'^\d+$'){throw "Game PID unavailable"};$gamePid=[int]$pidText
$tracer=(& $AdbPath -s $Device shell "grep '^TracerPid:' /proc/$gamePid/status").Trim();if($tracer-ne"TracerPid:`t0"-and$tracer-ne"TracerPid: 0"){throw "Game already traced: $tracer"}
$maps=& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'";$bases=foreach($line in $maps){if($line-match'libAsphalt9\.so'-and$line-match'^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+'-and[Convert]::ToUInt64($matches[2],16)-eq 0){[Convert]::ToUInt64($matches[1],16)}};$bases=@($bases|Sort-Object -Unique);if($bases.Count-ne 1){throw "libAsphalt9 base resolution failed"};$baseHex=$bases[0].ToString('x')
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss_fff';if($ReportOutputPath-eq''){$ReportOutputPath=Join-Path $root "evidence\a9tas_gate10_brake_$stamp.a9uer6"};$ReportOutputPath=[IO.Path]::GetFullPath($ReportOutputPath);if(Test-Path -LiteralPath $ReportOutputPath){throw "Output exists"}
$remoteBinary="/data/local/tmp/a9tas_hwbp_unified_brake_v1";$remoteInput="/data/local/tmp/a9tas_gate10_${gamePid}_$stamp.a9utk1";$remoteReport="/data/local/tmp/a9tas_gate10_${gamePid}_$stamp.a9uer6"
foreach($pair in @([pscustomobject]@{Local=$binary;Remote=$remoteBinary},[pscustomobject]@{Local=$input;Remote=$remoteInput})){& $AdbPath -s $Device push $pair.Local $pair.Remote|Out-Host;if($LASTEXITCODE-ne 0){throw "Push failed"}}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary; $remoteBinary $gamePid $baseHex $TimeoutMs $remoteInput $remoteReport $ack'"|Out-Host;$code=$LASTEXITCODE
$after=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim();$tracerAfter=(& $AdbPath -s $Device shell "grep '^TracerPid:' /proc/$gamePid/status").Trim();if($after-ne$pidText-or($tracerAfter-ne"TracerPid:`t0"-and$tracerAfter-ne"TracerPid: 0")){throw "Gate 10 did not cleanly detach"};if($code-ne 0){throw "Gate 10 failed closed (exit=$code)"}
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'";& $AdbPath -s $Device pull $remoteReport $ReportOutputPath|Out-Host;if($LASTEXITCODE-ne 0){throw "Report pull failed"}
python -B $verifier $ReportOutputPath $input;if($LASTEXITCODE-ne 0){throw "Gate 10 report verification failed"}
Write-Host "GATE10_BRAKE_PASSED report=$ReportOutputPath TracerPid=0"
