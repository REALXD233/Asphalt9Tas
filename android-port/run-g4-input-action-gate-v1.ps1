param(
    [ValidateSet("OfflineValidate", "PrepareProcess", "ExecuteTickGate",
                 "ExecuteRecordGate", "ExecuteReplayGate",
                 "ExecutePauseResumeReplayGate",
                 "ExecuteLifecycleRecordGate",
                 "ExecuteLifecycleRetryReplayGate")]
    [string]$Mode = "OfflineValidate",
    [ValidateRange(1, 7200)][int]$GateTicks = 5,
    [string]$ReplayRecording,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$GamePackage = "com.aligames.kuang.kybc.aligames",
    [string]$GameProcessName = "",
    [string]$GameActivity = "",
    # Optional manual override.  When omitted, every generated profile is
    # staged and the runner selects by the mapped libAsphalt9.so SHA-256, not
    # by package/channel name.
    [string]$BuildProfile = "",
    [ValidateRange(0, 2147483647)][int]$TargetProcessId = 0,
    # LDPlayer cold starts can legitimately take longer than one minute before
    # libAsphalt9 and both preloaded artifacts appear in one stable maps
    # snapshot.  Keep the identity predicate strict, but allow the observed
    # slow-start case to finish instead of reporting a false preparation
    # failure.
    [ValidateRange(10000, 120000)][int]$TimeoutMs = 120000,
    [switch]$AcknowledgeDeviceAccessAndFixedArtifactPush,
    [switch]$AcknowledgeFreshDisposableGameProcess,
    [Alias("AcknowledgeFiveSessionHooksAndAutomaticEsc")]
    [switch]$AcknowledgeSevenSessionHooksAndAutomaticEsc,
    [switch]$AcknowledgeRestoreOrTerminateOnUncertainty,
    [switch]$ExecuteExactlyOneStage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildScript = Join-Path $root "build-g4-input-action-live-gate-v1.ps1"
$payloadDir = Join-Path $root "build\g4-multi-hook-runtime-v1"
$buildDir = Join-Path $root "build\g4-input-action-live-gate-v1"
$payloadSource = Join-Path $root "src\payload_g4_multi_hook_runtime_v1.cpp"
$payload = Join-Path $payloadDir "liba9tas_g4_multi_hook_runtime_v1.so"
$bootstrap = Join-Path $buildDir "liba9tas_g4_input_action_bootstrap_v1.so"
$controller = Join-Path $buildDir "a9tas_g4_input_action_controller_v1"
$sourceHash = (Get-FileHash -LiteralPath $payloadSource -Algorithm SHA256).Hash.ToLowerInvariant()
$carrier = Join-Path $buildDir "a9tas_habi1_early_carrier_$($sourceHash.Substring(0,16))"
$observer = Join-Path $root "build\physics-interval-readonly-v1\a9tas_g8_profile_physics_interval_readonly_observer_v1"
$observerBuildScript = Join-Path $root "build-physics-interval-readonly-v1.ps1"
$parser = Join-Path $root "tools\parse_physics_interval_readonly_v1.py"
$baselineVerifier = Join-Path $root "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $root "baselines\known_good_900_exact_interval_v2.json"
$recordVerifier = Join-Path $root "tools\verify_g4_physics_recording_v2.py"
$recordVerifierSelftest = Join-Path $root "tools\test_verify_g4_physics_recording_v2.py"
$divergenceAnalyzer = Join-Path $root "tools\analyze_g5_replay_divergence_v1.py"
$divergenceAnalyzerSelftest = Join-Path $root "tools\test_analyze_g5_replay_divergence_v1.py"
$upstreamIntervalGuard = Join-Path $root "tools\test_upstream_physics_interval_semantics_v1.py"
$lifecyclePolicy = Join-Path $root "tools\test_g8_lifecycle_record_policy_v1.py"

foreach ($required in @($buildScript,$payloadSource,$baselineVerifier,$baseline,
                         $recordVerifier,$recordVerifierSelftest,
                         $divergenceAnalyzer,$divergenceAnalyzerSelftest,
                         $upstreamIntervalGuard,$lifecyclePolicy)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing G4 runner input: $required"
    }
}

$package = $GamePackage.Trim()
$processName = if ([string]::IsNullOrWhiteSpace($GameProcessName)) {
    $package
} else {
    $GameProcessName.Trim()
}
if ($package -notmatch '^[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+$') {
    throw "Invalid Android package name: $package"
}
if ($processName -notmatch '^[A-Za-z0-9._:-]+$' -or $processName.Length -ge 192) {
    throw "Invalid exact Android process name: $processName"
}
if ($Mode -eq 'PrepareProcess' -and $TargetProcessId -ne 0) {
    throw 'PrepareProcess creates a new PID; use GameProcessName, not TargetProcessId.'
}
$remotePayload = "/data/local/tmp/liba9tas_g4_multi_hook_runtime_v1.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_g4_input_action_bootstrap_v1.so"
$remoteController = "/data/local/tmp/a9tas_g4_input_action_controller_v1"
$remoteCarrier = "/data/local/tmp/a9tas_habi1_early_carrier_$($sourceHash.Substring(0,16))"
$remoteObserver = "/data/local/tmp/a9tas_g8_profile_physics_interval_readonly_observer_v1"
$remoteBuildProfile = "/data/local/tmp/a9tas_g8_runtime_build_profile_v1.bin"
$remoteCarrierLog = "/data/local/tmp/a9tas-g4-early-carrier.log"
$remoteInstallMarker = "/data/local/tmp/a9tas-g4-install-proven-begin-v1"
$ack = "I_ACCEPT_G4_TICK_COORDINATOR_V1"
$limit = $GateTicks
$lifecycleMode = $Mode -eq 'ExecuteLifecycleRecordGate' -or
                 $Mode -eq 'ExecuteLifecycleRetryReplayGate'
$replayMode = $Mode -eq 'ExecuteReplayGate' -or
              $Mode -eq 'ExecutePauseResumeReplayGate'
