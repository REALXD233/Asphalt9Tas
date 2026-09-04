param(
    [Parameter(Mandatory = $true)]
    [string]$GamePackage,
    [Parameter(Mandatory = $true)]
    [string]$ProfilePath,
    [string]$GameProcessName = "",
    [int]$TargetProcessId = 0,
    [string]$Device = "emulator-5554",
    [string]$AdbPath = "D:\leidian\LDPlayer9\adb.exe",
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binary = Join-Path $root `
    "build\channel-agnostic-vehicle-live-gate-v1\a9tas_channel_agnostic_vehicle_live_gate_v1"
$remoteBinary = "/data/local/tmp/a9tas_channel_agnostic_vehicle_live_gate_v1"
$resolvedProfilePath = [IO.Path]::GetFullPath($ProfilePath)
foreach ($path in @($AdbPath, $binary, $resolvedProfilePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing live-gate input: $path"
    }
}

$profile = Get-Content -Raw -LiteralPath $resolvedProfilePath | ConvertFrom-Json
if ($profile.schema -ne "A9_BUILD_PROFILE_RESOLUTION_V1" -or
    $profile.status -ne "STATIC_STAGE2_PASS_LIVE_READONLY_REQUIRED" -or
    $profile.write_authorized -ne $false -or
    $profile.channel_specific_literals -ne 0) {
    throw "Profile is not a fail-closed channel-agnostic Stage-2 profile"
}
$workspaceRoot = Split-Path -Parent $root
$candidatePath = [IO.Path]::GetFullPath((Join-Path $workspaceRoot $profile.candidate.path))
if (-not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) {
    throw "Profile candidate ELF is missing: $candidatePath"
}
$candidateHash = (Get-FileHash -LiteralPath $candidatePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($candidateHash -ne $profile.candidate.sha256) {
    throw "Profile candidate ELF hash mismatch"
}
$profileHash = (Get-FileHash -LiteralPath $resolvedProfilePath -Algorithm SHA256).Hash.ToLowerInvariant()

if ($GameProcessName -eq "") { $GameProcessName = $GamePackage }
if ($TargetProcessId -gt 0) {
    $gamePid = $TargetProcessId
    $actualName = (& $AdbPath -s $Device shell "cat /proc/$gamePid/cmdline").Trim([char]0).Trim()
    if ($actualName -ne $GameProcessName) {
        throw "PID $gamePid process mismatch: expected '$GameProcessName', got '$actualName'"
    }
} else {
    $pidText = (& $AdbPath -s $Device shell "pidof $GameProcessName").Trim()
    if ($pidText -notmatch '^\d+$') {
        throw "Exact game process lookup is missing or ambiguous: '$pidText'"
    }
    $gamePid = [int]$pidText
}

$identity = & $AdbPath -s $Device shell `
    "su -c 'cat /proc/$gamePid/maps; echo __TRACER__; grep TracerPid /proc/$gamePid/status'"
if ($LASTEXITCODE -ne 0) { throw "Unable to read process identity" }
$tracer = @($identity | Where-Object { $_ -match '^TracerPid:' })
if ($tracer.Count -ne 1 -or $tracer[0] -notmatch '^TracerPid:\s*0\s*$') {
    throw "Target is already traced: $($tracer -join ', ')"
}
$baseCandidates = foreach ($line in $identity) {
    if ($line -notmatch 'libAsphalt9\.so') { continue }
    if ($line -match '^([0-9a-fA-F]+)-[0-9a-fA-F]+\s+\S+\s+([0-9a-fA-F]+)\s+') {
        if ([Convert]::ToUInt64($matches[2], 16) -eq 0) {
            [Convert]::ToUInt64($matches[1], 16)
        }
    }
}
$baseCandidates = @($baseCandidates | Sort-Object -Unique)
if ($baseCandidates.Count -ne 1) {
    throw "Expected one zero-offset libAsphalt9 mapping, found $($baseCandidates.Count)"
}
$baseHex = $baseCandidates[0].ToString('x')

$rvas = [Collections.Generic.List[string]]::new()
0..8 | ForEach-Object {
    $rvas.Add(([uint64]$profile.vtables."physics_interface_$_".candidate_rva).ToString('x'))
}
foreach ($role in @('vehicle_position_getter', 'vehicle_rotation_getter')) {
    $rvas.Add(([uint64]$profile.roles.$role.candidate_rva).ToString('x'))
}
foreach ($suffix in @('40','48','58','60','68','88','90','98','A0')) {
    $rvas.Add(([uint64]$profile.roles."vehicle_wrapper_slot$suffix".candidate_rva).ToString('x'))
}
foreach ($suffix in @('40','48','58','60','68','88','90','98','A0')) {
    $rvas.Add(([uint64]$profile.roles."vehicle_delegate_slot$suffix".candidate_rva).ToString('x'))
}
$rvas.Add(([uint64]$profile.vtables.vehicle_source.candidate_rva).ToString('x'))
$rvas.Add(([uint64]$profile.roles.vehicle_source_update.candidate_rva).ToString('x'))
$rvas.Add(([uint64]$profile.vtables.physics_implementation.candidate_rva).ToString('x'))
$rvas.Add(([uint64]$profile.roles.car_physics_body_update.candidate_rva).ToString('x'))
$rvas.Add(([uint64]$profile.vtables.physics_backend.candidate_rva).ToString('x'))
$rvas.Add(([uint64]$profile.vtables.native_physics_body.candidate_rva).ToString('x'))
foreach ($role in @('physics_set_pose','physics_set_position','physics_set_rotation',
                    'physics_set_linear','physics_set_angular','physics_get_linear',
                    'physics_get_angular')) {
    $rvas.Add(([uint64]$profile.roles.$role.candidate_rva).ToString('x'))
}
if ($rvas.Count -ne 42 -or @($rvas | Where-Object { $_ -eq '0' }).Count -ne 0) {
    throw "Profile did not provide the required 42 nonzero runtime RVAs"
}
$graphRvas = [Collections.Generic.List[string]]::new()
foreach ($vtable in @('main_time_source','embedded_time_source','physics_context',
                      'physics_implementation')) {
    $graphRvas.Add(([uint64]$profile.vtables.$vtable.candidate_rva).ToString('x'))
}
$graphRvas.Add(([uint64]$profile.adjusted_setter_vtable.candidate_rva).ToString('x'))
$graphRvas.Add(([uint64]$profile.vtables.nitro_service.candidate_rva).ToString('x'))
foreach ($role in @('nitro_dispatch','adjusted_brake_setter',
                    'adjusted_steering_setter','lifecycle_phase_gate',
                    'lifecycle_shared_enter','lifecycle_derived_enter')) {
    $graphRvas.Add(([uint64]$profile.roles.$role.candidate_rva).ToString('x'))
}
$lifecycleVtables = @($profile.relationships.lifecycle_vtables.candidate_rvas | ForEach-Object {
    ([uint64]$_).ToString('x')
})
if ($graphRvas.Count -ne 12 -or
    @($graphRvas | Where-Object { $_ -eq '0' }).Count -ne 0 -or
    $lifecycleVtables.Count -eq 0) {
    throw "Profile did not provide the complete runtime object graph"
}

& $AdbPath -s $Device push $binary $remoteBinary | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Live-gate push failed" }
$argumentText = (@($gamePid, $baseHex, $profileHash) + $rvas + $graphRvas +
                 @($lifecycleVtables.Count) + $lifecycleVtables) -join ' '
$result = & $AdbPath -s $Device shell `
    "su -c 'chmod 700 $remoteBinary && $remoteBinary $argumentText'"
$exitCode = $LASTEXITCODE
$result | Out-Host
if ($OutputPath -ne '') {
    $resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
    New-Item -ItemType Directory -Path (Split-Path -Parent $resolvedOutput) -Force | Out-Null
    [IO.File]::WriteAllLines($resolvedOutput, [string[]]$result)
}
if ($exitCode -ne 0 -or -not ($result -match 'passed=1')) {
    throw "Channel-agnostic vehicle live gate failed with exit code $exitCode"
}
Write-Output "CHANNEL_AGNOSTIC_LIVE_GATE_HOST passed=1 pid=$gamePid base=0x$baseHex profile_sha256=$profileHash"
