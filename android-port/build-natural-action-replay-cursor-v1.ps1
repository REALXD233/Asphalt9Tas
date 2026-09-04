param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_replay_cursor_selftest_v1.cpp"
$header = Join-Path $root "src\natural_action_replay_cursor_v1.h"
$policy = Join-Path $root "tools\test_natural_action_replay_cursor_policy_v1.py"
$outDir = Join-Path $root "build\natural-action-replay-cursor-v1"
$output = Join-Path $outDir "natural_action_replay_cursor_selftest_v1_review_only.o"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach ($path in @($source, $header, $policy, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing natural-action replay cursor input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-I$(Join-Path $root 'src')" "-c" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "natural-action replay cursor build failed" }
python -B $policy $output
if ($LASTEXITCODE -ne 0) { throw "natural-action replay cursor policy failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_REPLAY_CURSOR_BUILD passed=1 compiled_selftest=1 runtime=disabled dual_receipt=1 deployed=0 device_access=0"
Write-Output "review_object_sha256=$hash"
