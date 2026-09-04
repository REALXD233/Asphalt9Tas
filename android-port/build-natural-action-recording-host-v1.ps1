param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_recording_host_selftest_v1.cpp"
$outDir = Join-Path $root "build\natural-action-recording-host-v1"
$output = Join-Path $outDir "natural_action_recording_host_selftest_v1"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach ($path in @($source, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing recording host build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fno-exceptions" `
    "-fno-rtti" "-Wall" "-Wextra" "-Werror" $source `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "Recording host selftest build failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_RECORDING_HOST_BUILD passed=1 deployed=0 device_access=0"
Write-Output "review_binary_sha256=$hash"
