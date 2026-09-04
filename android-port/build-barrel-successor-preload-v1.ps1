param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bundleSource = Join-Path $root "src\payload_bundle_barrel_successor_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_final_writer_barrel_successor_v1_build.cpp"
$bootstrapCore = Join-Path $root "src\bootstrap.cpp"
$policy = Join-Path $root "tools\test_barrel_successor_preload_policy_v1.py"
$writerPayload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$actionPayload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$intervalPayload = Join-Path $root "build\physics-interval-getter-payload-v2\liba9tas_physics_interval_getter_v2_passive.so"
$barrelPayload = Join-Path $root "build\barrel-yaw-tail-payload-v1\liba9tas_barrel_yaw_tail_v1_build_only.so"
$outDir = Join-Path $root "build\barrel-successor-preload-v1"
$bundle = Join-Path $outDir "liba9tas_payload_bundle_barrel_successor_v1.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_final_writer_barrel_successor_v1.so"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$armCompiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"

foreach ($path in @($bundleSource, $bootstrapSource, $bootstrapCore, $policy,
                     $writerPayload, $actionPayload, $intervalPayload,
                     $barrelPayload, $armCompiler, $x86Compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing barrel successor preload input: $path"
    }
}

$pins = @{
    $writerPayload = "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763"
    $actionPayload = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
    $intervalPayload = "f49b78eff51b606436f291c837ab50c297286ae3f43a10352dc2aa77ca8a62df"
    $barrelPayload = "965e6ba9ce819ee4b6c1a38bf663f68e0c76841dfa7bfad0924ad67bee677ea9"
}
foreach ($path in $pins.Keys) {
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $pins[$path]) {
        throw "Barrel successor payload pin mismatch: $path"
    }
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $armCompiler $bundleSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" "-ldl" `
    "-o" $bundle
if ($LASTEXITCODE -ne 0) { throw "Barrel successor ARM64 bundle build failed" }

& $x86Compiler $bootstrapSource "-I$(Join-Path $root 'src')" `
    "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" "-llog" "-ldl" `
    "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Barrel successor x86_64 bootstrap build failed" }

$bundleHeader = (& $readelf "-h" $bundle) -join "`n"
$bundleSymbols = (& $readelf "--dyn-syms" "--wide" $bundle) -join "`n"
$bootstrapHeader = (& $readelf "-h" $bootstrap) -join "`n"
$bootstrapSymbols = (& $readelf "--dyn-syms" "--wide" $bootstrap) -join "`n"
if ($bundleHeader -notmatch "Machine:\s+AArch64" -or
    $bundleHeader -notmatch "Type:\s+DYN" -or
    $bundleSymbols -notmatch "a9tas_payload_bundle_barrel_successor_status_v1") {
    throw "Barrel successor ARM64 bundle ABI mismatch"
}
if ($bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Type:\s+DYN" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "Barrel successor x86_64 bootstrap ABI mismatch"
}

python -B $policy $bundle $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Barrel successor preload policy failed" }

$bundleHash = (Get-FileHash -LiteralPath $bundle -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "BARREL_SUCCESSOR_PRELOAD_BUILD passed=1 payloads=4 one_preload_window=1 legacy_changed=0 deployed=0 device_access=0"
Write-Output "bundle_sha256=$bundleHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
