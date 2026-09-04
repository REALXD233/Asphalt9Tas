param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\physics_interval_getter_shadow_transaction_v2.cpp"
$header = Join-Path $root "src\physics_interval_getter_shadow_transaction_v2.h"
$policy = Join-Path $root "tools\test_physics_interval_getter_shadow_transaction_v2_policy.py"
$outDir = Join-Path $root "build\physics-interval-getter-shadow-v2"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "x86 compiler missing" }
$passive = Join-Path $outDir "a9tas_physics_interval_shadow_v2_build_only"
$selftest = Join-Path $outDir "a9tas_physics_interval_shadow_v2_selftest"
$common = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti",
            "-Wall", "-Wextra", "-Werror", "-static-libstdc++",
            "-Wl,--no-undefined")
& $compiler $source @common "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "shadow passive build failed" }
& $compiler $source @common "-DA9TAS_PHYSICS_INTERVAL_SHADOW_SELFTEST=1" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "shadow selftest build failed" }
python -B $policy $header $source
if ($LASTEXITCODE -ne 0) { throw "shadow policy failed" }
foreach ($artifact in @($passive, $selftest)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower()
    Write-Output "$([IO.Path]::GetFileName($artifact)) sha256=$hash"
}
Write-Output "PHYSICS_INTERVAL_SHADOW_TRANSACTION_V2_BUILD passed=1 runtime=disabled deployed=0 process_access=0 game_writes=0"
