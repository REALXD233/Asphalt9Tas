param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$runtime = Join-Path $portRoot 'A9TasAndroid/app/src/main/assets/runtime'
$manifestPath = Join-Path $runtime 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$hashes = @{}

# Build and stage the shared guest payload plus the x86/NativeBridge command
# path first.  The native ARM64 early loader embeds the exact payload hash, so
# building it before this synchronization would create a split protocol.
$sharedBuild = Join-Path $portRoot 'build-g4-input-action-live-gate-v1.ps1'
& $sharedBuild -NdkRoot $NdkRoot | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Shared G4 runtime build failed' }
$sharedOutput = Join-Path $portRoot 'build/g4-input-action-live-gate-v1'
$payloadSource = Join-Path $portRoot 'build/g4-multi-hook-runtime-v1/liba9tas_g4_multi_hook_runtime_v1.so'
$carrierSource = Get-ChildItem -LiteralPath $sharedOutput -File `
    -Filter 'a9tas_habi1_early_carrier_*' | Sort-Object LastWriteTimeUtc -Descending |
    Select-Object -First 1
if ($null -eq $carrierSource) { throw 'Rebuilt x86 early carrier is missing' }
$sharedArtifacts = @(
    @{ name = 'liba9tas_g4_multi_hook_runtime_v1.so'; source = $payloadSource },
    @{ name = 'liba9tas_g4_input_action_bootstrap_v1.so'; source = Join-Path $sharedOutput 'liba9tas_g4_input_action_bootstrap_v1.so' },
    @{ name = 'a9tas_g4_input_action_controller_v1'; source = Join-Path $sharedOutput 'a9tas_g4_input_action_controller_v1' },
    @{ name = $carrierSource.Name; source = $carrierSource.FullName }
)
foreach ($artifact in $sharedArtifacts) {
    $target = Join-Path $runtime $artifact.name
    $pending = "$target.pending"
    Copy-Item -LiteralPath $artifact.source -Destination $pending -Force
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifact.source).Hash.ToLowerInvariant()
    $pendingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pending).Hash.ToLowerInvariant()
    if ($sourceHash -ne $pendingHash) { throw "Shared runtime staging hash mismatch: $($artifact.name)" }
    Move-Item -LiteralPath $pending -Destination $target -Force
    $hashes[$artifact.name] = $sourceHash
}
$x86Backend = @($manifest.artifact_sets | Where-Object id -eq 'x86_64-nativebridge-libnb-0e34ebf9')
if ($x86Backend.Count -ne 1) { throw 'x86 runtime backend manifest entry is not unique' }
$carrierEntry = @($x86Backend[0].artifacts | Where-Object device_name -like 'a9tas_habi1_early_carrier_*')
if ($carrierEntry.Count -ne 1) { throw 'x86 carrier manifest entry is not unique' }
$carrierEntry[0].asset = 'runtime/' + $carrierSource.Name
$carrierEntry[0].device_name = $carrierSource.Name
$carrierEntry[0].sha256 = $hashes[$carrierSource.Name]
$x86Backend[0].roles.carrier = $carrierSource.Name

$buildScripts = @(
    'build-native-arm64-early-loader-v1.ps1',
    'build-native-arm64-g4-controller-v1.ps1',
    'build-native-arm64-g8-readonly-observer-v1.ps1'
)
foreach ($name in $buildScripts) {
    & (Join-Path $portRoot $name) -NdkRoot $NdkRoot | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "ARM64 runtime build failed: $name" }
}

$artifacts = @(
    @{
        name = 'a9tas_native_arm64_early_loader_v1'
        source = Join-Path $portRoot 'build/native-arm64-early-loader-v1/a9tas_native_arm64_early_loader_v1'
    },
    @{
        name = 'a9tas_native_arm64_g4_input_action_controller_v1'
        source = Join-Path $portRoot 'build/native-arm64-g4-controller-v1/a9tas_native_arm64_g4_input_action_controller_v1'
    },
    @{
        name = 'a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1'
        source = Join-Path $portRoot 'build/native-arm64-g8-readonly-observer-v1/a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1'
    }
)

foreach ($artifact in $artifacts) {
    if (-not (Test-Path -LiteralPath $artifact.source -PathType Leaf)) {
        throw "Missing rebuilt ARM64 artifact: $($artifact.source)"
    }
    $target = Join-Path $runtime $artifact.name
    $pending = "$target.pending"
    Copy-Item -LiteralPath $artifact.source -Destination $pending -Force
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifact.source).Hash.ToLowerInvariant()
    $pendingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pending).Hash.ToLowerInvariant()
    if ($sourceHash -ne $pendingHash) {
        throw "ARM64 runtime staging hash mismatch: $($artifact.name)"
    }
    Move-Item -LiteralPath $pending -Destination $target -Force
    $hashes[$artifact.name] = $sourceHash
}

# Architecture probes are rebuilt by the product build before this sync. Keep
# both the helper registry and backend artifact sets bound to those exact bytes.
foreach ($name in @('a9tas_identity_probe_arm64_v1',
                    'a9tas_identity_probe_x86_64_v1',
                    'a9tas_practice_mode_readonly_probe_arm64_v1',
                    'a9tas_practice_mode_readonly_probe_x86_64_v1')) {
    $path = Join-Path $runtime $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing rebuilt architecture probe: $path"
    }
    $hashes[$name] = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}

$backend = @($manifest.artifact_sets | Where-Object id -eq 'arm64-native-rooted-candidate-v1')
if ($backend.Count -ne 1) { throw 'ARM64 runtime backend manifest entry is not unique' }
foreach ($set in $manifest.artifact_sets) {
    foreach ($artifact in $set.artifacts) {
        if ($hashes.ContainsKey($artifact.device_name)) {
            $artifact.sha256 = $hashes[$artifact.device_name]
        }
    }
}
foreach ($helper in $manifest.identity_helpers) {
    if ($hashes.ContainsKey($helper.device_name)) {
        $helper.sha256 = $hashes[$helper.device_name]
    }
}
foreach ($name in @('a9tas_native_arm64_early_loader_v1',
                    'a9tas_native_arm64_g4_input_action_controller_v1',
                    'a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1')) {
    if (@($backend[0].artifacts | Where-Object device_name -eq $name).Count -ne 1) {
        throw "ARM64 runtime manifest role missing: $name"
    }
}

$manifestPending = "$manifestPath.pending"
$json = $manifest | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($manifestPending, $json + "`n", [Text.UTF8Encoding]::new($false))
$roundTrip = Get-Content -LiteralPath $manifestPending -Raw | ConvertFrom-Json
if ($roundTrip.schema -ne 2) { throw 'ARM64 runtime manifest round-trip failed' }
Move-Item -LiteralPath $manifestPending -Destination $manifestPath -Force

$receipt = ("NATIVE_ARM64_RUNTIME_SYNC passed=1 loader_sha256={0} " +
    "controller_sha256={1} observer_sha256={2}") -f @(
        $hashes['a9tas_native_arm64_early_loader_v1'],
        $hashes['a9tas_native_arm64_g4_input_action_controller_v1'],
        $hashes['a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1']
    )
Write-Output $receipt
