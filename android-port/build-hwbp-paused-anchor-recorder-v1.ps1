param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\hwbp_synchronized_tick_recorder_v1.cpp"
$outputDirectory = Join-Path $projectRoot "build\staging"
$output = Join-Path $outputDirectory "a9tas_hwbp_paused_anchor_recorder_v1"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "x86_64 compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$arguments = @(
    "-O2", "-std=c++20", "-static-libstdc++",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
    "-DA9TAS_PAUSED_ANCHOR_WARMUP_V1",
    $source, "-o", $output
)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) {
    throw "paused-anchor recorder build failed (exit=$LASTEXITCODE)"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
Write-Host "Built $output"
Write-Host "SHA256 $hash"
