param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\physics_interval_override_core_v1.cpp"
$header = Join-Path $root "src\physics_interval_override_core_v1.h"
$policy = Join-Path $root "tools\test_physics_interval_override_core_policy_v1.py"
$outDir = Join-Path $root "build\physics-interval-override-core-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$x64 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$arm64 = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($x64, $arm64, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$passive = Join-Path $outDir "a9tas_physics_interval_override_core_v1_build_only"
$selftest = Join-Path $outDir "a9tas_physics_interval_override_core_v1_selftest"
$x64Object = Join-Path $outDir "physics_interval_override_core_v1_x86_64.o"
$arm64Object = Join-Path $outDir "physics_interval_override_core_v1_arm64.o"
$disassembly = Join-Path $outDir "physics_interval_override_core_v1.disasm.txt"
$common = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti",
            "-Wall", "-Wextra", "-Werror")
$link = @("-static-libstdc++", "-Wl,--no-undefined")

& $x64 $source @common @link "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "physics interval passive build failed" }
& $x64 $source @common @link "-DA9TAS_PHYSICS_INTERVAL_OVERRIDE_SELFTEST=1" `
    "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "physics interval selftest build failed" }
& $x64 $source @common "-DA9TAS_PHYSICS_INTERVAL_OVERRIDE_NO_MAIN=1" `
    "-c" "-o" $x64Object
if ($LASTEXITCODE -ne 0) { throw "physics interval x86_64 object build failed" }
& $arm64 $source @common "-DA9TAS_PHYSICS_INTERVAL_OVERRIDE_NO_MAIN=1" `
    "-c" "-o" $arm64Object
if ($LASTEXITCODE -ne 0) { throw "physics interval arm64 object build failed" }

& $objdump "-d" "--demangle" $arm64Object |
    Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "physics interval disassembly failed" }

python -B $policy $header $source
if ($LASTEXITCODE -ne 0) { throw "physics interval policy failed" }

foreach ($artifact in @($passive, $selftest, $x64Object, $arm64Object,
                         $disassembly)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower()
    Write-Output "$([IO.Path]::GetFileName($artifact)) sha256=$hash"
}
Write-Output "PHYSICS_INTERVAL_OVERRIDE_CORE_BUILD passed=1 runtime=disabled deployed=0 device_access=0 game_writes=0"
