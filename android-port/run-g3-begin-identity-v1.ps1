param(
    [ValidateSet("OfflineValidate", "PrepareProcess", "ExecuteCapture")]
    [string]$Mode = "OfflineValidate",
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [ValidateRange(10000, 120000)][int]$TimeoutMs = 60000,
    [switch]$AcknowledgeDeviceAccessAndFixedArtifactPush,
    [switch]$AcknowledgeFreshDisposableGameProcess,
    [switch]$AcknowledgeOnePermanentReadOnlyHookAndAutomaticEsc,
    [switch]$ExecuteExactlyOneStage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ($Mode -ne "OfflineValidate") {
    throw "RETIRED: the identity observer captured a startup callsite and crashed two fresh processes. Use the revision-8 G3 runner; pause-time object diagnosis is also retired."
}
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildScript = Join-Path $root "build-g3-begin-identity-v1.ps1"
$source = Join-Path $root "src\payload_tick_observer_v1.cpp"
$config = Join-Path $root "config\g3-begin-identity-v1.cfg"
$parser = Join-Path $root "tools\parse_g3_begin_identity_v1.py"
$buildDir = Join-Path $root "build\g3-begin-identity-v1"
$payload = Join-Path $buildDir "liba9tas_g3_begin_identity_v1.so"
$bootstrap = Join-Path $buildDir "liba9tas_g3_begin_identity_bootstrap_v1.so"
$sourceSha = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$carrier = Join-Path $buildDir "a9tas_habi1_early_carrier_$($sourceSha.Substring(0,16))"

$package = "com.aligames.kuang.kybc.aligames"
$activity = "$package/$package.MainActivity"
$remotePayload = "/data/local/tmp/liba9tas_g3_begin_identity_v1.so"
$remoteBootstrap = "/data/local/tmp/liba9tas_g3_begin_identity_bootstrap_v1.so"
$remoteCarrier = "/data/local/tmp/a9tas_habi1_early_carrier_$($sourceSha.Substring(0,16))"
$remoteConfig = "/data/local/tmp/a9tas-tick-observer.cfg"
$remoteMarker = "/data/local/tmp/a9tas-enable-tick-observer"
$remoteCarrierLog = "/data/local/tmp/a9tas-g3-begin-identity-carrier.log"
$remoteIdentity = "/data/user/0/$package/files/a9tas-g3-begin-identity-v1.txt"
$remoteArmReceipt = "/data/user/0/$package/files/a9tas-g3-begin-identity-arm-v1.txt"

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
        Start-Sleep -Milliseconds 500
    }
    throw "Timed out: $Label"
}
function Get-GamePid {
    $value = Invoke-AdbText @('-s',$Device,'shell','pidof',$package)
    if ($value -match '^\d+$') { return [int]$value }
    return 0
}
function Assert-RemoteHash([string]$Path, [string]$Expected) {
    $line = Invoke-Root "sha256sum $Path" "hash $Path"
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+' -or
        $matches[1].ToLowerInvariant() -ne $Expected) {
        throw "Remote hash mismatch: $Path"
    }
}
function Read-GameIdentity {
    $gameProcessId = Get-GamePid
    if ($gameProcessId -le 0) { throw "Game process is not running" }
    $status = Invoke-Root "grep '^TracerPid:' /proc/$gameProcessId/status" "read tracer state"
    if ($status -notmatch '^TracerPid:\s*0$') { throw "Unexpected tracer state: $status" }
    $maps = Invoke-Root "cat /proc/$gameProcessId/maps" "read game maps"
    if ($maps -notmatch [Regex]::Escape($remotePayload) -or
        $maps -notmatch [Regex]::Escape($remoteBootstrap)) {
        throw "Pinned identity payload/bootstrap are not both mapped"
    }
    $bases = foreach ($line in ($maps -split "`n")) {
        if ($line -notmatch 'libAsphalt9\.so') { continue }
        if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+' -and
            [Convert]::ToUInt64($matches[2],16) -eq 0) {
            [Convert]::ToUInt64($matches[1],16)
        }
    }
    $bases = @($bases | Sort-Object -Unique)
    if ($bases.Count -ne 1) { throw "Expected one game base, found $($bases.Count)" }
    return [PSCustomObject]@{ Pid=$gameProcessId; Base=[UInt64]$bases[0] }
}

