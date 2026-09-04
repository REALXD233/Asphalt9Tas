param(
    [ValidateSet("OfflineValidate", "PrepareProcess", "ExecuteCallGate",
                 "ExecuteFiveCallGate")]
    [string]$Mode = "OfflineValidate",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [ValidateRange(10000, 120000)][int]$TimeoutMs = 60000,
    [ValidateSet(5, 60, 900)][int]$CallLimit = 5,
    [ValidateRange(0, 30000)][int]$CompletionWaitMs = 0,
    [switch]$AcknowledgeDeviceAccessAndFixedArtifactPush,
    [switch]$AcknowledgeFreshDisposableGameProcess,
    [switch]$AcknowledgeSinglePermanentHookAndAutomaticEsc,
    [switch]$AcknowledgeRestoreOrTerminateOnUncertainty,
    [switch]$ExecuteExactlyOneStage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildScript = Join-Path $root "build-g2-physics-interval-passthrough-v1.ps1"
$buildDir = Join-Path $root "build\g2-physics-interval-passthrough-v1"
$payload = Join-Path $buildDir "liba9tas_g2_physics_interval_passthrough_v1.so"
$bootstrap = Join-Path $buildDir "liba9tas_g2_physics_interval_bootstrap_v1.so"
$controller = Join-Path $buildDir "a9tas_g2_physics_interval_controller_v1"
$carrier = Join-Path $buildDir "a9tas_habi1_early_carrier_acca657729c89983"
$observer = Join-Path $root "build\physics-interval-readonly-v1\a9tas_physics_interval_readonly_observer_v1"
$parser = Join-Path $root "tools\parse_physics_interval_readonly_v1.py"
$baselineVerifier = Join-Path $root "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $root "baselines\known_good_900_exact_interval_v2.json"

$expectedPayloadHash = "6fdd266f00184950bfe2f0dbdc6a58040254ebd0afca50ccbbe89c53f5c14737"
$expectedBootstrapHash = "99c08feaac93c3ef7e7062830c3a7f31a7e59ccb86535e5750a0951e859afb4c"
$expectedControllerHash = "287e1a918fad40300c35bf5a5301cd4bb11e4ef9b323397997b497681e7b7e2d"
$expectedCarrierHash = "b5e1aff4f952fd783a98c6b50feb64ce4055617261274688295ca7d1613ee89b"
$expectedObserverHash = "e8e75ac176650ba48962774a8ad6a209d00f27592d09c935e739722c785a8005"
$expectedLibcHash = "0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc"

$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$remotePayload = "/data/local/tmp/liba9tas_g2_physics_interval_passthrough_v1.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_g2_physics_interval_bootstrap_v1.so"
$remoteController = "/data/local/tmp/a9tas_g2_physics_interval_controller_v1"
$remoteCarrier = "/data/local/tmp/a9tas_habi1_early_carrier_acca657729c89983"
$remoteObserver = "/data/local/tmp/a9tas_physics_interval_readonly_observer_v1"
$remoteCarrierLog = "/data/local/tmp/a9tas-g2-early-carrier.log"
$ack = "I_ACCEPT_G2_PHYSICS_INTERVAL_V1"
$limit = $CallLimit

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
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
            if ([Convert]::ToUInt64($matches[2], 16) -eq 0) {
                [Convert]::ToUInt64($matches[1], 16)
            }
        }
    }
    $bases = @($bases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one libAsphalt9 base, found $($bases.Count)" }
    if ($maps -notmatch [Regex]::Escape($remotePayload) -or
        $maps -notmatch [Regex]::Escape($remoteBootstrap)) {
        throw "Fixed G2 payload/bootstrap are not both preloaded"
    }
    Assert-TracerClear $gameProcessId
    return [PSCustomObject]@{
        Pid = $gameProcessId
        StartTicks = Get-StartTicks $gameProcessId
        Base = [UInt64]$bases[0]
    }
}
function Invoke-Controller(
    [string]$Action, [object]$Identity, [UInt64]$Owner,
    [string]$RemoteOutput) {
    $baseHex = $Identity.Base.ToString('x')
    $ownerHex = $Owner.ToString('x')
    $command = "$remoteController $Action $($Identity.Pid) " +
               "$($Identity.StartTicks) $baseHex $ownerHex $limit " +
               "$RemoteOutput $ack"
    return Invoke-Root $command "G2 controller $Action"
}
function Assert-RemoteArtifacts {
    $entries = @(
        @($remotePayload,$expectedPayloadHash),
        @($remoteBootstrap,$expectedBootstrapHash),
        @($remoteController,$expectedControllerHash),
        @($remoteCarrier,$expectedCarrierHash),
        @($remoteObserver,$expectedObserverHash),
        @('/system/lib64/libc.so',$expectedLibcHash)
    )
    foreach ($entry in $entries) { Assert-RemoteHash $entry[0] $entry[1] }
}

