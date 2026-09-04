param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bundleSource = Join-Path $root "src\payload_bundle_natural_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_final_writer_m1_bundle_v1_build.cpp"
$policy = Join-Path $root "tools\test_m1_three_payload_preload_policy_v1.py"
$helper = Join-Path $root "tools\run_m1_three_payload_preload_v1.sh"
$outDir = Join-Path $root "build\m1-three-payload-preload-v1"
$bundle = Join-Path $outDir "liba9tas_payload_bundle_natural_controller_v1.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_final_writer_m1_v1.so"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$armCompiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"

foreach ($path in @($bundleSource, $bootstrapSource, $policy, $helper, $armCompiler,
                     $x86Compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing M1 preload build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $armCompiler $bundleSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" "-ldl" `
    "-o" $bundle
if ($LASTEXITCODE -ne 0) { throw "M1 ARM64 payload bundle build failed" }

& $x86Compiler $bootstrapSource "-I$(Join-Path $root 'src')" `
    "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" "-llog" "-ldl" `
    "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "M1 x86_64 preload bootstrap build failed" }

$bundleHeader = (& $readelf "-h" $bundle) -join "`n"
$bundleSymbols = (& $readelf "--dyn-syms" "--wide" $bundle) -join "`n"
$bootstrapHeader = (& $readelf "-h" $bootstrap) -join "`n"
$bootstrapSymbols = (& $readelf "--dyn-syms" "--wide" $bootstrap) -join "`n"
if ($bundleHeader -notmatch "Machine:\s+AArch64" -or
    $bundleHeader -notmatch "Type:\s+DYN" -or
    $bundleSymbols -notmatch "a9tas_payload_bundle_status_v1" -or
    $bundleSymbols -notmatch "a9tas_payload_bundle_natural_handle_v1" -or
    $bundleSymbols -notmatch "a9tas_payload_bundle_controller_handle_v1") {
    throw "M1 ARM64 payload bundle ABI mismatch"
}
if ($bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Type:\s+DYN" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "M1 x86_64 bootstrap ABI mismatch"
}

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $bundle $bootstrap
if ($LASTEXITCODE -ne 0) { throw "M1 three-payload preload policy failed" }

$bundleHash = (Get-FileHash -LiteralPath $bundle -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "M1_THREE_PAYLOAD_PRELOAD_BUILD passed=1 arm64_bundle=1 x86_64_bootstrap=1 guest_loads=3 legacy_changed=0 deployed=0 device_access=0"
Write-Output "bundle_sha256=$bundleHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
