param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$header = Join-Path $root "src\camera_wrapper_graph_v1.h"
$selftest = Join-Path $root "src\camera_wrapper_graph_selftest_v1.cpp"
$source = Join-Path $root "src\camera_wrapper_graph_check_v1.cpp"
$policy = Join-Path $root "tools\test_camera_wrapper_graph_policy_v1.py"
$outDir = Join-Path $root "build\camera-wrapper-graph-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$arm = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$armObject = Join-Path $outDir "camera_wrapper_graph_v1_arm64_review_only.o"
$x86Object = Join-Path $outDir "camera_wrapper_graph_v1_x86_64_review_only.o"
$output = Join-Path $outDir "a9tas_camera_wrapper_graph_check_v1_review_only"

foreach ($path in @($header, $selftest, $source, $policy, $arm, $x86)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing camera wrapper-graph input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

foreach ($pair in @(@($arm, $armObject), @($x86, $x86Object))) {
    & $pair[0] $selftest "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -fno-exceptions -fno-rtti -Wall -Wextra -Werror -c -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "Camera wrapper-graph selftest build failed" }
}

& $x86 $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fPIE -pie -fno-exceptions -fno-rtti `
    -Wall -Wextra -Werror "-Wl,--no-undefined" -o $output
if ($LASTEXITCODE -ne 0) { throw "Camera wrapper-graph check build failed" }

python -B $policy $output
if ($LASTEXITCODE -ne 0) { throw "Camera wrapper-graph policy failed" }

Write-Output "CAMERA_WRAPPER_GRAPH_BUILD passed=1 read_only=1 arm64=1 x86_64=1 device_access=0 deployed=0"
Write-Output "header_sha256=$((Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "arm64_object_sha256=$((Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "x86_64_object_sha256=$((Get-FileHash -LiteralPath $x86Object -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "review_binary_sha256=$((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant())"
