param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$header = Join-Path $root "src\camera_replay_protocol_v1.h"
$selftest = Join-Path $root "src\camera_replay_protocol_selftest_v1.cpp"
$policy = Join-Path $root "tools\test_camera_replay_protocol_policy_v1.py"
$upstream = Join-Path $root "tools\upstream_src\DetourFunctions.cpp"
$upstreamReplay = Join-Path $root "tools\upstream_src\ReplayStateManager.cpp"
$outDir = Join-Path $root "build\camera-replay-protocol-v1"
$armObject = Join-Path $outDir "camera_replay_protocol_v1_arm64_review_only.o"
$x86Object = Join-Path $outDir "camera_replay_protocol_v1_x86_64_review_only.o"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$arm = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"

foreach ($path in @($header, $selftest, $policy, $upstream, $upstreamReplay, $arm, $x86)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing camera protocol input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

python -B $policy $header $upstream $upstreamReplay
if ($LASTEXITCODE -ne 0) { throw "Camera protocol source parity failed" }

foreach ($pair in @(@($arm, $armObject), @($x86, $x86Object))) {
    & $pair[0] $selftest "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -fno-exceptions -fno-rtti -Wall -Wextra -Werror -c -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "Camera protocol Android object build failed" }
}

Write-Output "CAMERA_REPLAY_PROTOCOL_BUILD passed=1 header=64 frame=64 arm64=1 x86_64=1 lossless_sidecar=1 derived_camera=0 device_access=0 deployed=0"
Write-Output "header_sha256=$((Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "arm64_object_sha256=$((Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "x86_64_object_sha256=$((Get-FileHash -LiteralPath $x86Object -Algorithm SHA256).Hash.ToLowerInvariant())"