foreach ($path in @($buildScript,$payload,$bootstrap,$controller,$carrier,
                     $observer,$parser,$baselineVerifier,$baseline)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G2 runner input: $path"
    }
}
& $buildScript | Out-Host
if ($LASTEXITCODE -ne 0) { throw "G2 offline build failed" }
python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "Known-good 900-frame baseline drift" }
$localArtifacts = @(
    @($payload,$expectedPayloadHash), @($bootstrap,$expectedBootstrapHash),
    @($controller,$expectedControllerHash), @($carrier,$expectedCarrierHash),
    @($observer,$expectedObserverHash)
)
foreach ($entry in $localArtifacts) {
    if ((Get-Sha $entry[0]) -ne $entry[1]) {
        throw "Pinned G2 artifact drift: $($entry[0])"
    }
}

if ($Mode -eq 'OfflineValidate') {
    Write-Output "G2_CALL_GATE_OFFLINE passed=1 device_access=0 deployed=0 dynamic=NOT_RUN limit=$limit auto_esc=1"
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
    Invoke-Root "chmod 644 $remotePayload $remoteBootstrap; chmod 700 $remoteController $remoteCarrier $remoteObserver" "set G2 artifact permissions" | Out-Null
    Assert-RemoteArtifacts
    Invoke-Root "am force-stop $package" "stop prior game process" | Out-Null
    Wait-Until { (Get-GamePid) -eq 0 } "prior game termination"
    Invoke-Root "rm -f $remoteCarrierLog; nohup $remoteCarrier >$remoteCarrierLog 2>&1 </dev/null &" "arm fixed G2 carrier" | Out-Null
    Invoke-Root "am start -n $activity" "start fresh G2 game process" | Out-Null
    Wait-Until {
        $log = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat $remoteCarrierLog 2>/dev/null'")
        return $log -match 'HABI1_EARLY_CARRIER passed=[01]'
    } "G2 early carrier completion"
    $carrierResult = Invoke-Root "cat $remoteCarrierLog" "read G2 carrier result"
    if ($carrierResult -notmatch 'HABI1_EARLY_CARRIER passed=1') {
        throw "G2 early carrier failed: $carrierResult"
    }
    Wait-Until {
        $gameProcessId = Get-GamePid
        if ($gameProcessId -le 0) { return $false }
        $maps = Invoke-AdbText @('-s',$Device,'shell',
            "su -c 'cat /proc/$gameProcessId/maps 2>/dev/null'")
        return $maps -match 'libAsphalt9\.so' -and
               $maps -match [Regex]::Escape($remotePayload) -and
               $maps -match [Regex]::Escape($remoteBootstrap)
    } "game and fixed G2 mappings"
    Wait-Until {
        try {
            $null = Read-GameIdentity
            return $true
        } catch {
            return $false
        }
    } "stable unique game identity"
    Start-Sleep -Milliseconds 100
    $identity = Read-GameIdentity
    Write-Output "G2_PROCESS_PREPARED passed=1 pid=$($identity.Pid) start_ticks=$($identity.StartTicks) next=enter_ancient_ruins_zl1_and_pause_at_countdown3"
    exit 0
}

