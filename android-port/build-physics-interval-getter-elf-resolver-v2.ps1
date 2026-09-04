param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\physics_interval_getter_payload_elf_resolver_selftest_v2.cpp"
$resolver = Join-Path $root "src\physics_interval_getter_payload_elf_resolver_v2.h"
$policy = Join-Path $root "tools\test_physics_interval_getter_payload_elf_resolver_v2_policy.py"
$outDir = Join-Path $root "build\physics-interval-getter-elf-resolver-v2"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "x86 compiler missing" }
$output = Join-Path $outDir "a9tas_physics_interval_getter_elf_resolver_v2_selftest"
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-static-libstdc++" `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "ELF resolver selftest build failed" }
python -B $policy $resolver
if ($LASTEXITCODE -ne 0) { throw "ELF resolver policy failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
Write-Output "a9tas_physics_interval_getter_elf_resolver_v2_selftest sha256=$hash"
Write-Output "PHYSICS_INTERVAL_GETTER_ELF_RESOLVER_V2_BUILD passed=1 runtime=read_only deployed=0 game_writes=0"
