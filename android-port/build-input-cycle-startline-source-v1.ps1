param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_synchronized_tick_recorder_v1.cpp"
$outputDirectory = Join-Path $root "build\input-cycle-startline-source-v1"
$output = Join-Path $outputDirectory "a9tas_input_cycle_startline_source_v1"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"

foreach ($path in @($source, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing input-cycle source build input: $path"
    }
}
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$arguments = @(
    "-O2", "-std=c++20", "-static-libstdc++",
    "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
    "-DA9TAS_SYNC_BRAKE_CAPTURE_V1",
    "-DA9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1",
    $source, "-Wl,--no-undefined", "-o", $output
)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) {
    throw "input-cycle startline source build failed (exit=$LASTEXITCODE)"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "INPUT_CYCLE_STARTLINE_SOURCE_BUILD passed=1 deployed=0 device_access=0"
Write-Host "binary=$output"
Write-Host "sha256=$hash"
