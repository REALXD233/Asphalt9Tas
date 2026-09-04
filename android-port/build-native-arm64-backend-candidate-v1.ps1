param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildRoot = Join-Path $portRoot 'build'
$output = Join-Path $buildRoot 'native-arm64-backend-candidate-v1'
$bundle = Join-Path $output 'bundle'
$archive = Join-Path $output 'a9tas-native-arm64-backend-candidate-v1.zip'
$componentScripts = @(
    'build-native-arm64-early-loader-v1.ps1',
    'build-native-arm64-g4-controller-v1.ps1',
    'build-native-arm64-g8-readonly-observer-v1.ps1'
)

foreach ($name in $componentScripts) {
    $script = Join-Path $portRoot $name
    if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
        throw "Missing native backend component build: $script"
    }
    & $script -NdkRoot $NdkRoot | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "native backend component failed: $name" }
}

$artifacts = @(
    @{
        role='loader';
        source=Join-Path $buildRoot 'native-arm64-early-loader-v1/a9tas_native_arm64_early_loader_v1';
        name='a9tas_native_arm64_early_loader_v1'; mode='0700'
    },
    @{
        role='controller';
        source=Join-Path $buildRoot 'native-arm64-g4-controller-v1/a9tas_native_arm64_g4_input_action_controller_v1';
        name='a9tas_native_arm64_g4_input_action_controller_v1'; mode='0700'
    },
    @{
        role='observer';
        source=Join-Path $buildRoot 'native-arm64-g8-readonly-observer-v1/a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1';
        name='a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1'; mode='0700'
    },
    @{
        role='payload';
        source=Join-Path $buildRoot 'g4-multi-hook-runtime-v1/liba9tas_g4_multi_hook_runtime_v1.so';
        name='liba9tas_g4_multi_hook_runtime_v1.so'; mode='0644'
    },
    @{
        role='profile';
        source=Join-Path $portRoot 'A9TasAndroid/app/src/main/assets/profiles/439fd7f94ef570d9.a9profile.bin';
        name='439fd7f94ef570d9.a9profile.bin'; mode='0644'
    },
    @{
        role='profile';
        source=Join-Path $portRoot 'A9TasAndroid/app/src/main/assets/profiles/671522d4614abcce.a9profile.bin';
        name='671522d4614abcce.a9profile.bin'; mode='0644'
    },
    @{
        role='profile_registry';
        source=Join-Path $portRoot 'A9TasAndroid/app/src/main/assets/profiles/registry.json';
        name='profile-registry.json'; mode='0644'
    }
)

foreach ($artifact in $artifacts) {
    if (-not (Test-Path -LiteralPath $artifact.source -PathType Leaf)) {
        throw "Missing native backend artifact: $($artifact.source)"
    }
}
New-Item -ItemType Directory -Force -Path $bundle | Out-Null
$manifestArtifacts = @()
foreach ($artifact in $artifacts) {
    $target = Join-Path $bundle $artifact.name
    Copy-Item -LiteralPath $artifact.source -Destination $target -Force
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifact.source).Hash.ToLowerInvariant()
    $targetHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash.ToLowerInvariant()
    if ($sourceHash -ne $targetHash) {
        throw "native backend copy verification failed: $($artifact.name)"
    }
    $manifestArtifacts += [ordered]@{
        role=$artifact.role
        file=$artifact.name
        mode=$artifact.mode
        size=(Get-Item -LiteralPath $target).Length
        sha256=$targetHash
    }
}
$manifest = [ordered]@{
    schema=1
    id='arm64-native-rooted-candidate-v1'
    label='Native ARM64 rooted Android acceptance candidate'
    enabled=$false
    accepted=$false
    host_machine='arm64'
    bridge_set='none'
game_payload_sha256='0dfdec4c3119e9f48dd604488364e2c71e15a186e320577dae308637a604e3dc'
    evidence_boundary='offline-build-only; loader/controller/observer require live rooted ARM64 acceptance'
    gate_order=@('loader-only','observer-only','passive-5','passive-60','passive-900',
                 'neutral-5','neutral-60','neutral-900','nonzero-record-900',
                 'nonzero-replay-900','retry-rearm','cancel-restore','failure-recovery')
    artifacts=$manifestArtifacts
}
$manifestPath = Join-Path $bundle 'candidate-manifest.json'
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
$parsed = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($parsed.enabled -ne $false -or $parsed.accepted -ne $false -or
    $parsed.artifacts.Count -ne $artifacts.Count) {
    throw 'native backend candidate manifest validation failed'
}
if (Test-Path -LiteralPath $archive -PathType Leaf) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -Path (Join-Path $bundle '*') -DestinationPath $archive
$archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
$manifestHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestPath).Hash.ToLowerInvariant()
Write-Output 'NATIVE_ARM64_BACKEND_CANDIDATE_BUILD passed=1 artifacts=7 enabled=0 accepted=0 live_gate=NOT_RUN device_access=0'
Write-Output "candidate_manifest_sha256=$manifestHash"
Write-Output "candidate_archive_sha256=$archiveHash"
Write-Output "candidate_archive=$archive"
