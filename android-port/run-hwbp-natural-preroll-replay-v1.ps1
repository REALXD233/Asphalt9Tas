# GUARDED NATURAL PRE-ROLL REPLAY. This file is not authorization.
# Search cycles are read-only; frame 0 begins on the tick after a committed match.

param(
    [Parameter(Mandatory=$true)][string]$RecordingPath,
    [Parameter(Mandatory=$true)][string]$SourceReportPath,
    [Parameter(Mandatory=$true)][string]$AnchorPath,
    [int]$TimeoutMs = 30000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$ReplayReportOutputPath = "",
    [string]$SearchReportOutputPath = "",
    [switch]$SearchOnly,
    [switch]$OfflineValidateOnly,
    [switch]$Gate9Validated,
    [switch]$NaturalSourceLiveValidated,
    [switch]$AcknowledgeRetriedRaceAlreadyRunning,
    [switch]$AcknowledgeReadOnlySearchUntilMatchOrTimeout,
    [switch]$AcknowledgeSearchOnlyNoGameplayWrites,
    [switch]$AcknowledgeFiveFixedDeltaWritesAfterMatch,
    [switch]$AcknowledgeTenSteeringWritesAfterMatch,
    [switch]$AcknowledgeUpToFivePhysicsCorrectionsAfterMatch,
    [switch]$AcknowledgeFirstFrameGuard,
    [switch]$AcknowledgeShortPtraceStallRisk
)

$ErrorActionPreference="Stop"
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$binaryName=if($SearchOnly){"a9tas_hwbp_natural_preroll_search_only_v1"}else{"a9tas_hwbp_natural_preroll_replay_v1"}
$binary=Join-Path $root "build\staging\$binaryName"
$source=Join-Path $root "src\hwbp_unified_tick_executor_v1.cpp"
$header=Join-Path $root "src\natural_preroll_anchor_v1.h"
$syncVerifier=Join-Path $root "tools\synchronized_tick_recording_v1.py"
$anchorTool=Join-Path $root "tools\natural_preroll_anchor_v1.py"
$replayVerifier=Join-Path $root "tools\aligned_tick_replay_v1.py"
$searchVerifier=Join-Path $root "tools\natural_preroll_search_report_v1.py"
$pins=@{
    $binary=$(if($SearchOnly){"3c9eee24744b0abf8d6889d2262f71cc986ee8b60961533a6565047c5bd74750"}else{"978d791af0a0d3e617b1beacefc7ac2f53e083bb34f80057f2a96109e5502c32"})
    $source="65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4"
    $header="dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75"
    $syncVerifier="5ff50df48f1412371628d8618ba185c54926b048da38a74820487bbe17e5a5ae"
$anchorTool="532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf"
    $replayVerifier="8b67002d442dbc003e5a3bd39754fa7b01410ef1c5e1663888e5c1c8aa045391"
    $searchVerifier="38f658dfcac9f99e05c53b1c907378bd0adc29007eafe7b9d87c269bf2355c36"
}
$ack="I_ACCEPT_UNIFIED_TICK_EXECUTOR_V1"
$package="com.aligames.kuang.kybc.aligames"
$remoteBinary="/data/local/tmp/$binaryName"

function Get-Sha([string]$Path){$s=[IO.File]::OpenRead($Path);try{$h=[Security.Cryptography.SHA256]::Create();try{return -join($h.ComputeHash($s)|ForEach-Object{$_.ToString("x2")})}finally{$h.Dispose()}}finally{$s.Dispose()}}
if($TimeoutMs -lt 1000 -or $TimeoutMs -gt 30000){throw "TimeoutMs must be 1000..30000"}
foreach($item in @($RecordingPath,$SourceReportPath,$AnchorPath)+$pins.Keys){if(-not(Test-Path -LiteralPath $item -PathType Leaf)){throw "Missing artifact: $item"}}
foreach($item in $pins.Keys){$actual=Get-Sha $item;if($actual -ne $pins[$item]){throw "Reviewed artifact hash mismatch: $item"}}
$RecordingPath=[IO.Path]::GetFullPath($RecordingPath);$SourceReportPath=[IO.Path]::GetFullPath($SourceReportPath);$AnchorPath=[IO.Path]::GetFullPath($AnchorPath)

