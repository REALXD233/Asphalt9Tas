param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build-natural-action-recording-payload-v1.ps1") -NdkRoot $NdkRoot | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recording payload build failed" }
& (Join-Path $root "build-lifecycle-natural-action-source-v1.ps1") -NdkRoot $NdkRoot | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Recording source build failed" }

$source = Join-Path $root "src\bootstrap_natural_action_recording_v1_build.cpp"
$outDir = Join-Path $root "build\natural-action-recording-live-v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_natural_action_recording_v1.so"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($path in @($source, $compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing recording live build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Recording preload bootstrap build failed" }
$header = (& $readelf -h $bootstrap) -join "`n"
$symbols = (& $readelf --dyn-syms $bootstrap) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN" -or
    $symbols -notmatch "a9tas_bootstrap_status" -or
    $symbols -notmatch "a9tas_bootstrap_stage") {
    throw "Recording preload bootstrap identity mismatch"
}
$hash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_RECORDING_LIVE_BUILD passed=1 deployed=0 device_access=0"
Write-Output "bootstrap_sha256=$hash"
