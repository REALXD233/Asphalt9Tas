param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "build\camera-raceview-same-state-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$source = Join-Path $root "src\payload_camera_raceview_same_state_v1.cpp"
$protocol = Join-Path $root "src\camera_raceview_same_state_protocol_v1.h"
$policy = Join-Path $root "tools\test_camera_raceview_same_state_payload_policy_v1.py"
$output = Join-Path $outDir "liba9tas_camera_raceview_same_state_v1_build_only.so"
$disassembly = Join-Path $outDir "camera_raceview_same_state_v1.disasm.txt"
foreach ($path in @($compiler,$readelf,$objdump,$source,$protocol,$policy)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing same-state input: $path" }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -Wall -Wextra -Werror -fPIC -shared "-Wl,--no-undefined" -o $output
if ($LASTEXITCODE -ne 0) { throw "RaceView same-state payload build failed" }
& $objdump -d --demangle $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "RaceView same-state disassembly failed" }
python -B $policy $output $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "RaceView same-state policy failed" }
Write-Output "CAMERA_RACEVIEW_SAME_STATE_BUILD passed=1 build_only=1 arm64=1 deployed=0 device_access=0"
Write-Output "payload_sha256=$((Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "source_sha256=$((Get-FileHash $source -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "protocol_sha256=$((Get-FileHash $protocol -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "disassembly_sha256=$((Get-FileHash $disassembly -Algorithm SHA256).Hash.ToLowerInvariant())"
