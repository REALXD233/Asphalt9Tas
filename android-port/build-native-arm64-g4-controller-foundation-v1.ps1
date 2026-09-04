param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$compiler = Join-Path $toolBin 'aarch64-linux-android24-clang++.cmd'
$readelf = Join-Path $toolBin 'llvm-readelf.exe'
$nm = Join-Path $toolBin 'llvm-nm.exe'
$source = Join-Path $portRoot 'native_arm64_g4_controller_foundation_build_only_v1.cpp'
$foundation = Join-Path $portRoot 'src/native_arm64_g4_controller_foundation_v1.cpp'
$command = Join-Path $portRoot 'src/native_arm64_command_backend_v1.cpp'
$remote = Join-Path $portRoot 'src/native_arm64_remote_call_v1.cpp'
$trap = Join-Path $portRoot 'src/native_arm64_immutable_trap_resolver_v1.cpp'
$resolver = Join-Path $portRoot 'src/native_arm64_g4_payload_resolver_v1.h'
$freeze = Join-Path $portRoot 'src/ptrace_stable_freeze_v1.h'
$output = Join-Path $portRoot 'build/native-arm64-g4-controller-foundation-v1'
$objects = @(
    (Join-Path $output 'foundation_probe.arm64.o'),
    (Join-Path $output 'foundation.arm64.o'),
    (Join-Path $output 'command.arm64.o'),
    (Join-Path $output 'remote_call.arm64.o'),
    (Join-Path $output 'immutable_trap.arm64.o')
)

foreach ($required in @($compiler,$readelf,$nm,$source,$foundation,$command,
                         $remote,$trap,$resolver,$freeze)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing native G4 controller foundation input: $required"
    }
}
New-Item -ItemType Directory -Force -Path $output | Out-Null
$sources = @($source,$foundation,$command,$remote,$trap)
$common = @("-I$(Join-Path $portRoot 'src')",'-c','-O2','-std=c++20',
            '-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti')
for ($index=0; $index -lt $sources.Count; $index++) {
    & $compiler $sources[$index] @common -o $objects[$index]
    if ($LASTEXITCODE -ne 0) {
        throw "native G4 controller foundation compile failed: $($sources[$index])"
    }
    $identity = (& $readelf -h $objects[$index]) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $identity -notmatch 'Machine:\s+AArch64') {
        throw "native G4 controller object identity failed: $($objects[$index])"
    }
}
$symbols = (& $nm -C $objects[0..1]) -join "`n"
foreach ($symbol in @(
    'a9tas_native_arm64_g4_controller_foundation_build_only_v1',
    'PrepareStopped', 'InvokePreparedStopped')) {
    if ($symbols -notmatch [regex]::Escape($symbol)) {
        throw "native G4 controller foundation symbol missing: $symbol"
    }
}
$text = (Get-Content -LiteralPath $foundation -Raw) +
        (Get-Content -LiteralPath (Join-Path $portRoot 'src/native_arm64_g4_controller_foundation_v1.h') -Raw)
foreach ($token in @('ptrace_stable_freeze_v1::Complete',
                      'native_arm64_g4_payload_resolver_v1::Resolve',
                      'native_arm64_immutable_trap_resolver_v1::Resolve',
                      'native_arm64_command_backend_v1::InvokeStopped')) {
    if (-not $text.Contains($token)) {
        throw "native G4 controller foundation invariant missing: $token"
    }
}
foreach ($forbidden in @('libhoudini','libnb.so','guest_trampoline','bootstrap_base')) {
    if ($text.Contains($forbidden)) {
        throw "native G4 controller foundation retained bridge dependency: $forbidden"
    }
}
$hashes = $objects | ForEach-Object {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash.ToLowerInvariant()
}
Write-Output 'NATIVE_ARM64_G4_CONTROLLER_FOUNDATION_BUILD passed=1 stopped_transaction=1 direct_payload=1 immutable_trap=1 shared_command_contract=1 nativebridge_dependency=0 backend_enabled=0 device_access=0'
Write-Output "foundation_object_sha256=$($hashes[1])"
Write-Output "probe_object_sha256=$($hashes[0])"
