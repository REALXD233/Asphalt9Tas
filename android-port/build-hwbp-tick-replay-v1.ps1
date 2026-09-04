param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\hwbp_tick_replay_v1.cpp"
$outputDir = Join-Path $projectRoot "build\staging"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
$output = Join-Path $outputDir "a9tas_hwbp_tick_replay_v1"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "NDK compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fno-exceptions" `
    "-fno-rtti" "-Wall" "-Wextra" "-Werror" $source "-o" $output
if ($LASTEXITCODE -ne 0) { throw "HWBP tick replay v1 build failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Built $output"
Write-Output "SHA256 $hash"
