param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_synchronized_tick_recorder_v1.cpp"
$policy = Join-Path $root "tools\test_lifecycle_natural_action_source_policy_v1.py"
$outDir = Join-Path $root "build\lifecycle-natural-action-source-v1"
$output = Join-Path $outDir "a9tas_lifecycle_natural_action_source_v1_review_only"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach ($path in @($source, $policy, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing lifecycle natural-action source build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-DA9TAS_SYNC_BRAKE_CAPTURE_V1" `
    "-DA9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1" `
    "-DA9TAS_RACE_LIFECYCLE_SOURCE_V1" `
    "-DA9TAS_NATURAL_ACTION_RECORDING_V1" $source `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "Lifecycle natural-action source build failed" }
python -B $policy $source $output
if ($LASTEXITCODE -ne 0) { throw "Lifecycle natural-action source policy failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "LIFECYCLE_NATURAL_ACTION_SOURCE_BUILD passed=1 review_only=1 deployed=0 device_access=0"
Write-Output "review_binary_sha256=$hash"
