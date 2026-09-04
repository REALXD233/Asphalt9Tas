param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$source = Join-Path $root "src\in_process_tick_coordinator_selftest_v1.cpp"
$header = Join-Path $root "src\in_process_tick_coordinator_v1.h"
$outDir = Join-Path $root "build\in-process-tick-coordinator-v1"
$output = Join-Path $outDir "in_process_tick_coordinator_selftest_v1.o"

foreach ($path in @($compiler, $source, $header)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing in-process coordinator input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $compiler $source "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-c" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "in-process coordinator ARM64 build failed" }

$headerHash = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "IN_PROCESS_TICK_COORDINATOR_BUILD passed=1 arm64=1 device_access=0 deployed=0"
Write-Output "header_sha256=$headerHash"
Write-Output "selftest_sha256=$sourceHash"
Write-Output "object_sha256=$objectHash"
