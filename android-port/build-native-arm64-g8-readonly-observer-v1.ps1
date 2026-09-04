param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$compiler = Join-Path $toolBin 'aarch64-linux-android24-clang++.cmd'
$readelf = Join-Path $toolBin 'llvm-readelf.exe'
$alignmentPolicy = Join-Path $portRoot 'tools/assert_elf_load_alignment_v1.ps1'
$source = Join-Path $portRoot 'src/g8_profile_physics_interval_readonly_observer_v1.cpp'
$includedSource = Join-Path $portRoot 'src/physics_interval_readonly_observer_v1.cpp'
$runner = Join-Path $portRoot 'run-physics-interval-readonly-v1.ps1'
$policy = Join-Path $portRoot 'tools/test_physics_interval_readonly_policy_v1.py'
$parserTests = Join-Path $portRoot 'tools/test_parse_physics_interval_readonly_v1.py'
$output = Join-Path $portRoot 'build/native-arm64-g8-readonly-observer-v1'
$observer = Join-Path $output 'a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1'

foreach ($required in @($compiler,$readelf,$alignmentPolicy,$source,$includedSource,$runner,
                         $policy,$parserTests)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing native ARM64 observer input: $required"
    }
}
. $alignmentPolicy
New-Item -ItemType Directory -Force -Path $output | Out-Null
& $compiler -O2 -std=c++20 -static-libstdc++ -Wall -Wextra -Werror `
    '-Wl,-z,max-page-size=16384' '-Wl,-z,common-page-size=16384' `
    $source -o $observer
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 G8 observer build failed' }

$header = (& $readelf -h $observer) -join "`n"
if ($LASTEXITCODE -ne 0 -or $header -notmatch 'Machine:\s+AArch64' -or
    $header -notmatch 'Type:\s+DYN') {
    throw 'native ARM64 G8 observer ELF identity failed'
}
$programs = (& $readelf -l $observer) -join "`n"
if ($LASTEXITCODE -ne 0 -or
    $programs -match 'LOAD\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+RWE') {
    throw 'native ARM64 G8 observer W-X policy failed'
}
Assert-A9TasElfLoadAlignment -ReadElf $readelf -ElfPath $observer `
    -ExpectedAlignment 0x4000 -Label 'native ARM64 G8 observer'
python -B $policy $includedSource $runner
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 observer policy failed' }
python -B $parserTests
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 observer parser tests failed' }

$text = Get-Content -LiteralPath $includedSource -Raw
foreach ($forbidden in @('PTRACE_','ptrace(','O_RDWR','pwrite(')) {
    if ($text.Contains($forbidden)) {
        throw "native ARM64 observer gained forbidden mutation: $forbidden"
    }
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $observer).Hash.ToLowerInvariant()
Write-Output 'NATIVE_ARM64_G8_READONLY_OBSERVER_BUILD passed=1 arm64_executable=1 profile_driven=1 ptrace=0 game_writes=0 nativebridge_dependency=0 backend_enabled=0 device_access=0'
Write-Output "observer_sha256=$hash"