if ($Mode -ne 'OfflineValidate' -and -not $lifecycleMode -and
    $GateTicks -notin @(5, 60, 900)) {
    throw 'Fixed-length Gates accept only the frozen 5/60/900 tick limits; extended capacity is lifecycle-only.'
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-BuildProfileMetadata([string]$Path) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $bytes = [IO.File]::ReadAllBytes($resolved)
    if ($bytes.Length -lt 416 -or
        [Text.Encoding]::ASCII.GetString($bytes,0,6) -ne 'A9BPR1' -or
        [BitConverter]::ToUInt32($bytes,8) -ne 1 -or
        [BitConverter]::ToUInt32($bytes,12) -ne 384 -or
        [Text.Encoding]::ASCII.GetString($bytes,384,7) -ne 'A9BPAX1' -or
        [BitConverter]::ToUInt32($bytes,408) -ne $bytes.Length) {
        throw "Invalid G8 runtime build profile: $resolved"
    }
    $nativeSha = -join ($bytes[232..263] | ForEach-Object { $_.ToString('x2') })
    if ($nativeSha -notmatch '^[0-9a-f]{64}$' -or
        $nativeSha -eq ('0' * 64)) {
        throw "Invalid native SHA in G8 profile: $resolved"
    }
    return [PSCustomObject]@{
        Path = $resolved
        NativeSha = $nativeSha
        FileSha = Get-Sha $resolved
        RemoteCandidate = "/data/local/tmp/a9tas-g8-profile-$nativeSha.bin"
    }
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
function Wait-Until([scriptblock]$Condition, [string]$Label) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out: $Label"
}
function Get-GamePid {
    # pidof is available to the adb shell user.  Do not request a fresh
    # superuser grant merely to discover whether the game exists.
    $value = Invoke-AdbText @('-s',$Device,'shell','pidof',$processName)
    $ids = @([Regex]::Matches($value,'(?<!\d)\d+(?!\d)') |
        ForEach-Object { [int]$_.Value } | Sort-Object -Unique)
    if ($TargetProcessId -ne 0) {
        if ($ids -contains $TargetProcessId) { return $TargetProcessId }
        return 0
    }
    if ($ids.Count -eq 1) { return $ids[0] }
    if ($ids.Count -gt 1) {
        throw "Multiple processes match '$processName': $($ids -join ', '). Use -TargetProcessId."
    }
    return 0
}
function Resolve-GameActivity {
    $component = $GameActivity.Trim()
    if ([string]::IsNullOrWhiteSpace($component)) {
        $resolved = Invoke-AdbText @('-s',$Device,'shell','cmd','package',
                                     'resolve-activity','--brief',$package)
        $candidate = (($resolved -split "`r?`n") |
            Where-Object { $_ -match '^[A-Za-z0-9_.]+/[A-Za-z0-9_.$]+$' } |
            Select-Object -Last 1)
        $component = if ($null -eq $candidate) { '' } else { [string]$candidate }
    } elseif ($component.StartsWith('.')) {
        $component = "$package/$package$component"
    } elseif (-not $component.Contains('/')) {
        $component = "$package/$component"
    }
    if ($component -notmatch '^[A-Za-z0-9_.]+/[A-Za-z0-9_.$]+$') {
        throw "Unable to resolve a launcher Activity for package '$package'. Supply -GameActivity package/class."
    }
    $componentPackage = $component.Substring(0,$component.IndexOf('/'))
    if ($componentPackage -ne $package) {
        throw "Resolved Activity belongs to '$componentPackage', not selected package '$package'."
    }
    return $component
}
function Get-StartTicksFromStat([string]$Stat) {
    $stat = $Stat.Trim()
    $close = $stat.LastIndexOf(')')
    if ($close -lt 1) { throw "Malformed process stat" }
    $fields = @($stat.Substring($close + 1).Trim() -split '\s+')
    if ($fields.Count -lt 20 -or $fields[19] -notmatch '^\d+$') {
        throw "Process start ticks unavailable"
    }
    return [UInt64]$fields[19]
}
function Assert-RemoteHashes([System.Collections.IDictionary]$ExpectedByPath) {
    # One root transaction proves the complete artifact set.  The old loop
    # launched su once per file and made LDPlayer display the same grant toast
    # repeatedly without adding any verification strength.
    $paths = @($ExpectedByPath.Keys)
    $output = Invoke-AdbChecked (@('-s',$Device,'shell','sha256sum') + $paths) `
        "hash fixed G4 artifacts"
    $observed = @{}
    foreach ($line in ($output -split "`r?`n")) {
        if ($line -match '^([0-9a-fA-F]{64})\s+(.+)$') {
            $observed[$matches[2].Trim()] = $matches[1].ToLowerInvariant()
        }
    }
    foreach ($path in $paths) {
        if (-not $observed.ContainsKey($path) -or
            $observed[$path] -ne [string]$ExpectedByPath[$path]) {
            throw "Remote hash mismatch: $path"
        }
    }
}
function Read-GameIdentity {
    $gameProcessId = Get-GamePid
    if ($gameProcessId -le 0) { throw "Game process is not running" }
    # stat, TracerPid and maps belong to one identity snapshot.  Reading them
    # through one su process both lowers toast frequency and narrows the old
    # cross-command race window.
    # Do not rely on a backslash escape surviving both adb shell and su -c.
    # LDPlayer's shell consumed the slash in printf 'A9STAT\n' and emitted
    # A9STATn, which made a valid identity snapshot look malformed.
    $snapshot = Invoke-Root ("echo A9STAT; cat /proc/$gameProcessId/stat; " +
        "echo A9TRACER; grep '^TracerPid:' /proc/$gameProcessId/status; " +
        "echo A9MAPS; cat /proc/$gameProcessId/maps") "read game identity snapshot"
    if ($snapshot -notmatch '(?ms)^A9STAT\r?\n(.+?)\r?\nA9TRACER\r?\n(TracerPid:\s*\d+)\r?\nA9MAPS\r?\n(.*)$') {
        throw "Malformed game identity snapshot"
    }
    $stat = $matches[1]
    $tracer = $matches[2]
    $maps = $matches[3]
    if ($tracer -notmatch '^TracerPid:\s*0$') {
        throw "Unexpected tracer state: $tracer"
    }
    $bases = foreach ($line in ($maps -split "`n")) {
        if ($line -notmatch 'libAsphalt9\.so') { continue }
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2],16) -eq 0) {
            [Convert]::ToUInt64($matches[1],16)
        }
    }
    $bases = @($bases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base, found $($bases.Count)" }
    $gamePaths = @($maps -split "`n" | ForEach-Object {
        if ($_ -match '^\S+\s+\S+\s+\S+\s+\S+\s+\S+\s+(/\S*libAsphalt9\.so)\s*$') {
            $matches[1]
        }
    } | Sort-Object -Unique)
    if ($gamePaths.Count -ne 1 -or
        $gamePaths[0] -notmatch '^/[A-Za-z0-9_./+=:-]+$') {
        throw "Expected one safe mapped libAsphalt9 path, found $($gamePaths.Count)"
    }
    if ($maps -notmatch [Regex]::Escape($remotePayload) -or
        $maps -notmatch [Regex]::Escape($remoteBootstrap)) {
        throw "Fixed G4 payload/bootstrap are not both preloaded"
    }
    return [PSCustomObject]@{
        Pid = $gameProcessId
        StartTicks = Get-StartTicksFromStat $stat
        Base = [UInt64]$bases[0]
        GameLibraryPath = [string]$gamePaths[0]
    }
}
function Invoke-Controller([string]$Action, [object]$Identity,
                           [UInt64]$Owner, [string]$RemoteOutput,
                           [string]$RemoteReplayInput = '',
                           [UInt32]$ControllerLimit = $limit) {
    $command = "$remoteController $Action $($Identity.Pid) " +
               "$($Identity.StartTicks) $($Identity.Base.ToString('x')) " +
               "$($Owner.ToString('x')) $ControllerLimit "
    if ($RemoteReplayInput) { $command += "$RemoteReplayInput " }
    $command += "$RemoteOutput $ack"
    return Invoke-Root $command "G4 controller $Action"
}

