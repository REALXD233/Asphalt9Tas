param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\camera_raceview_callback_node_selftest_v1.cpp"
$policy = Join-Path $root "tools\test_camera_raceview_callback_node_policy_v1.py"
$outDir = Join-Path $root "build\camera-raceview-callback-node-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$arm = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$armObject = Join-Path $outDir "camera_raceview_callback_node_v1_arm64_review_only.o"
$x86Object = Join-Path $outDir "camera_raceview_callback_node_v1_x86_64_review_only.o"

foreach ($path in @($source, $policy, $arm, $x86)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing RaceView callback-node input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

foreach ($pair in @(@($arm, $armObject), @($x86, $x86Object))) {
    & $pair[0] $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -fno-exceptions -fno-rtti -Wall -Wextra -Werror -c -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "RaceView callback-node build failed" }
}
python -B $policy
if ($LASTEXITCODE -ne 0) { throw "RaceView callback-node policy failed" }

Write-Output "CAMERA_RACEVIEW_CALLBACK_NODE_BUILD passed=1 read_only=1 arm64=1 x86_64=1 device_access=0 deployed=0"
Write-Output "arm64_object_sha256=$((Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "x86_64_object_sha256=$((Get-FileHash -LiteralPath $x86Object -Algorithm SHA256).Hash.ToLowerInvariant())"
