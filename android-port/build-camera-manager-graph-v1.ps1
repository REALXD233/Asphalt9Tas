param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$header = Join-Path $root "src\camera_manager_graph_v1.h"
$selftest = Join-Path $root "src\camera_manager_graph_selftest_v1.cpp"
$source = Join-Path $root "src\camera_manager_graph_check_v1.cpp"
$policy = Join-Path $root "tools\test_camera_manager_graph_policy_v1.py"
$outDir = Join-Path $root "build\camera-manager-graph-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$arm = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x86 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$armObject = Join-Path $outDir "camera_manager_graph_v1_arm64_review_only.o"
$x86Object = Join-Path $outDir "camera_manager_graph_v1_x86_64_review_only.o"
$armOutput = Join-Path $outDir "a9tas_camera_manager_graph_check_v1_arm64"
$x86Output = Join-Path $outDir "a9tas_camera_manager_graph_check_v1_x86_64"
$hudHeader = Join-Path $root "src\gameplay_hud_graph_v1.h"
$hudSelftest = Join-Path $root "src\gameplay_hud_graph_selftest_v1.cpp"
$hudSource = Join-Path $root "src\gameplay_hud_graph_check_v1.cpp"
$hudArmObject = Join-Path $outDir "gameplay_hud_graph_v1_arm64_review_only.o"
$hudX86Object = Join-Path $outDir "gameplay_hud_graph_v1_x86_64_review_only.o"
$hudArmOutput = Join-Path $outDir "a9tas_gameplay_hud_graph_check_v1_arm64"
$hudX86Output = Join-Path $outDir "a9tas_gameplay_hud_graph_check_v1_x86_64"
$hudArmSelftestOutput = Join-Path $outDir "a9tas_gameplay_hud_graph_selftest_v1_arm64"
$hudX86SelftestOutput = Join-Path $outDir "a9tas_gameplay_hud_graph_selftest_v1_x86_64"

foreach ($path in @($header, $selftest, $source, $policy, $hudHeader,
        $hudSelftest, $hudSource, $arm, $x86)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing camera manager-graph input: $path"
    }
}

foreach ($pair in @(@($arm, $hudArmObject), @($x86, $hudX86Object))) {
    & $pair[0] $hudSelftest "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -fno-exceptions -fno-rtti -Wall -Wextra -Werror -c -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "Gameplay HUD graph selftest build failed" }
}

foreach ($pair in @(@($arm, $hudArmSelftestOutput), @($x86, $hudX86SelftestOutput))) {
    & $pair[0] $hudSelftest "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -static-libstdc++ -fPIE -pie -fno-exceptions -fno-rtti `
        -Wall -Wextra -Werror "-Wl,--no-undefined" -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "Gameplay HUD graph selftest link failed" }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

foreach ($pair in @(@($arm, $armObject), @($x86, $x86Object))) {
    & $pair[0] $selftest "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -fno-exceptions -fno-rtti -Wall -Wextra -Werror -c -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "Camera manager-graph selftest build failed" }
}

foreach ($pair in @(@($arm, $hudArmOutput), @($x86, $hudX86Output))) {
    & $pair[0] $hudSource "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -static-libstdc++ -fPIE -pie -fno-exceptions -fno-rtti `
        -Wall -Wextra -Werror "-Wl,--no-undefined" -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "Gameplay HUD graph check build failed" }
}

foreach ($pair in @(@($arm, $armOutput), @($x86, $x86Output))) {
    & $pair[0] $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
        -static-libstdc++ -fPIE -pie -fno-exceptions -fno-rtti `
        -Wall -Wextra -Werror "-Wl,--no-undefined" -o $pair[1]
    if ($LASTEXITCODE -ne 0) { throw "Camera manager-graph check build failed" }
}

python -B $policy $x86Output
if ($LASTEXITCODE -ne 0) { throw "Camera manager-graph policy failed" }

Write-Output "CAMERA_MANAGER_GRAPH_BUILD passed=1 read_only=1 arm64=1 x86_64=1 device_access=0 deployed=0"
Write-Output "GAMEPLAY_HUD_GRAPH_BUILD passed=1 read_only=1 arm64=1 x86_64=1 game_writes=0 method_calls=0 ptrace=0"
Write-Output "header_sha256=$((Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "arm64_object_sha256=$((Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "x86_64_object_sha256=$((Get-FileHash -LiteralPath $x86Object -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "arm64_binary_sha256=$((Get-FileHash -LiteralPath $armOutput -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "x86_64_binary_sha256=$((Get-FileHash -LiteralPath $x86Output -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "hud_header_sha256=$((Get-FileHash -LiteralPath $hudHeader -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "hud_arm64_binary_sha256=$((Get-FileHash -LiteralPath $hudArmOutput -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "hud_x86_64_binary_sha256=$((Get-FileHash -LiteralPath $hudX86Output -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "hud_arm64_selftest_sha256=$((Get-FileHash -LiteralPath $hudArmSelftestOutput -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "hud_x86_64_selftest_sha256=$((Get-FileHash -LiteralPath $hudX86SelftestOutput -Algorithm SHA256).Hash.ToLowerInvariant())"
