param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build-lifecycle-final-writer-natural-action-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "composite final-writer prerequisite failed" }
& (Join-Path $root "build-natural-action-external-replay-controller-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "external action lifecycle prerequisite failed" }

$source = Join-Path $root "src\bootstrap_final_writer_natural_action_v1_build.cpp"
$policy = Join-Path $root "tools\test_final_writer_natural_action_preload_policy_v1.py"
$outDir = Join-Path $root "build\final-writer-natural-action-live-v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_final_writer_natural_action_v1.so"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($path in @($source, $policy, $compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing composite preload input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "composite preload bootstrap build failed" }
$header = (& $readelf -h $bootstrap) -join "`n"
$symbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN" -or
    $symbols -notmatch "a9tas_bootstrap_status" -or
    $symbols -notmatch "a9tas_bootstrap_stage") {
    throw "composite bootstrap ABI mismatch"
}
python -B $policy $bootstrap
if ($LASTEXITCODE -ne 0) { throw "composite preload policy failed" }
$hash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "FINAL_WRITER_NATURAL_ACTION_LIVE_BUILD passed=1 payloads=2 one_preload_window=1 candidates=2 deployed=0 device_access=0"
Write-Output "bootstrap_sha256=$hash"