$profilePaths = if ([string]::IsNullOrWhiteSpace($BuildProfile)) {
    @(Get-ChildItem -LiteralPath (Join-Path $root 'build\generated-profiles') `
        -Filter '*.a9profile.bin' -File | Select-Object -ExpandProperty FullName)
} else {
    @($BuildProfile)
}
if ($profilePaths.Count -eq 0) {
    throw 'No generated G8 runtime build profiles are available.'
}
$buildProfiles = @($profilePaths | ForEach-Object {
    Get-BuildProfileMetadata $_
})
$duplicateNative = @($buildProfiles | Group-Object NativeSha |
    Where-Object { $_.Count -ne 1 })
if ($duplicateNative.Count -ne 0) {
    throw "G8 profile native SHA collision: $($duplicateNative.Name -join ',')"
}

function Select-ProfileForLiveImage([object]$Identity) {
    $hashLine = Invoke-Root "sha256sum $($Identity.GameLibraryPath)" `
        'hash mapped game native library'
    if ($hashLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw 'Unable to hash mapped game native library.'
    }
    $nativeSha = $matches[1].ToLowerInvariant()
    $matchesProfile = @($buildProfiles | Where-Object {
        $_.NativeSha -eq $nativeSha
    })
    if ($matchesProfile.Count -ne 1) {
        throw "No unique generated profile for native SHA $nativeSha"
    }
    return $matchesProfile[0]
}

function Resolve-ExistingRemoteProfile {
    $hashLine = Invoke-AdbChecked @('-s',$Device,'shell','sha256sum',
                                     $remoteBuildProfile) `
        'hash selected remote build profile'
    if ($hashLine -notmatch '^([0-9a-fA-F]{64})\s+') {
        throw 'Unable to hash selected remote build profile.'
    }
    $fileSha = $matches[1].ToLowerInvariant()
    $matchesProfile = @($buildProfiles | Where-Object {
        $_.FileSha -eq $fileSha
    })
    if ($matchesProfile.Count -ne 1) {
        throw "Selected remote profile is not in the local validated set: $fileSha"
    }
    return $matchesProfile[0]
}

foreach ($path in @($buildScript,$payloadSource,$observerBuildScript,$parser,
                     $baselineVerifier,$baseline)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G4 runner input: $path"
    }
}
& $buildScript | Out-Host
if ($LASTEXITCODE -ne 0) { throw "G4 offline build failed" }
& $observerBuildScript | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Physics interval observer build failed" }
foreach ($path in @($payload,$bootstrap,$controller,$carrier)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing rebuilt G4 artifact: $path"
    }
}
python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "Known-good 900-frame baseline drift" }
$artifactHashes = [ordered]@{
    $remotePayload = Get-Sha $payload
    $remoteBootstrap = Get-Sha $bootstrap
    $remoteController = Get-Sha $controller
    $remoteCarrier = Get-Sha $carrier
    $remoteObserver = Get-Sha $observer
    '/system/lib64/libc.so' = '0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc'
}
$prepareArtifactHashes = [ordered]@{}
foreach ($entry in $artifactHashes.GetEnumerator()) {
    $prepareArtifactHashes[$entry.Key] = $entry.Value
}
foreach ($profile in $buildProfiles) {
    $prepareArtifactHashes[$profile.RemoteCandidate] = $profile.FileSha
}
if ($Mode -eq 'OfflineValidate') {
    & python -B $recordVerifierSelftest --verifier $recordVerifier
    if ($LASTEXITCODE -ne 0) { throw "G4 action recording verifier selftest failed" }
    & python -B $divergenceAnalyzerSelftest
    if ($LASTEXITCODE -ne 0) { throw "G5 replay divergence analyzer selftest failed" }
    & python -B $upstreamIntervalGuard
    if ($LASTEXITCODE -ne 0) { throw "Upstream Physics Interval semantic guard failed" }
    & python -B $lifecyclePolicy
    if ($LASTEXITCODE -ne 0) { throw "G8 lifecycle recording policy failed" }
    Write-Output "G6_GATE_OFFLINE passed=1 device_access=0 deployed=0 dynamic=NOT_RUN limit=$GateTicks physical_hooks=7 logical_boundaries=6 auto_esc=1"
    exit 0
}
if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB executable not found: $AdbPath"
}
$devices = Invoke-AdbChecked @('devices') "enumerate ADB devices"
if ($devices -notmatch "(?m)^$([Regex]::Escape($Device))\s+device$") {
    throw "Expected ready device not found: $Device"
}

if ($Mode -eq 'PrepareProcess') {
    if (-not ($AcknowledgeDeviceAccessAndFixedArtifactPush -and
              $AcknowledgeFreshDisposableGameProcess -and
              $ExecuteExactlyOneStage)) {
        throw "PrepareProcess requires its three explicit acknowledgements"
    }
    $deploy = @(
        @($payload,$remotePayload), @($bootstrap,$remoteBootstrap),
        @($controller,$remoteController), @($carrier,$remoteCarrier),
        @($observer,$remoteObserver)
    )
    foreach ($profile in $buildProfiles) {
        $deploy += ,@($profile.Path,$profile.RemoteCandidate)
    }
    foreach ($entry in $deploy) {
        Invoke-AdbChecked @('-s',$Device,'push',$entry[0],$entry[1]) "push $($entry[1])" | Out-Null
    }
    $remoteProfileCandidates = ($buildProfiles.RemoteCandidate -join ' ')
    Invoke-Root "chmod 644 $remotePayload $remoteBootstrap $remoteProfileCandidates; chmod 700 $remoteController $remoteCarrier $remoteObserver" "set G4 permissions" | Out-Null
    Assert-RemoteHashes $prepareArtifactHashes
    Invoke-AdbChecked @('-s',$Device,'shell','am','force-stop',$package) `
        "stop prior game process" | Out-Null
    Wait-Until { (Get-GamePid) -eq 0 } "prior game termination"
    Invoke-Root "rm -f $remoteCarrierLog $remoteInstallMarker; : >$remoteCarrierLog; chmod 644 $remoteCarrierLog; nohup $remoteCarrier $processName >>$remoteCarrierLog 2>&1 </dev/null &" "arm fixed G4 carrier" | Out-Null
    $activity = Resolve-GameActivity
    Invoke-AdbChecked @('-s',$Device,'shell','am','start','-n',$activity) `
        "start fresh G4 game process" | Out-Null
    Wait-Until {
        $log = Invoke-AdbText @('-s',$Device,'shell','cat',$remoteCarrierLog)
        return $log -match 'HABI1_EARLY_CARRIER passed=[01]'
    } "G4 early carrier completion"
    $carrierResult = Invoke-AdbChecked @('-s',$Device,'shell','cat',$remoteCarrierLog) `
        "read G4 carrier result"
    if ($carrierResult -notmatch 'HABI1_EARLY_CARRIER passed=1') {
        throw "G4 early carrier failed: $carrierResult"
    }
    Wait-Until {
        try { $null = Read-GameIdentity; return $true } catch { return $false }
    } "stable G4 game identity"
    $identity = Read-GameIdentity
    $selectedProfile = Select-ProfileForLiveImage $identity
    Invoke-Root "cp $($selectedProfile.RemoteCandidate) $remoteBuildProfile; chmod 644 $remoteBuildProfile" `
        'publish selected channel-agnostic build profile' | Out-Null
    $artifactHashes[$remoteBuildProfile] = $selectedProfile.FileSha
    Assert-RemoteHashes $artifactHashes
    Write-Output "G4_PROCESS_PRELOADED passed=1 package=$package process=$processName activity=$activity pid=$($identity.Pid) start_ticks=$($identity.StartTicks) native_sha256=$($selectedProfile.NativeSha) profile_sha256=$($selectedProfile.FileSha) profile_selection=native_sha early_code_patch_bytes=0 gameplay_state_writes=0 physical_hooks=0 next=cross_matrix_countdown3"
    exit 0
}

if (-not ($AcknowledgeSevenSessionHooksAndAutomaticEsc -and
          $AcknowledgeRestoreOrTerminateOnUncertainty -and
          $ExecuteExactlyOneStage)) {
    throw "$Mode requires its three explicit acknowledgements"
}
$selectedProfile = Resolve-ExistingRemoteProfile
$artifactHashes[$remoteBuildProfile] = $selectedProfile.FileSha
Assert-RemoteHashes $artifactHashes
$identity = Read-GameIdentity
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$stageLabel = if ($Mode -eq 'ExecuteRecordGate') { 'record' } `
               elseif ($Mode -eq 'ExecuteLifecycleRetryReplayGate') { 'life-retry-replay' } `
               elseif ($Mode -eq 'ExecuteLifecycleRecordGate') { 'life-record' } `
              elseif ($Mode -eq 'ExecutePauseResumeReplayGate') { 'pause-resume-replay' } `
              elseif ($Mode -eq 'ExecuteReplayGate') { 'replay' } `
              else { 'tick' }
$evidenceDir = Join-Path $root "evidence\g4-$GateTicks-$stageLabel-$stamp"
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null
$remotePrefix = "/data/local/tmp/a9tas-g4-$GateTicks-$stamp"
$remoteObservation = "$remotePrefix.a9pio2"
$localObservation = Join-Path $evidenceDir "locator.a9pio2"
$owner = [UInt64]0
$mayBeInstalled = $false
$restored = $false
$activeControllerLimit = [UInt32]$GateTicks
try {
    Invoke-Root "rm -f $remotePrefix.*" "clear unique G4 receipts" | Out-Null
    $installOutput = "$remotePrefix.install.txt"
    $install = Invoke-Controller 'install' $identity ([UInt64]1) $installOutput
    $install | Write-Output
    if ($install -notmatch 'G4_TICK_COORDINATOR_CONTROLLER passed=1 action=6') {
        throw "Five-hook passive validation was not a strict pass"
    }
    $mayBeInstalled = $true
    $passiveOutput = "$remotePrefix.passive.txt"
    $passive = Invoke-Controller 'passive' $identity ([UInt64]1) $passiveOutput
    $passive | Write-Output
    if ($passive -notmatch 'G4_PASSIVE_STATUS passed=1 installed=1') {
        throw "G4 passive target identity was not proven before countdown arming"
    }
    Invoke-Root "$remoteObserver $($identity.Pid) $($identity.Base.ToString('x')) 300 10 $remoteObservation 0 $remoteBuildProfile" "run profile-driven read-only step-options locator" | Out-Host
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteObservation,$localObservation) "pull G4 locator evidence" | Out-Null
    $observationText = (& python -B $parser $localObservation --json --profile car-physics --build-profile $selectedProfile.Path) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "Existing read-only step-options locator failed" }
    $observation = $observationText | ConvertFrom-Json
    $owners = @($observation.step_options)
    if (-not $observation.passed -or $owners.Count -ne 1 -or
        [string]$owners[0] -notmatch '^0x[0-9a-fA-F]+$') {
        throw "Step-options owner is not unique and stable"
    }
    $owner = [Convert]::ToUInt64(([string]$owners[0]).Substring(2),16)
    $repinned = Read-GameIdentity
    if ($repinned.Pid -ne $identity.Pid -or
        $repinned.StartTicks -ne $identity.StartTicks -or
        $repinned.Base -ne $identity.Base) {
        throw "Game identity changed after read-only location"
    }
    $armOutput = "$remotePrefix.arm.txt"
    $remoteReplayInput = ''
    if ($replayMode) {
        if (-not $ReplayRecording -or
            -not (Test-Path -LiteralPath $ReplayRecording -PathType Leaf)) {
            throw 'ExecuteReplayGate requires -ReplayRecording'
        }
        & python -B $recordVerifier $ReplayRecording --expected-frames $GateTicks
        if ($LASTEXITCODE -ne 0) { throw 'Replay source host verification failed' }
        $replayHash = Get-Sha $ReplayRecording
        $remoteReplayInput = "/data/local/tmp/a9tas-g5-replay-$replayHash.a9g4r2"
        Invoke-AdbChecked @('-s',$Device,'push',$ReplayRecording,$remoteReplayInput) `
            'push exact A9G4R2 replay source' | Out-Null
        $remoteHash = Invoke-AdbChecked @('-s',$Device,'shell','sha256sum',$remoteReplayInput) `
            'hash exact A9G4R2 replay source'
        if ($remoteHash -notmatch '^([0-9a-fA-F]{64})\s+' -or
            $matches[1].ToLowerInvariant() -ne $replayHash) {
            throw 'Remote replay source hash mismatch'
        }
    }
    $armAction = if ($Mode -eq 'ExecuteRecordGate') { 'record' } `
                 elseif ($lifecycleMode) { 'record-life' } `
                 elseif ($replayMode) { 'replay' } `
                 else { 'arm' }
    $expectedArmAction = if ($Mode -eq 'ExecuteRecordGate') { 7 } `
                          elseif ($lifecycleMode) { 11 } `
                          elseif ($replayMode) { 9 } `
                         else { 1 }
    try {
        $arm = Invoke-Controller $armAction $identity $owner $armOutput $remoteReplayInput
    } catch { throw }
    $arm | Write-Output
    if ($arm -notmatch "G4_TICK_COORDINATOR_CONTROLLER passed=1 action=$expectedArmAction") {
        throw "G4 arm receipt was not a strict pass"
    }
    Invoke-AdbChecked @('-s',$Device,'shell','input keyevent 111') "send one ESC" | Out-Null
    $statusOutput = "$remotePrefix.status.txt"
    $pauseProbeOutput = $null
    $progressWaitOutput = $null
    if ($lifecycleMode) {
        # One device-side read-only waiter keeps /proc/PID/mem open while the
        # user drives and selects Retry. It causes no per-frame ptrace stop or
        # game write and avoids repeated root-grant toasts.
        $status = Invoke-Controller 'wait' $identity $owner $statusOutput
    } elseif ($Mode -eq 'ExecutePauseResumeReplayGate') {
        # Pause once after the authoritative runtime has published at least
        # 120 packets. Do not infer race progress from wall-clock time.
        # The controller then holds one read-only /proc/PID/mem descriptor and
        # proves two samples two seconds apart are identical for every
        # packet-consuming counter. No payload command or game write occurs.
        $progressWaitOutput = "$remotePrefix.progress-wait.txt"
        $progressWait = Invoke-Controller 'wait-progress' $identity $owner `
            $progressWaitOutput $null ([UInt32]$GateTicks)
        $progressWait | Write-Output
        if ($progressWait -notmatch 'G8_PROGRESS_WAIT passed=1 minimum=120 ' -or
            $progressWait -notmatch 'ticks=(\d+) ' -or
            [int]$matches[1] -lt 120 -or [int]$matches[1] -ge $GateTicks) {
            throw "G8 replay did not reach authoritative pause threshold: $progressWait"
        }
        Invoke-AdbChecked @('-s',$Device,'shell','input','keyevent','111') `
            'pause active replay once' | Out-Null
        Start-Sleep -Milliseconds 750
        $pauseProbeOutput = "$remotePrefix.pause-probe.txt"
        $pauseProbe = Invoke-Controller 'pause-probe' $identity $owner `
            $pauseProbeOutput $null ([UInt32]$GateTicks)
        $pauseProbe | Write-Output
        if ($pauseProbe -notmatch 'G8_PAUSE_PROBE passed=1 ' -or
            $pauseProbe -notmatch 'ticks=(\d+),(\d+) ' -or
            [int]$matches[1] -ne [int]$matches[2] -or
            [int]$matches[1] -le 0 -or [int]$matches[1] -ge $GateTicks) {
            throw "G8 pause probe did not prove a stable mid-replay tick: $pauseProbe"
        }
        Invoke-AdbChecked @('-s',$Device,'shell','input','keyevent','111') `
            'resume paused replay once' | Out-Null
        $runMilliseconds = 10000 + [Math]::Max(
            2500, [int][Math]::Ceiling($GateTicks / 45.0 * 1000.0) + 1000)
        Start-Sleep -Milliseconds $runMilliseconds
        $status = Invoke-Controller 'status' $identity $owner $statusOutput
    } else {
        # The runtime is armed in lifecycle phase 2 and intentionally ignores
        # countdown calls. Fixed-length legacy Gates retain their proven wait.
        $raceStartBudgetMilliseconds = 10000
        $tickBudgetMilliseconds = [Math]::Max(
            2500, [int][Math]::Ceiling($GateTicks / 45.0 * 1000.0) + 1000)
        $runMilliseconds = $raceStartBudgetMilliseconds + $tickBudgetMilliseconds
        Start-Sleep -Milliseconds $runMilliseconds
        $status = Invoke-Controller 'status' $identity $owner $statusOutput
    }
    if ((Get-GamePid) -ne $identity.Pid) { throw "Game process changed during G4 Gate" }
    $status | Write-Output
    $statusFailure = $null
    $actualTicks = $GateTicks
    if ($lifecycleMode) {
        if ($status -notmatch '(^|\s)complete=1\s+ticks=(\d+)(\s|$)') {
            $statusFailure = "G8 lifecycle receipt has no completed actual count: $status"
        } else {
            $actualTicks = [int]$matches[2]
            if ($actualTicks -le 0 -or $actualTicks -ge $GateTicks) {
                $statusFailure = "G8 lifecycle Gate must end naturally before capacity: actual=$actualTicks capacity=$GateTicks"
            }
        }
    }
    $requiredStatus = @('passed=1', 'complete=1', "ticks=$actualTicks",
                        "begin=$actualTicks", "interval=$actualTicks",
                        "final=$actualTicks", "end=$actualTicks", 'error=0')
    if ($lifecycleMode) {
        $requiredStatus += 'completion=1,2'
    }
    foreach ($token in $requiredStatus) {
        if ($null -eq $statusFailure -and
            $status -notmatch "(^|\s)$([Regex]::Escape($token))(\s|$)") {
            $statusFailure = "G4 receipt missing '$token': $status"
        }
    }
    $recordOutput = $null
    $remoteRecording = $null
    $localRecording = $null
    if ($null -eq $statusFailure -and
         ($Mode -eq 'ExecuteRecordGate' -or $lifecycleMode)) {
$remoteRecording = "$remotePrefix.a9g4r2"
$localRecording = Join-Path $evidenceDir 'recording.a9g4r2'
        $recordOutput = Invoke-Controller 'dump' $identity $owner $remoteRecording
        $recordOutput | Write-Output
        if ($recordOutput -notmatch "G4_RECORD_DUMP passed=1 frames=$actualTicks ") {
            throw "G4 action recording dump was not a strict pass"
        }
        Invoke-AdbChecked @('-s',$Device,'pull',$remoteRecording,$localRecording) `
            "pull G4 action recording" | Out-Null
        & python -B $recordVerifier `
            $localRecording --expected-frames $actualTicks
        if ($LASTEXITCODE -ne 0) { throw "G4 action recording host verification failed" }
        if ($Mode -eq 'ExecuteLifecycleRetryReplayGate') {
            # Do not guess how long Retry loading takes and do not send ESC
            # while lifecycle is still in the transition state. The seven
            # hooks remain exact pass-through with enabled=0. The user pauses
            # the new race at countdown 3, then the caller creates this unique
            # host-owned trigger file to authorize phase-2 relocation.
            $remoteRearmTrigger = "$remotePrefix.phase2.ready"
            Write-Output "G8_ARCHIVE_READY_WAITING_FOR_PHASE2 actual_ticks=$actualTicks trigger=$remoteRearmTrigger user_action=pause_retry_countdown3"
            $triggerDeadline = [DateTime]::UtcNow.AddMinutes(10)
            $triggered = $false
            while ([DateTime]::UtcNow -lt $triggerDeadline) {
                & $AdbPath -s $Device shell test -f $remoteRearmTrigger 2>$null
                if ($LASTEXITCODE -eq 0) { $triggered = $true; break }
                Start-Sleep -Milliseconds 250
            }
            if (-not $triggered) {
                throw 'Timed out waiting for explicit Retry phase-2 pause trigger'
            }
            Invoke-AdbChecked @('-s',$Device,'shell','rm','-f',
                $remoteRearmTrigger) 'consume Retry phase-2 trigger' | Out-Null
            $retryObservation = "$remotePrefix.retry.a9pio2"
            $localRetryObservation = Join-Path $evidenceDir 'retry-locator.a9pio2'
            Invoke-Root "rm -f $retryObservation; $remoteObserver $($identity.Pid) $($identity.Base.ToString('x')) 300 10 $retryObservation 0 $remoteBuildProfile" `
                'locate Retry StepOptions owner read-only' | Out-Host
            Invoke-AdbChecked @('-s',$Device,'pull',$retryObservation,
                $localRetryObservation) 'pull Retry locator evidence' | Out-Null
            $retryObservationText = (& python -B $parser $localRetryObservation `
                --json --profile car-physics `
                --build-profile $selectedProfile.Path) -join "`n"
            if ($LASTEXITCODE -ne 0) { throw 'Retry StepOptions locator failed' }
            $retryParsed = $retryObservationText | ConvertFrom-Json
            $retryOwners = @($retryParsed.step_options)
            if (-not $retryParsed.passed -or $retryOwners.Count -ne 1 -or
                [string]$retryOwners[0] -notmatch '^0x[0-9a-fA-F]+$') {
                throw 'Retry StepOptions owner is not unique and stable'
            }
            $owner = [Convert]::ToUInt64(
                ([string]$retryOwners[0]).Substring(2),16)
            $retryIdentity = Read-GameIdentity
            if ($retryIdentity.Pid -ne $identity.Pid -or
                $retryIdentity.StartTicks -ne $identity.StartTicks -or
                $retryIdentity.Base -ne $identity.Base) {
                throw 'Game identity changed before same-process rearm'
            }
            $rearmOutput = "$remotePrefix.rearm.txt"
            $rearm = Invoke-Controller 'rearm-replay' $identity $owner `
                $rearmOutput $remoteRecording ([UInt32]$actualTicks)
            $rearm | Write-Output
            if ($rearm -notmatch 'G4_TICK_COORDINATOR_CONTROLLER passed=1 action=13 ' -or
                $rearm -notmatch 'status=1 ticks=0 ') {
                throw "G8 generation+1 rearm was not a strict tick-0 pass: $rearm"
            }
            $activeControllerLimit = [UInt32]$actualTicks
            Invoke-AdbChecked @('-s',$Device,'shell','input','keyevent','111') `
                'resume generation+1 Retry replay' | Out-Null
            $replayBudgetMilliseconds = 10000 + [Math]::Max(
                2500, [int][Math]::Ceiling($actualTicks / 45.0 * 1000.0) + 1000)
            Start-Sleep -Milliseconds $replayBudgetMilliseconds
            $replayStatusOutput = "$remotePrefix.replay-status.txt"
            $replayStatus = Invoke-Controller 'status' $identity $owner `
                $replayStatusOutput '' ([UInt32]$actualTicks)
            $replayStatus | Write-Output
            foreach ($token in @('passed=1','complete=1',"ticks=$actualTicks",
                                  "begin=$actualTicks","interval=$actualTicks",
                                  "final=$actualTicks","end=$actualTicks",
                                  'error=0','completion=0,1')) {
                if ($replayStatus -notmatch
                    "(^|\s)$([Regex]::Escape($token))(\s|$)") {
                    throw "G8 Retry replay receipt missing '$token': $replayStatus"
                }
            }
            $remoteDiagnostic = "$remotePrefix.retry.a9g5d1"
            $remoteTickReceipts = "$remoteDiagnostic.ticks.csv"
            $localDiagnostic = Join-Path $evidenceDir `
                'retry-natural-before-correction.a9g5d1'
            $localTickReceipts = Join-Path $evidenceDir `
                'retry-tick-receipts.csv'
            $localDivergence = Join-Path $evidenceDir `
                'retry-first-divergence.json'
            $diagnosticOutput = Invoke-Controller 'diff' $identity $owner `
                $remoteDiagnostic '' ([UInt32]$actualTicks)
            $diagnosticOutput | Write-Output
            if ($diagnosticOutput -notmatch
                "G5_REPLAY_DIAGNOSTIC_DUMP passed=1 frames=$actualTicks ") {
                throw 'G8 Retry replay diagnostic dump was not a strict pass'
            }
            Invoke-AdbChecked @('-s',$Device,'pull',$remoteDiagnostic,
                $localDiagnostic) 'pull Retry replay diagnostic' | Out-Null
            Invoke-AdbChecked @('-s',$Device,'pull',$remoteTickReceipts,
                $localTickReceipts) 'pull Retry tick receipts' | Out-Null
            & python -B $divergenceAnalyzer $localDiagnostic $localRecording `
                --tick-receipts $localTickReceipts --output $localDivergence
            if ($LASTEXITCODE -ne 0) {
                throw 'G8 Retry replay divergence analysis failed'
            }
        }
    } elseif ($null -eq $statusFailure -and $replayMode) {
        $remoteDiagnostic = "$remotePrefix.a9g5d1"
        $remoteTickReceipts = "$remoteDiagnostic.ticks.csv"
        $localDiagnostic = Join-Path $evidenceDir 'natural-before-correction.a9g5d1'
        $localTickReceipts = Join-Path $evidenceDir 'tick-receipts.csv'
        $localDivergence = Join-Path $evidenceDir 'first-divergence.json'
        $diagnosticOutput = Invoke-Controller 'diff' $identity $owner $remoteDiagnostic
        $diagnosticOutput | Write-Output
        $diagnosticOutput | Set-Content -LiteralPath `
            (Join-Path $evidenceDir 'diagnostic-dump.txt') -Encoding utf8
        if ($diagnosticOutput -notmatch "G5_REPLAY_DIAGNOSTIC_DUMP passed=1 frames=$GateTicks ") {
            throw "G5 replay diagnostic dump was not a strict pass"
        }
        Invoke-AdbChecked @('-s',$Device,'pull',$remoteDiagnostic,$localDiagnostic) `
            "pull G5 replay natural-before-correction diagnostic" | Out-Null
        Invoke-AdbChecked @('-s',$Device,'pull',$remoteTickReceipts,$localTickReceipts) `
            "pull G5 existing tick receipts" | Out-Null
        & python -B $divergenceAnalyzer $localDiagnostic $ReplayRecording `
            --tick-receipts $localTickReceipts --output $localDivergence
        if ($LASTEXITCODE -ne 0) { throw "G5 replay divergence analysis failed" }
    }
    $restoreOutput = "$remotePrefix.restore.txt"
    $restore = Invoke-Controller 'restore' $identity $owner $restoreOutput '' `
        $activeControllerLimit
    $restore | Write-Output
    if ($restore -notmatch 'G4_TICK_COORDINATOR_CONTROLLER passed=1 action=3') {
        throw "G4 restore receipt was not a strict pass"
    }
    $restored = $true
    # Restore the temporary hooks before interpreting the semantic receipt.
    # A failed receipt is diagnostic evidence, not a reason to leave a live
    # process patched until the fallback path runs.
    if ($null -ne $statusFailure) { throw $statusFailure }
    foreach ($entry in @(
        @($installOutput,(Join-Path $evidenceDir 'install.txt')),
        @($passiveOutput,(Join-Path $evidenceDir 'passive.txt')),
        @($armOutput,(Join-Path $evidenceDir 'arm.txt')),
        @($statusOutput,(Join-Path $evidenceDir 'status.txt')),
        @($restoreOutput,(Join-Path $evidenceDir 'restore.txt')),
        @($remoteCarrierLog,(Join-Path $evidenceDir 'carrier.log')))) {
        Invoke-AdbChecked @('-s',$Device,'pull',$entry[0],$entry[1]) "pull G4 evidence" | Out-Null
    }
    if ($Mode -eq 'ExecuteLifecycleRetryReplayGate') {
        foreach ($entry in @(
            @($rearmOutput,(Join-Path $evidenceDir 'rearm.txt')),
            @($replayStatusOutput,(Join-Path $evidenceDir 'replay-status.txt')))) {
            Invoke-AdbChecked @('-s',$Device,'pull',$entry[0],$entry[1]) `
                'pull G8 Retry replay evidence' | Out-Null
        }
    }
    if ($Mode -eq 'ExecutePauseResumeReplayGate') {
        foreach ($entry in @(
            @($progressWaitOutput,(Join-Path $evidenceDir 'progress-wait.txt')),
            @($pauseProbeOutput,(Join-Path $evidenceDir 'pause-probe.txt')))) {
            Invoke-AdbChecked @('-s',$Device,'pull',$entry[0],$entry[1]) `
                'pull G8 pause/resume evidence' | Out-Null
        }
    }
    $finalIdentity = Read-GameIdentity
    if ($finalIdentity.Pid -ne $identity.Pid -or
        $finalIdentity.StartTicks -ne $identity.StartTicks) {
        throw "Game identity changed after G4 restore"
    }
    if ($Mode -eq 'ExecuteLifecycleRetryReplayGate') {
Write-Output "G8_LIFECYCLE_RETRY_REPLAY_GATE_PASSED actual_ticks=$actualTicks generation=1_to_2 replay_tick0=authoritative source_archive_exact=1 session_hooks_reused=7 session_hooks_restored=7 recording=$localRecording first_divergence=$localDivergence evidence=$evidenceDir"
    } elseif ($Mode -eq 'ExecuteLifecycleRecordGate') {
Write-Output "G8_LIFECYCLE_RECORD_GATE_PASSED actual_ticks=$actualTicks capacity=$GateTicks completion=race_lifecycle action_fields=brake,steering,nitro barrel_fields=rbx2,angular3 physics_fields=transform64,linear12 tool_fixed_delta_writes=$actualTicks ptrace_hot_path=0 session_hooks_restored=7 recording=$localRecording evidence=$evidenceDir"
    } elseif ($Mode -eq 'ExecuteRecordGate') {
Write-Output "G6_RECORD_GATE_PASSED ticks=$GateTicks action_fields=brake,steering,nitro barrel_fields=rbx2,angular3 physics_fields=transform64,linear12 interval_recording=diagnostic_calls physics_valid=1 tool_fixed_delta_writes=$GateTicks ptrace_hot_path=0 session_hooks_restored=7 recording=$localRecording evidence=$evidenceDir"
    } elseif ($Mode -eq 'ExecutePauseResumeReplayGate') {
        Write-Output "G8_PAUSE_RESUME_REPLAY_GATE_PASSED ticks=$GateTicks pause_samples=stable packet_advance_during_pause=0 resume_continuity=source_bound session_hooks_restored=7 source=$ReplayRecording evidence=$evidenceDir"
    } elseif ($Mode -eq 'ExecuteReplayGate') {
        Write-Output "G6_REPLAY_GATE_PASSED ticks=$GateTicks action_fields=brake,steering,nitro barrel_tails=after_original_rbx2,angular3 physics_correction=conditional_64_plus_12 natural_before_correction=A9G5D1 first_divergence=$localDivergence interval_semantics=alu_default_passthrough ptrace_hot_path=0 session_hooks_restored=7 source=$ReplayRecording evidence=$evidenceDir"
    } else {
        Write-Output "G6_TICK_GATE_PASSED ticks=$GateTicks logical_boundaries=6 physical_hooks_installed=7 naturally_called_hooks=receipt_dependent gameplay_state_writes=0 ptrace_hot_path=0 session_hooks_restored=7 evidence=$evidenceDir"
    }
}
finally {
    # Preserve every receipt that exists even when the controller has already
    # terminated an uncertain disposable process. A failed arm must leave an
    # actionable diagnosis instead of only an empty evidence directory.
    foreach ($entry in @(
        @("$remotePrefix.install.txt",(Join-Path $evidenceDir 'install.txt')),
        @("$remotePrefix.passive.txt",(Join-Path $evidenceDir 'passive.txt')),
        @("$remotePrefix.arm.txt",(Join-Path $evidenceDir 'arm.txt')),
        @("$remotePrefix.status.txt",(Join-Path $evidenceDir 'status.txt')),
        @("$remotePrefix.restore.txt",(Join-Path $evidenceDir 'restore.txt')),
        @("$remotePrefix.rollback.txt",(Join-Path $evidenceDir 'rollback.txt')))) {
        try {
            # adb pull itself is the existence/readability check.  A separate
            # root `test -f` doubled the commands and produced six cosmetic
            # superuser toasts on every cleanup path.
            Invoke-AdbChecked @('-s',$Device,'pull',$entry[0],$entry[1]) `
                "pull G4 failure evidence" | Out-Null
        } catch { }
    }
    if ($mayBeInstalled -and -not $restored -and
        (Get-GamePid) -eq $identity.Pid) {
        try {
            $rollbackOutput = "$remotePrefix.rollback.txt"
            $rollbackOwner = if ($owner -ne 0) { $owner } else { [UInt64]1 }
            $rollback = Invoke-Controller 'restore' $identity $rollbackOwner `
                $rollbackOutput '' $activeControllerLimit
            $rollback | Out-Host
            if ($rollback -notmatch 'G4_TICK_COORDINATOR_CONTROLLER passed=1 action=3') {
                throw "G4 rollback receipt was not a strict pass"
            }
            Invoke-AdbChecked @('-s',$Device,'pull',$rollbackOutput,
                (Join-Path $evidenceDir 'rollback.txt')) `
                "pull G4 rollback evidence" | Out-Null
            $restored = $true
            Write-Warning "G4 Gate failed; all five session hooks were restored"
        } catch {
            Write-Warning "G4 restore could not be proven; terminating only this disposable process"
            try {
                Invoke-AdbChecked @('-s',$Device,'shell','am','force-stop',$package) `
                    "terminate uncertain G4 process" | Out-Null
            } catch { }
        }
    }
}

# Optional evidence pulls in the finally block intentionally ignore missing
# files.  adb still leaves its native exit code in $LASTEXITCODE, which made a
# successful Gate look like a process failure to callers.  This line is only
# reached when the protected body completed without a terminating exception.
$global:LASTEXITCODE = 0
