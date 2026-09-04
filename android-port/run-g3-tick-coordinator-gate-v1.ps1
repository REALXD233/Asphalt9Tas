param(
    [ValidateSet("OfflineValidate", "PrepareProcess", "ExecuteTickGate")]
    [string]$Mode = "OfflineValidate",
    [ValidateSet(5, 60, 900)][int]$GateTicks = 5,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [ValidateRange(10000, 120000)][int]$TimeoutMs = 60000,
    [switch]$AcknowledgeDeviceAccessAndFixedArtifactPush,
    [switch]$AcknowledgeFreshDisposableGameProcess,
    [switch]$AcknowledgeThreeSessionHooksAndAutomaticEsc,
    [switch]$AcknowledgeRestoreOrTerminateOnUncertainty,
    [switch]$ExecuteExactlyOneStage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildScript = Join-Path $root "build-g3-tick-coordinator-live-gate-v1.ps1"
$payloadDir = Join-Path $root "build\g3-multi-hook-runtime-v1"
$buildDir = Join-Path $root "build\g3-tick-coordinator-live-gate-v1"
$payloadSource = Join-Path $root "src\payload_g3_multi_hook_runtime_v1.cpp"
$payload = Join-Path $payloadDir "liba9tas_g3_multi_hook_runtime_v1.so"
$bootstrap = Join-Path $buildDir "liba9tas_g3_tick_coordinator_bootstrap_v1.so"
$controller = Join-Path $buildDir "a9tas_g3_tick_coordinator_controller_v1"
$sourceHash = (Get-FileHash -LiteralPath $payloadSource -Algorithm SHA256).Hash.ToLowerInvariant()
$carrier = Join-Path $buildDir "a9tas_habi1_early_carrier_$($sourceHash.Substring(0,16))"
$observer = Join-Path $root "build\physics-interval-readonly-v1\a9tas_physics_interval_readonly_observer_v1"
$parser = Join-Path $root "tools\parse_physics_interval_readonly_v1.py"
$baselineVerifier = Join-Path $root "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $root "baselines\known_good_900_exact_interval_v2.json"

$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$remotePayload = "/data/local/tmp/liba9tas_g3_multi_hook_runtime_v1.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_g3_tick_coordinator_bootstrap_v1.so"
$remoteController = "/data/local/tmp/a9tas_g3_tick_coordinator_controller_v1"
$remoteCarrier = "/data/local/tmp/a9tas_habi1_early_carrier_$($sourceHash.Substring(0,16))"
$remoteObserver = "/data/local/tmp/a9tas_physics_interval_readonly_observer_v1"
$remoteCarrierLog = "/data/local/tmp/a9tas-g3-early-carrier.log"
$remoteInstallMarker = "/data/local/tmp/a9tas-g3-install-proven-begin-v1"
$ack = "I_ACCEPT_G3_TICK_COORDINATOR_V1"
$limit = $GateTicks

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
function Wait-Until([scriptblock]$Condition, [string]$Label) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out: $Label"
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
    return [UInt64]$fields[19]
}
function Assert-RemoteHash([string]$Path, [string]$Expected) {
    $line = Invoke-Root "sha256sum $Path" "hash $Path"
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Path"
    }
}
function Assert-TracerClear([int]$GameProcessId) {
    $line = Invoke-Root "grep '^TracerPid:' /proc/$GameProcessId/status" "read tracer state"
    if ($line -notmatch '^TracerPid:\s*0$') { throw "Unexpected tracer state: $line" }
}
function Read-GameIdentity {
    $gameProcessId = Get-GamePid
    if ($gameProcessId -le 0) { throw "Game process is not running" }
    $maps = Invoke-Root "cat /proc/$gameProcessId/maps" "read game maps"
    $bases = foreach ($line in ($maps -split "`n")) {
        if ($line -notmatch 'libAsphalt9\.so') { continue }
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2],16) -eq 0) {
            [Convert]::ToUInt64($matches[1],16)
        }
    }
    $bases = @($bases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base, found $($bases.Count)" }
    if ($maps -notmatch [Regex]::Escape($remotePayload) -or
        $maps -notmatch [Regex]::Escape($remoteBootstrap)) {
        throw "Fixed G3 payload/bootstrap are not both preloaded"
    }
    Assert-TracerClear $gameProcessId
    return [PSCustomObject]@{
        Pid = $gameProcessId
        StartTicks = Get-StartTicks $gameProcessId
        Base = [UInt64]$bases[0]
    }
}
function Invoke-Controller([string]$Action, [object]$Identity,
                           [UInt64]$Owner, [string]$RemoteOutput) {
    $command = "$remoteController $Action $($Identity.Pid) " +
               "$($Identity.StartTicks) $($Identity.Base.ToString('x')) " +
               "$($Owner.ToString('x')) $limit $RemoteOutput $ack"
    return Invoke-Root $command "G3 controller $Action"
}

