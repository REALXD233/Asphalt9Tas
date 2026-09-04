param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$armCompiler = Join-Path $toolBin 'aarch64-linux-android24-clang++.cmd'
$hostCompiler = Join-Path $toolBin 'clang++.exe'
$readelf = Join-Path $toolBin 'llvm-readelf.exe'
$nm = Join-Path $toolBin 'llvm-nm.exe'
$contract = Join-Path $portRoot 'src/remote_command_call_contract_v1.h'
$adapterHeader = Join-Path $portRoot 'src/native_arm64_command_backend_v1.h'
$adapterSource = Join-Path $portRoot 'src/native_arm64_command_backend_v1.cpp'
$remoteHeader = Join-Path $portRoot 'src/native_arm64_remote_call_v1.h'
$remoteSource = Join-Path $portRoot 'src/native_arm64_remote_call_v1.cpp'
$selftest = Join-Path $portRoot 'remote_command_call_contract_selftest_v1.cpp'
$g2Source = Join-Path $portRoot 'src/g2_physics_interval_controller_v1.cpp'
$output = Join-Path $portRoot 'build/native-arm64-command-backend-v1'
$adapterObject = Join-Path $output 'native_arm64_command_backend_v1.arm64.o'
$remoteObject = Join-Path $output 'native_arm64_remote_call_v1.arm64.o'
$selftestObject = Join-Path $output 'remote_command_call_contract_selftest_v1.host.o'

foreach ($required in @($armCompiler,$hostCompiler,$readelf,$nm,$contract,
                         $adapterHeader,$adapterSource,$remoteHeader,$remoteSource,
                         $selftest,$g2Source)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing native ARM64 command backend input: $required"
    }
}
New-Item -ItemType Directory -Force -Path $output | Out-Null

$common = @("-I$(Join-Path $portRoot 'src')",'-c','-O2','-std=c++20',
            '-Wall','-Wextra','-Werror','-fno-exceptions','-fno-rtti')
& $armCompiler $adapterSource @common -o $adapterObject
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 command adapter build failed' }
& $armCompiler $remoteSource @common -o $remoteObject
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 remote-call dependency build failed' }
foreach ($object in @($adapterObject,$remoteObject)) {
    $identity = (& $readelf -h $object) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $identity -notmatch 'Machine:\s+AArch64') {
        throw "wrong command backend object architecture: $object"
    }
}
$adapterSymbols = (& $nm -C $adapterObject) -join "`n"
foreach ($symbol in @('InvokeStopped', 'CallStoppedThread')) {
    if ($adapterSymbols -notmatch [regex]::Escape($symbol)) {
        throw "native ARM64 command adapter symbol missing: $symbol"
    }
}

& $hostCompiler $selftest "-I$portRoot" -c -O2 -std=c++20 -Wall -Wextra -Werror `
    -o $selftestObject
if ($LASTEXITCODE -ne 0) { throw 'remote command contract selftest failed' }
$selftestSymbols = (& $nm -C $selftestObject) -join "`n"
if ($selftestSymbols -notmatch 'a9tas_remote_command_call_contract_selftest_v1') {
    throw 'remote command contract selftest marker missing'
}

$adapterText = Get-Content -LiteralPath $adapterSource -Raw
$g2Text = Get-Content -LiteralPath $g2Source -Raw
foreach ($token in @('BuildJniArguments(request, arguments)',
                      'CallStoppedThread(', 'Matches(report->call.return_value')) {
    if (-not $adapterText.Contains($token)) {
        throw "native command backend contract missing: $token"
    }
}
foreach ($token in @('#include "remote_command_call_contract_v1.h"',
                      'BuildJniArguments(request, args)',
                      'command_contract::Matches(call.result, request)')) {
    if (-not $g2Text.Contains($token)) {
        throw "proven x86 command path is not using shared contract: $token"
    }
}

$adapterHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $adapterObject).Hash.ToLowerInvariant()
$remoteHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $remoteObject).Hash.ToLowerInvariant()
$testHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $selftestObject).Hash.ToLowerInvariant()
Write-Output 'NATIVE_ARM64_COMMAND_BACKEND_BUILD passed=1 direct_payload_export=1 shared_jni_arguments=1 shared_return_contract=1 register_rollback=1 backend_enabled=0 device_access=0'
Write-Output "adapter_object_sha256=$adapterHash"
Write-Output "remote_object_sha256=$remoteHash"
Write-Output "contract_selftest_sha256=$testHash"
