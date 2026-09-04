param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\bootstrap_final_writer_natural_action_physics_interval_v1_build.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap.cpp"
$policy = Join-Path $root "tools\test_final_writer_natural_action_physics_interval_preload_policy_v1.py"
$helper = Join-Path $root "tools\run_final_writer_natural_action_physics_interval_preload_v1.sh"
$writerPayload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$actionPayload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$intervalPayload = Join-Path $root "build\physics-interval-getter-payload-v2\liba9tas_physics_interval_getter_v2_passive.so"
$outDir = Join-Path $root "build\final-writer-natural-action-physics-interval-live-v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_final_writer_natural_action_physics_interval_v1.so"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"

foreach ($path in @($source, $bootstrapSource, $policy, $helper,
                     $writerPayload, $actionPayload, $intervalPayload,
                     $compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing triple preload input: $path"
    }
}
$pins = @{
    $writerPayload = "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763"
    $actionPayload = "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f"
    $intervalPayload = "f49b78eff51b606436f291c837ab50c297286ae3f43a10352dc2aa77ca8a62df"
}
foreach ($path in $pins.Keys) {
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $pins[$path]) { throw "Triple payload pin mismatch: $path" }
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Triple preload bootstrap build failed" }
$header = (& $readelf -h $bootstrap) -join "`n"
$symbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN" -or
    $symbols -notmatch "a9tas_bootstrap_status" -or
    $symbols -notmatch "a9tas_bootstrap_stage") {
    throw "Triple bootstrap ABI mismatch"
}
python -B $policy $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Triple preload policy failed" }
$hash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLower()
Write-Output "FINAL_WRITER_NATURAL_ACTION_PHYSICS_INTERVAL_LIVE_BUILD passed=1 payloads=3 one_preload_window=1 deployed=0 device_access=0"
Write-Output "bootstrap_sha256=$hash"
