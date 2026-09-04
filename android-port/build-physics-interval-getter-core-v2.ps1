param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\physics_interval_getter_core_v2.cpp"
$header = Join-Path $root "src\physics_interval_getter_core_v2.h"
$policy = Join-Path $root "tools\test_physics_interval_getter_core_v2_policy.py"
$outDir = Join-Path $root "build\physics-interval-getter-core-v2"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$x64 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$arm64 = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
foreach ($tool in @($x64, $arm64)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing tool: $tool" }
}
$passive = Join-Path $outDir "a9tas_physics_interval_getter_core_v2_build_only"
$selftest = Join-Path $outDir "a9tas_physics_interval_getter_core_v2_selftest"
$x64Object = Join-Path $outDir "physics_interval_getter_core_v2_x86_64.o"
$arm64Object = Join-Path $outDir "physics_interval_getter_core_v2_arm64.o"
$common = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti",
            "-Wall", "-Wextra", "-Werror")
$link = @("-static-libstdc++", "-Wl,--no-undefined")
& $x64 $source @common @link "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "V2 passive build failed" }
& $x64 $source @common @link "-DA9TAS_PHYSICS_INTERVAL_GETTER_SELFTEST=1" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "V2 selftest build failed" }
& $x64 $source @common "-DA9TAS_PHYSICS_INTERVAL_GETTER_NO_MAIN=1" "-c" "-o" $x64Object
if ($LASTEXITCODE -ne 0) { throw "V2 x86 object build failed" }
& $arm64 $source @common "-DA9TAS_PHYSICS_INTERVAL_GETTER_NO_MAIN=1" "-c" "-o" $arm64Object
if ($LASTEXITCODE -ne 0) { throw "V2 ARM64 object build failed" }
python -B $policy $header $source
if ($LASTEXITCODE -ne 0) { throw "V2 policy failed" }
foreach ($artifact in @($passive, $selftest, $x64Object, $arm64Object)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower()
    Write-Output "$([IO.Path]::GetFileName($artifact)) sha256=$hash"
}
Write-Output "PHYSICS_INTERVAL_GETTER_CORE_V2_BUILD passed=1 runtime=disabled deployed=0 device_access=0 game_writes=0"