foreach ($path in @($buildScript,$source,$config,$parser)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G3 begin-identity runner input: $path"
    }
}
& $buildScript | Out-Host
if ($LASTEXITCODE -ne 0) { throw "G3 begin-identity offline build failed" }
foreach ($path in @($payload,$bootstrap,$carrier)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing rebuilt G3 begin-identity artifact: $path"
    }
}
$artifactHashes = [ordered]@{
    $remotePayload = Get-Sha $payload
    $remoteBootstrap = Get-Sha $bootstrap
    $remoteCarrier = Get-Sha $carrier
    $remoteConfig = Get-Sha $config
}
if ($Mode -eq 'OfflineValidate') {
    Write-Output "G3_BEGIN_IDENTITY_OFFLINE passed=1 device_access=0 deployed=0 hook=UpdatePerTick capture=first_entry_only"
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
    foreach ($entry in @(
        @($payload,$remotePayload), @($bootstrap,$remoteBootstrap),
        @($carrier,$remoteCarrier), @($config,$remoteConfig))) {
        Invoke-AdbChecked @('-s',$Device,'push',$entry[0],$entry[1]) "push $($entry[1])" | Out-Null
    }
    Invoke-Root "chmod 644 $remotePayload $remoteBootstrap $remoteConfig; chmod 700 $remoteCarrier" "set identity artifact permissions" | Out-Null
    foreach ($entry in $artifactHashes.GetEnumerator()) {
        Assert-RemoteHash $entry.Key $entry.Value
    }
    Invoke-Root "am force-stop $package" "stop prior game process" | Out-Null
    Wait-Until { (Get-GamePid) -eq 0 } "prior game termination"
    Invoke-Root "rm -f $remoteIdentity $remoteArmReceipt $remoteCarrierLog; touch $remoteMarker; chmod 644 $remoteMarker; nohup $remoteCarrier >$remoteCarrierLog 2>&1 </dev/null &" "arm fixed begin-identity carrier" | Out-Null
    # Start from the regular Android shell user and wait for ActivityManager's
    # launch receipt.  On this LDPlayer build, root `am start` can return zero
    # while only reviving a stale task record and never creating the process.
    $launch = Invoke-AdbChecked @('-s',$Device,'shell','am','start','-W',
        '--user','0','-n',$activity) "start fresh identity game process"
    if ($launch -notmatch '(?m)^Status:\s+ok\s*$') {
        throw "Fresh identity activity did not return Status: ok: $launch"
    }
    Wait-Until {
        $log = Invoke-AdbText @('-s',$Device,'shell',"su -c 'cat $remoteCarrierLog 2>/dev/null'")
        return $log -match 'HABI1_EARLY_CARRIER passed=[01]'
    } "begin-identity early carrier completion"
    $carrierResult = Invoke-Root "cat $remoteCarrierLog" "read begin-identity carrier result"
    if ($carrierResult -notmatch 'HABI1_EARLY_CARRIER passed=1') {
        Invoke-Root "rm -f $remoteMarker" "disarm failed identity marker" | Out-Null
        throw "Begin-identity early carrier failed: $carrierResult"
    }
    Wait-Until {
        if ((Get-GamePid) -le 0) { return $false }
        $exists = Invoke-AdbText @('-s',$Device,'shell',
            "su -c 'test -s $remoteArmReceipt; echo `$?'")
        return $exists -eq '0'
    } "early UpdatePerTick hook arm receipt"
    $identity = Read-GameIdentity
    Invoke-Root "rm -f $remoteMarker" "disarm future identity installs" | Out-Null
    $armReceipt = Invoke-Root "cat $remoteArmReceipt" "read identity arm receipt"
    foreach ($token in @('magic=A9G3AR1',"pid=$($identity.Pid)",
                          "guest_base=0x$($identity.Base.ToString('x'))",'mask=0x1')) {
        if ($armReceipt -notmatch "(?m)^$([Regex]::Escape($token))$") {
            throw "Identity arm receipt mismatch: $armReceipt"
        }
    }
    Write-Output $carrierResult
    Write-Output $armReceipt
    Write-Output "G3_BEGIN_IDENTITY_PREPARED passed=1 pid=$($identity.Pid) base=0x$($identity.Base.ToString('x')) hook_install_delay_ms=0 gameplay_writes=0 next=enter_ancient_ruins_zl1_pause_at_countdown3"
    exit 0
}

if (-not ($AcknowledgeOnePermanentReadOnlyHookAndAutomaticEsc -and
          $ExecuteExactlyOneStage)) {
    throw "ExecuteCapture requires its two explicit acknowledgements"
}
foreach ($entry in $artifactHashes.GetEnumerator()) {
    Assert-RemoteHash $entry.Key $entry.Value
}
$identity = Read-GameIdentity
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$evidenceDir = Join-Path $root "evidence\g3-begin-identity-$stamp"
New-Item -ItemType Directory -Path $evidenceDir -Force | Out-Null
$localIdentity = Join-Path $evidenceDir "begin-identity.txt"
$localCarrier = Join-Path $evidenceDir "carrier.log"
$captured = $false
try {
    Invoke-AdbChecked @('-s',$Device,'shell','input keyevent 111') "send one ESC" | Out-Null
    Wait-Until {
        $exists = Invoke-AdbText @('-s',$Device,'shell',"su -c 'test -s $remoteIdentity; echo `$?'")
        return $exists -eq '0'
    } "first UpdatePerTick identity receipt"
    if ((Get-GamePid) -ne $identity.Pid) { throw "Game process changed during identity capture" }
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteIdentity,$localIdentity) "pull begin identity receipt" | Out-Null
    Invoke-AdbChecked @('-s',$Device,'pull',$remoteCarrierLog,$localCarrier) "pull begin identity carrier log" | Out-Null
    $parsed = (& python -B $parser $localIdentity) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        throw "Begin identity receipt was captured but strict route parsing failed: $parsed"
    }
    $parsed | Write-Output
    $captured = $true
}
finally {
    try { Invoke-Root "rm -f $remoteMarker" "disarm future identity installs" | Out-Null } catch { }
    if ((Get-GamePid) -eq $identity.Pid) {
        try {
            Invoke-Root "am force-stop $package" "terminate disposable identity process" | Out-Null
            Wait-Until { (Get-GamePid) -eq 0 } "disposable identity process termination"
        } catch {
            if ($captured) { throw }
        }
    }
}
Write-Output "G3_BEGIN_IDENTITY_CAPTURED passed=1 pid=$($identity.Pid) auto_esc=1 hook=UpdatePerTick first_entry_only process_terminated=1 evidence=$evidenceDir"