if (-not ($AcknowledgeSinglePermanentHookAndAutomaticEsc -and
          $AcknowledgeRestoreOrTerminateOnUncertainty -and
          $ExecuteExactlyOneStage)) {
    throw "ExecuteCallGate requires its three explicit acknowledgements"
}
Assert-RemoteArtifacts
$identity = Read-GameIdentity
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$evidenceDir = Join-Path $root "evidence\g2-$limit-call-$stamp"
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null
$remotePrefix = "/data/local/tmp/a9tas-g2-$limit-$stamp"
$remoteObservation = "$remotePrefix.a9pio2"
$localObservation = Join-Path $evidenceDir "locator.a9pio2"
$owner = [UInt64]0
$mayBeInstalled = $false
$restored = $false
try {
    Invoke-Root "rm -f $remotePrefix.*" "clear unique G2 receipts" | Out-Null
    $baseHex = $identity.Base.ToString('x')
    Invoke-Root "$remoteObserver $($identity.Pid) $baseHex 300 10 $remoteObservation" "run existing read-only step-options locator" | Out-Host
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteObservation,$localObservation) "pull read-only locator evidence" | Out-Null
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

    $installOutput = "$remotePrefix.install.bin"
    $mayBeInstalled = $true
    try {
        $install = Invoke-Controller 'install' $identity $owner $installOutput
    } catch {
        if ($_.Exception.Message -match
            'stage=(process_identity|runtime_identity|open_mem|freeze|stopped_identity|install_precondition|configure)\b') {
            $mayBeInstalled = $false
        }
        throw
    }
    $install | Write-Output
    if ($install -notmatch 'G2_PHYSICS_INTERVAL_CONTROLLER passed=1 action=1') {
        throw "G2 install receipt was not a strict pass"
    }

    # The only automatic input in this Gate: resume the countdown once.
    Invoke-AdbChecked @('-s',$Device,'shell','input keyevent 111') "send one ESC" | Out-Null
    $waitMs = $CompletionWaitMs
    if ($waitMs -eq 0) {
        $waitMs = [Math]::Max(600,
            [int][Math]::Ceiling(($limit / 60.0) * 1000.0 + 2000.0))
    }
    Start-Sleep -Milliseconds $waitMs
    if ((Get-GamePid) -ne $identity.Pid) { throw "Game process changed during call Gate" }

    $statusOutput = "$remotePrefix.status.bin"
    $status = Invoke-Controller 'status' $identity $owner $statusOutput
    $status | Write-Output
    $requiredStatus = @(
        'passed=1', 'status=2', "cursor=$limit", 'completed=1', 'active=0',
        "wrapper_returns=$limit", "qualified=$limit", 'unqualified=0',
        "valid_outputs=$limit", 'semantic_errors=0', 'identity_errors=0',
        'tid_changes=0'
    )
    foreach ($token in $requiredStatus) {
        if ($status -notmatch "(^|\s)$([Regex]::Escape($token))(\s|$)") {
            throw "$limit-call receipt missing '$token': $status"
        }
    }

    $restoreOutput = "$remotePrefix.restore.bin"
    $restore = Invoke-Controller 'restore' $identity $owner $restoreOutput
    $restore | Write-Output
    if ($restore -notmatch 'G2_PHYSICS_INTERVAL_CONTROLLER passed=1 action=3') {
        throw "G2 restore receipt was not a strict pass"
    }
    $restored = $true
    foreach ($entry in @(
        @($installOutput,(Join-Path $evidenceDir 'install.bin')),
        @($statusOutput,(Join-Path $evidenceDir 'status.bin')),
        @($restoreOutput,(Join-Path $evidenceDir 'restore.bin')),
        @($remoteCarrierLog,(Join-Path $evidenceDir 'carrier.log')))) {
        Invoke-AdbChecked @('-s',$Device,'pull',$entry[0],$entry[1]) "pull G2 evidence" | Out-Null
    }
    $finalIdentity = Read-GameIdentity
    if ($finalIdentity.Pid -ne $identity.Pid -or
        $finalIdentity.StartTicks -ne $identity.StartTicks) {
        throw "Game identity changed after restore"
    }
    Write-Output "G2_CALL_GATE_PASSED calls=$limit original_once_each=1 output_writes=0 ptrace_hot_path=0 restored=1 TracerPid=0 evidence=$evidenceDir"
}
finally {
    if ($mayBeInstalled -and -not $restored -and $owner -ne 0 -and
        (Get-GamePid) -eq $identity.Pid) {
        try {
            $rollbackOutput = "$remotePrefix.rollback.bin"
            Invoke-Controller 'restore' $identity $owner $rollbackOutput | Out-Host
            $restored = $true
            Write-Warning "G2 Gate failed; bounded restore completed"
        } catch {
            Write-Warning "G2 restore could not be proven; terminating only this disposable process"
            try { Invoke-Root "am force-stop $package" "terminate uncertain G2 process" | Out-Null } catch { }
        }
    }
}