python -B $syncVerifier $SourceReportPath $RecordingPath
if($LASTEXITCODE -ne 0){throw "Synchronized source validation failed"}
python -B $anchorTool verify $AnchorPath $SourceReportPath $RecordingPath
if($LASTEXITCODE -ne 0){throw "Bound A9NPA1 verification failed"}
Push-Location (Join-Path $root "tools")
try{python -B -m unittest test_natural_preroll_anchor_v1 test_natural_preroll_replay_policy_v1 test_natural_preroll_search_report_v1;$testCode=$LASTEXITCODE}finally{Pop-Location}
if($testCode -ne 0){throw "Natural pre-roll replay offline tests failed"}
if($OfflineValidateOnly){Write-Host "NATURAL_PREROLL_REPLAY_OFFLINE_VALIDATION_OK";Write-Host "search_gameplay_writes=0 frames_after_match=$(if($SearchOnly){0}else{5})";Write-Host "binary_sha256=$($pins[$binary])";return}

foreach($gate in @(
    @($Gate9Validated,"Gate 9 evidence"),@($NaturalSourceLiveValidated,"natural source live evidence"),
    @($AcknowledgeRetriedRaceAlreadyRunning,"an already-running retried race"),
    @($AcknowledgeReadOnlySearchUntilMatchOrTimeout,"read-only search until match or timeout"),
    @($(if($SearchOnly){$AcknowledgeSearchOnlyNoGameplayWrites}else{$true}),"search-only zero-gameplay-write scope"),
    @($(if($SearchOnly){$true}else{$AcknowledgeFiveFixedDeltaWritesAfterMatch}),"five fixed-delta writes after match"),
    @($(if($SearchOnly){$true}else{$AcknowledgeTenSteeringWritesAfterMatch}),"ten steering writes after match"),
    @($(if($SearchOnly){$true}else{$AcknowledgeUpToFivePhysicsCorrectionsAfterMatch}),"up to five physics corrections after match"),
    @($(if($SearchOnly){$true}else{$AcknowledgeFirstFrameGuard}),"the first-frame guard"),
    @($AcknowledgeShortPtraceStallRisk,"the ptrace stall risk")
)){if(-not $gate[0]){throw "Must acknowledge $($gate[1])"}}
if(-not(Test-Path -LiteralPath $AdbPath -PathType Leaf)){throw "ADB not found: $AdbPath"}
$deviceLine=& $AdbPath devices|Where-Object{$_ -match "^$([regex]::Escape($Device))\s+device$"};if(-not $deviceLine){throw "ADB device not connected"}
$pidText=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim();if($pidText -notmatch '^\d+$'){throw "Game PID unavailable: '$pidText'"};$gamePid=[int]$pidText
$tracer=(& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim();if($tracer -ne "TracerPid:`t0" -and $tracer -ne "TracerPid: 0"){throw "Game already traced: '$tracer'"}
$maps=& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'";$bases=foreach($line in $maps){if($line -match 'libAsphalt9\.so' -and $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and [Convert]::ToUInt64($matches[2],16)-eq 0){[Convert]::ToUInt64($matches[1],16)}};$bases=@($bases|Sort-Object -Unique);if($bases.Count-ne 1){throw "libAsphalt9 base resolution failed"};$baseHex=$bases[0].ToString("x")
$stamp=Get-Date -Format "yyyyMMdd_HHmmss_fff";if($ReplayReportOutputPath -eq ""){$ReplayReportOutputPath=Join-Path $root "evidence\a9tas_natural_replay_$stamp.a9uer5"};if($SearchReportOutputPath -eq ""){$SearchReportOutputPath=Join-Path $root "evidence\a9tas_natural_replay_$stamp.a9npr1"};$ReplayReportOutputPath=[IO.Path]::GetFullPath($ReplayReportOutputPath);$SearchReportOutputPath=[IO.Path]::GetFullPath($SearchReportOutputPath);if($ReplayReportOutputPath -eq $SearchReportOutputPath){throw "Output paths must differ"};foreach($o in @($ReplayReportOutputPath,$SearchReportOutputPath)){if(Test-Path -LiteralPath $o){throw "Output exists: $o"};New-Item -ItemType Directory -Path (Split-Path -Parent $o) -Force|Out-Null}
$remoteRecording="/data/local/tmp/a9tas_natural_replay_${gamePid}_$stamp.a9utk1";$remoteAnchor="/data/local/tmp/a9tas_natural_replay_${gamePid}_$stamp.a9npa1";$remoteReplay="/data/local/tmp/a9tas_natural_replay_${gamePid}_$stamp.a9uer5";$remoteSearch="/data/local/tmp/a9tas_natural_replay_${gamePid}_$stamp.a9npr1"
function Get-RemoteSha([string]$Path){$line=((& $AdbPath -s $Device shell "su -c 'sha256sum $Path'")|Out-String).Trim();if($line-notmatch'^([0-9a-fA-F]{64})\s+'){throw "Remote hash failed: $Path"};return $matches[1].ToLowerInvariant()}
function Assert-Clean{$after=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim();if($after-ne$pidText){throw "Game PID changed"};$t=(& $AdbPath -s $Device shell "cat /proc/$gamePid/status | grep '^TracerPid:'").Trim();if($t-ne"TracerPid:`t0"-and$t-ne"TracerPid: 0"){throw "Replay left tracer: '$t'"}}
foreach($pair in @(@($binary,$remoteBinary),@($RecordingPath,$remoteRecording),@($AnchorPath,$remoteAnchor))){& $AdbPath -s $Device push $pair[0] $pair[1]|Out-Host;if($LASTEXITCODE -ne 0){throw "Push failed: $($pair[0])"};if((Get-RemoteSha $pair[1]) -ne (Get-Sha $pair[0])){throw "Remote hash mismatch: $($pair[0])"}}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'";Write-Host "NATURAL_PREROLL_REPLAY_START pid=$gamePid search_gameplay_writes=0"
& $AdbPath -s $Device shell "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteAnchor $remoteReplay $remoteSearch $ack 0 0 0'"|Out-Host;$code=$LASTEXITCODE;Assert-Clean;if($code-ne 0){throw "Natural replay failed closed (exit=$code, TracerPid=0)"}
$reportPairs=if($SearchOnly){
    @([pscustomobject]@{Remote=$remoteSearch;Local=$SearchReportOutputPath})
}else{
    @(
        [pscustomobject]@{Remote=$remoteReplay;Local=$ReplayReportOutputPath}
        [pscustomobject]@{Remote=$remoteSearch;Local=$SearchReportOutputPath}
    )
}
foreach($pair in $reportPairs){$hash=Get-RemoteSha $pair.Remote;& $AdbPath -s $Device shell "su -c 'chmod 644 $($pair.Remote)'";& $AdbPath -s $Device pull $pair.Remote $pair.Local|Out-Host;if($LASTEXITCODE -ne 0 -or (Get-Sha $pair.Local) -ne $hash){throw "Pulled report mismatch: $($pair.Local)"}}
python -B $searchVerifier $SearchReportOutputPath $AnchorPath;if($LASTEXITCODE-ne 0){throw "A9NPR1 search verification failed"};if(-not $SearchOnly){python -B $replayVerifier $ReplayReportOutputPath $SourceReportPath $RecordingPath;if($LASTEXITCODE-ne 0){throw "A9UER5 replay verification failed"}}
Write-Host "NATURAL_PREROLL_REPLAY_PASSED";Write-Host "Replay report: $ReplayReportOutputPath";Write-Host "Search report: $SearchReportOutputPath"
