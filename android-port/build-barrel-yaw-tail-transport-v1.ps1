param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\barrel_yaw_tail_transport_selftest_v1.cpp"
$policy = Join-Path $root "tools\test_barrel_yaw_tail_transport_policy_v1.py"
$outDir = Join-Path $root "build\barrel-yaw-tail-transport-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($tool in @($compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required NDK tool unavailable: $tool"
    }
}
$output = Join-Path $outDir "barrel_yaw_tail_transport_v1_selftest"
& $compiler $source "-I$(Join-Path $root 'src')" "-O1" "-std=c++20" `
    "-static-libstdc++" "-fno-exceptions" "-fno-rtti" "-fno-inline" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw transport build failed" }
python -B $policy $output $readelf
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw transport policy failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "BARREL_YAW_TAIL_TRANSPORT_BUILD passed=1 runtime=disabled deployed=0 device_access=0 game_writes=0"
Write-Output "selftest_sha256=$hash"
