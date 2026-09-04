param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\hwbp_synchronized_tick_recorder_v1.cpp"
$naturalPolicy = Join-Path $projectRoot `
    "tools\test_lifecycle_natural_action_source_policy_v1.py"
$barrelPolicy = Join-Path $projectRoot `
    "tools\test_lifecycle_barrel_source_policy_v1.py"
$outputDirectory = Join-Path $projectRoot `
    "build\lifecycle-natural-action-barrel-source-v1"
$output = Join-Path $outputDirectory `
    "a9tas_lifecycle_natural_action_barrel_source_v1_review_only"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"

foreach ($path in @($source, $naturalPolicy, $barrelPolicy, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing lifecycle barrel source build input: $path"
    }
}
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$arguments = @(
    "-O2", "-std=c++20", "-static-libstdc++",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-function", "-DA9TAS_SYNC_BRAKE_CAPTURE_V1",
    "-DA9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1",
    "-DA9TAS_RACE_LIFECYCLE_SOURCE_V1",
    "-DA9TAS_NATURAL_ACTION_RECORDING_V1",
    "-DA9TAS_BARREL_CAPTURE_V1",
    "-DA9TAS_COMPLETION_WRITE_CERTIFICATE_V1", $source,
    "-Wl,--no-undefined", "-o", $output
)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Lifecycle natural-action barrel source build failed"
}
python -B $naturalPolicy $source $output
if ($LASTEXITCODE -ne 0) {
    throw "Natural-action source policy failed"
}
python -B $barrelPolicy $source $output
if ($LASTEXITCODE -ne 0) {
    throw "Barrel source policy failed"
}
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
Write-Host "LIFECYCLE_BARREL_SOURCE_BUILD passed=1 review_only=1 deployed=0 device_access=0"
Write-Host "SHA256 $hash"
