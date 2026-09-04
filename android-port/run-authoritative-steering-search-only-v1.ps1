# Guarded compile-time zero-write anchor search. This file is not authorization.
param(
    [int]$TimeoutMs = 30000,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = "",
    [switch]$OfflineValidateOnly,
    [switch]$ExecuteExactlyOneSearchAttempt,
    [switch]$AcknowledgeReviewedHashPinnedZeroWriteBinary,
    [switch]$AcknowledgeRetriedSameRaceNaturallyRunning,
    [switch]$AcknowledgeSameRaceRetryPausedBeforeSingleEscape,
    [switch]$AdbEscapeResumeImmediatelyBeforeSearch,
    [switch]$AcknowledgeSingleEscapeResumeInput,
    [switch]$AcknowledgeReadOnlySearchUntilMatchOrTimeout,
    [switch]$AcknowledgeNoGameplayWriteCapabilityInArtifact,
    [switch]$AcknowledgePtraceStallAndCrashRisk
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root "build\authoritative-steering-search-only-v1\a9tas_authoritative_steering_search_only_v1"
$source = Join-Path $root "src\hwbp_authoritative_steering_v1.cpp"
$policy = Join-Path $root "tools\test_authoritative_steering_search_only_policy_v1.py"
$validator = Join-Path $root "tools\validate_authoritative_steering_search_only_v1.py"
$diagnosticValidator = Join-Path $root "tools\validate_authoritative_steering_v1.py"
$recording = Join-Path $root "evidence\a9tas_authoritative_steering_projection_438f_20260821.a9utk1"
$anchor = Join-Path $root "evidence\a9tas_authoritative_steering_projection_438f_20260821.a9npa1"
$tool = Join-Path $root "..\toolchains\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin"
$readelf = Join-Path $tool "llvm-readelf.exe"
$objdump = Join-Path $tool "llvm-objdump.exe"
$pins = @{
    $binary = "0e7b955224c5e3ac9ee8f255da56cc091520e6eaab68d5b7f66ce476f8080400"
    $source = "d1f33332ff66427b94d08a7679077aac42994e71023a4a93a4514d8952a45798"
    $policy = "7a9505292768e963d1d8ea645cae3debedb61817ec235429feba24f613a96e14"
    $validator = "233a8bfbbc5b70ed94a9e9cdda21e245455d498d754537ebc67614c3190bafc3"
    $diagnosticValidator = "1eae53cbc459da77d8cb7bfad07cece3639e668a1106b093dac184f59335dd4e"
    $recording = "ee3d710a2ed68f464d85485a26935f04c49a6632c00f517ced34b817f9078e3e"
    $anchor = "9ac31fa042f56e1fb4dc5af45df8ddb61e9ea229c850c2fbe8b974778ef4a986"
}
function Get-Sha([string]$Path){return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLower()}
foreach($path in $pins.Keys){if(-not(Test-Path -LiteralPath $path -PathType Leaf)-or(Get-Sha $path)-ne$pins[$path]){throw "Reviewed hash mismatch: $path"}}
if($TimeoutMs -lt 1000 -or $TimeoutMs -gt 120000){throw "TimeoutMs must be 1000..120000"}
python -B $policy $binary $readelf $objdump
if($LASTEXITCODE -ne 0){throw "Zero-write artifact policy failed"}
python -B $validator --selftest
if($LASTEXITCODE -ne 0){throw "Search report validator selftest failed"}
if($OfflineValidateOnly){
    Write-Host "AUTHORITATIVE_SEARCH_ONLY_OFFLINE_OK"
    Write-Host "frames=438 pwrite_imports=0 gameplay_writes=0 candidate_sha256=$($pins[$binary])"
    return
}
foreach($gate in @(
    @($ExecuteExactlyOneSearchAttempt,"exactly one search attempt"),
    @($AcknowledgeReviewedHashPinnedZeroWriteBinary,"the reviewed hash-pinned zero-write binary"),
    @($AcknowledgeReadOnlySearchUntilMatchOrTimeout,"read-only search until match or timeout"),
    @($AcknowledgeNoGameplayWriteCapabilityInArtifact,"no gameplay-write capability in the artifact"),
    @($AcknowledgePtraceStallAndCrashRisk,"ptrace stall and crash risk")
)){if(-not$gate[0]){throw "Must acknowledge $($gate[1])"}}
$startModeCount=@(
    $AcknowledgeRetriedSameRaceNaturallyRunning,
    $AdbEscapeResumeImmediatelyBeforeSearch
).Where({$_}).Count
if($startModeCount-ne1){throw "Choose exactly one start mode: naturally running or single-ESC resume"}
if($AdbEscapeResumeImmediatelyBeforeSearch){
    if(-not$AcknowledgeSameRaceRetryPausedBeforeSingleEscape){throw "Single-ESC search requires same-race Retry paused acknowledgement"}
    if(-not$AcknowledgeSingleEscapeResumeInput){throw "Single-ESC search requires exactly one resume-input acknowledgement"}
}elseif($AcknowledgeSameRaceRetryPausedBeforeSingleEscape-or$AcknowledgeSingleEscapeResumeInput){
    throw "Paused/single-ESC acknowledgements are valid only in single-ESC search mode"
}
if(-not(Test-Path -LiteralPath $AdbPath -PathType Leaf)){throw "ADB not found"}
$deviceLine=& $AdbPath devices|Where-Object{$_ -match "^$([regex]::Escape($Device))\s+device$"}
if(-not$deviceLine){throw "ADB device not connected"}
$package="com.aligames.kuang.kybc.aligames"
$pidText=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if($pidText -notmatch '^\d+$'){throw "Game PID unavailable: $pidText"}
$gamePid=[int]$pidText
function Get-Tracer{return (& $AdbPath -s $Device shell "su -c 'grep ^TracerPid: /proc/$gamePid/status'").Trim()}
if((Get-Tracer)-notin@("TracerPid:`t0","TracerPid: 0")){throw "Game already traced"}
$maps=& $AdbPath -s $Device shell "su -c 'cat /proc/$gamePid/maps'"
$baseCandidates = foreach($line in $maps){
    if($line -match 'libAsphalt9\.so' -and
       $line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
       [Convert]::ToUInt64($matches[2],16) -eq 0){
        [Convert]::ToUInt64($matches[1],16)
    }
}
$bases=@($baseCandidates | Sort-Object -Unique)
if($bases.Count-ne1){throw "Expected one libAsphalt9 base"}
$baseHex=$bases[0].ToString("x")
$stamp=Get-Date -Format "yyyyMMdd_HHmmss_fff"
if($OutputPath-eq""){$OutputPath=Join-Path $root "evidence\a9tas_authoritative_search_only_$stamp.a9ast1"}
$OutputPath=[IO.Path]::GetFullPath($OutputPath)
if(Test-Path -LiteralPath $OutputPath){throw "Output exists: $OutputPath"}
$remoteBinary="/data/local/tmp/a9tas_authoritative_search_only_v1"
$remoteRecording="/data/local/tmp/a9tas_authoritative_search_438.a9utk1"
$remoteAnchor="/data/local/tmp/a9tas_authoritative_search_438.a9npa1"
$remoteReport="/data/local/tmp/a9tas_authoritative_search_${gamePid}_$stamp.a9ast1"
foreach($pair in @(@($binary,$remoteBinary),@($recording,$remoteRecording),@($anchor,$remoteAnchor))){
    & $AdbPath -s $Device push $pair[0] $pair[1]|Out-Host
    if($LASTEXITCODE-ne0){throw "Push failed: $($pair[0])"}
    $line=((& $AdbPath -s $Device shell "su -c 'sha256sum $($pair[1])'")|Out-String).Trim()
    if($line-notmatch'^([0-9a-fA-F]{64})\s+'-or$matches[1].ToLower()-ne$pins[$pair[0]]){throw "Remote hash mismatch: $($pair[1])"}
}
& $AdbPath -s $Device shell "su -c 'chmod 700 $remoteBinary'"|Out-Null
$ack="I_ACCEPT_ZERO_WRITE_ANCHOR_SEARCH_V1"
if($AdbEscapeResumeImmediatelyBeforeSearch){
    Write-Host "AUTHORITATIVE_SEARCH_SINGLE_ESC_RESUME pid=$gamePid attach_started=0"
    & $AdbPath -s $Device shell input keyevent 111|Out-Null
    if($LASTEXITCODE-ne0){throw "The single ESC resume input failed; search was not started"}
}
Write-Host "AUTHORITATIVE_SEARCH_ONLY_START pid=$gamePid frames=438 gameplay_writes=0"
& $AdbPath -s $Device shell "su -c '$remoteBinary $gamePid $baseHex $TimeoutMs $remoteRecording $remoteAnchor $remoteReport $ack 0 0 0'"|Out-Host
$runCode=$LASTEXITCODE
$pidAfter=(& $AdbPath -s $Device shell "su -c 'pidof $package'").Trim()
if($pidAfter-ne$pidText){throw "Game process exited or changed PID"}
if((Get-Tracer)-notin@("TracerPid:`t0","TracerPid: 0")){throw "Search left tracer attached"}
& $AdbPath -s $Device shell "su -c 'chmod 644 $remoteReport'"|Out-Null
& $AdbPath -s $Device pull $remoteReport $OutputPath|Out-Host
if($LASTEXITCODE-ne0){throw "Search report pull failed"}
if($runCode-ne0){
    python -B $diagnosticValidator --diagnostic $OutputPath
    throw "Zero-write search failed closed (exit=$runCode); diagnostic=$OutputPath"
}
python -B $validator $OutputPath
if($LASTEXITCODE-ne0){throw "Strict zero-write search validation failed"}
if((Get-Tracer)-notin@("TracerPid:`t0","TracerPid: 0")){throw "Post-validation tracer found"}
Write-Host "AUTHORITATIVE_SEARCH_ONLY_LIVE_PASSED report=$OutputPath TracerPid=0 gameplay_writes=0"
