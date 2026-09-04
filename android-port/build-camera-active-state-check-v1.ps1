param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\camera_active_state_check_v1.cpp"
$policy = Join-Path $root "tools\test_camera_active_state_check_policy_v1.py"
$outDir = Join-Path $root "build\camera-active-state-check-v1"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
$output = Join-Path $outDir "a9tas_camera_active_state_check_v1_review_only"

foreach ($path in @($source, $policy, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing camera active-state check input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $compiler $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fPIE -pie -fno-exceptions -fno-rtti `
    -Wall -Wextra -Werror "-Wl,--no-undefined" -o $output
if ($LASTEXITCODE -ne 0) { throw "Camera active-state check build failed" }

python -B $policy $output
if ($LASTEXITCODE -ne 0) { throw "Camera active-state check policy failed" }

Write-Output "CAMERA_ACTIVE_STATE_CHECK_BUILD passed=1 read_only=1 x86_64=1 device_access=0 deployed=0"
Write-Output "review_binary_sha256=$((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant())"