foreach ($path in @($buildScript,$payloadSource,$observer,$parser,
                     $baselineVerifier,$baseline)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G3 runner input: $path"
    }
}
& $buildScript | Out-Host
if ($LASTEXITCODE -ne 0) { throw "G3 offline build failed" }
foreach ($path in @($payload,$bootstrap,$controller,$carrier)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing rebuilt G3 artifact: $path"
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
if ($Mode -eq 'OfflineValidate') {
    Write-Output "G3_GATE_OFFLINE passed=1 device_access=0 deployed=0 dynamic=NOT_RUN limit=$GateTicks physical_hooks=3 logical_boundaries=4 auto_esc=1"
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
    foreach ($entry in $deploy) {
        Invoke-AdbChecked @('-s',$Device,'push',$entry[0],$entry[1]) "push $($entry[1])" | Out-Null
    }
    Invoke-Root "chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteCarrier $remoteObserver" "set G3 permissions" | Out-Null
    foreach ($entry in $artifactHashes.GetEnumerator()) {
        Assert-RemoteHash $entry.Key $entry.Value
    }
    Invoke-Root "am force-stop $package" "stop prior game process" | Out-Null
    Wait-Until { (Get-GamePid) -eq 0 } "prior game termination"
    Invoke-Root "rm -f $remoteCarrierLog $remoteInstallMarker; nohup $remoteCarrier >$remoteCarrierLog 2>&1 </dev/null &" "arm fixed G3 carrier" | Out-Null
    Invoke-Root "am start -n $activity" "start fresh G3 game process" | Out-Null
    Wait-Until {
        $log = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat $remoteCarrierLog 2>/dev/null'")
        return $log -match 'HABI1_EARLY_CARRIER passed=[01]'
    } "G3 early carrier completion"
    $carrierResult = Invoke-Root "cat $remoteCarrierLog" "read G3 carrier result"
    if ($carrierResult -notmatch 'HABI1_EARLY_CARRIER passed=1') {
        throw "G3 early carrier failed: $carrierResult"
    }
    Wait-Until {
        try { $null = Read-GameIdentity; return $true } catch { return $false }
    } "stable G3 game identity"
    $identity = Read-GameIdentity
    Write-Output "G3_PROCESS_PRELOADED passed=1 pid=$($identity.Pid) start_ticks=$($identity.StartTicks) early_code_patch_bytes=0 gameplay_state_writes=0 physical_hooks=0 next=ancient_ruins_zl1_countdown3"
    exit 0
}

if (-not ($AcknowledgeThreeSessionHooksAndAutomaticEsc -and
          $AcknowledgeRestoreOrTerminateOnUncertainty -and
          $ExecuteExactlyOneStage)) {
    throw "ExecuteTickGate requires its three explicit acknowledgements"
}
foreach ($entry in $artifactHashes.GetEnumerator()) {
    Assert-RemoteHash $entry.Key $entry.Value
}
$identity = Read-GameIdentity
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$evidenceDir = Join-Path $root "evidence\g3-$GateTicks-tick-$stamp"
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null
$remotePrefix = "/data/local/tmp/a9tas-g3-$GateTicks-$stamp"
$remoteObservation = "$remotePrefix.a9pio2"
$localObservation = Join-Path $evidenceDir "locator.a9pio2"
$owner = [UInt64]0
$mayBeInstalled = $false
$restored = $false
try {
    Invoke-Root "rm -f $remotePrefix.*" "clear unique G3 receipts" | Out-Null
    $installOutput = "$remotePrefix.install.txt"
    $install = Invoke-Controller 'install' $identity ([UInt64]1) $installOutput
    $install | Write-Output
    if ($install -notmatch 'G3_TICK_COORDINATOR_CONTROLLER passed=1 action=6') {
        throw "Three-hook passive validation was not a strict pass"
    }
    $mayBeInstalled = $true
    $passiveOutput = "$remotePrefix.passive.txt"
    $passive = Invoke-Controller 'passive' $identity ([UInt64]1) $passiveOutput
    $passive | Write-Output
    if ($passive -notmatch 'G3_PASSIVE_STATUS passed=1 installed=1') {
        throw "G3 passive target identity was not proven before countdown arming"
    }
    Invoke-Root "$remoteObserver $($identity.Pid) $($identity.Base.ToString('x')) 300 10 $remoteObservation" "run existing read-only step-options locator" | Out-Host
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteObservation,$localObservation) "pull G3 locator evidence" | Out-Null
    $observationText = (& python -B $parser $localObservation --json --profile car-physics) -join "`n"
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
    try {
        $arm = Invoke-Controller 'arm' $identity $owner $armOutput
    } catch { throw }
    $arm | Write-Output
    if ($arm -notmatch 'G3_TICK_COORDINATOR_CONTROLLER passed=1 action=1') {
        throw "G3 arm receipt was not a strict pass"
    }
    Invoke-AdbChecked @('-s',$Device,'shell','input keyevent 111') "send one ESC" | Out-Null
    $runMilliseconds = [Math]::Max(2500, [int][Math]::Ceiling($GateTicks / 45.0 * 1000.0) + 1000)
    Start-Sleep -Milliseconds $runMilliseconds
    if ((Get-GamePid) -ne $identity.Pid) { throw "Game process changed during G3 Gate" }
    $statusOutput = "$remotePrefix.status.txt"
    $status = Invoke-Controller 'status' $identity $owner $statusOutput
    $status | Write-Output
    foreach ($token in @('passed=1', 'complete=1', "ticks=$GateTicks",
                          "begin=$GateTicks", "interval=$GateTicks",
                          "final=$GateTicks", "end=$GateTicks", 'error=0')) {
        if ($status -notmatch "(^|\s)$([Regex]::Escape($token))(\s|$)") {
            throw "G3 receipt missing '$token': $status"
        }
    }
    $restoreOutput = "$remotePrefix.restore.txt"
    $restore = Invoke-Controller 'restore' $identity $owner $restoreOutput
    $restore | Write-Output
    if ($restore -notmatch 'G3_TICK_COORDINATOR_CONTROLLER passed=1 action=3') {
        throw "G3 restore receipt was not a strict pass"
    }
    $restored = $true
    foreach ($entry in @(
        @($installOutput,(Join-Path $evidenceDir 'install.txt')),
        @($passiveOutput,(Join-Path $evidenceDir 'passive.txt')),
        @($armOutput,(Join-Path $evidenceDir 'arm.txt')),
        @($statusOutput,(Join-Path $evidenceDir 'status.txt')),
        @($restoreOutput,(Join-Path $evidenceDir 'restore.txt')),
        @($remoteCarrierLog,(Join-Path $evidenceDir 'carrier.log')))) {
        Invoke-AdbChecked @('-s',$Device,'pull',$entry[0],$entry[1]) "pull G3 evidence" | Out-Null
    }
    $finalIdentity = Read-GameIdentity
    if ($finalIdentity.Pid -ne $identity.Pid -or
        $finalIdentity.StartTicks -ne $identity.StartTicks) {
        throw "Game identity changed after G3 restore"
    }
    Write-Output "G3_TICK_GATE_PASSED ticks=$GateTicks logical_boundaries=4 physical_hooks=3 natural_originals=3 gameplay_state_writes=0 ptrace_hot_path=0 session_hooks_restored=3 evidence=$evidenceDir"
}
finally {
    if ($mayBeInstalled -and -not $restored -and
        (Get-GamePid) -eq $identity.Pid) {
        try {
            $rollbackOutput = "$remotePrefix.rollback.txt"
            $rollbackOwner = if ($owner -ne 0) { $owner } else { [UInt64]1 }
            Invoke-Controller 'restore' $identity $rollbackOwner $rollbackOutput | Out-Host
            $restored = $true
            Write-Warning "G3 Gate failed; all three session hooks were restored"
        } catch {
            Write-Warning "G3 restore could not be proven; terminating only this disposable process"
            try { Invoke-Root "am force-stop $package" "terminate uncertain G3 process" | Out-Null } catch { }
        }
    }
}
