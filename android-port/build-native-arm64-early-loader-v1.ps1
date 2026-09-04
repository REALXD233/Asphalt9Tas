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
$alignmentPolicy = Join-Path $portRoot 'tools/assert_elf_load_alignment_v1.ps1'
$loaderSource = Join-Path $portRoot 'src/native_arm64_early_loader_v1.cpp'
$trapHeader = Join-Path $portRoot 'src/native_arm64_immutable_trap_resolver_v1.h'
$trapSource = Join-Path $portRoot 'src/native_arm64_immutable_trap_resolver_v1.cpp'
$callSource = Join-Path $portRoot 'src/native_arm64_remote_call_v1.cpp'
$transactionHeader = Join-Path $portRoot 'src/native_arm64_loader_transaction_v1.h'
$selftest = Join-Path $portRoot 'native_arm64_loader_transaction_selftest_v1.cpp'
$policy = Join-Path $portRoot 'tools/test_native_arm64_early_loader_policy_v1.py'
$payload = Join-Path $portRoot 'A9TasAndroid/app/src/main/assets/runtime/liba9tas_g4_multi_hook_runtime_v1.so'
$output = Join-Path $portRoot 'build/native-arm64-early-loader-v1'
$loader = Join-Path $output 'a9tas_native_arm64_early_loader_v1'
$selftestObject = Join-Path $output 'native_arm64_loader_transaction_selftest_v1.host.o'

foreach ($required in @($armCompiler,$hostCompiler,$readelf,$nm,$alignmentPolicy,$loaderSource,
                         $trapHeader,$trapSource,$callSource,$transactionHeader,
                         $selftest,$policy,$payload)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing native ARM64 loader input: $required"
    }
}
. $alignmentPolicy
New-Item -ItemType Directory -Force -Path $output | Out-Null

$payloadHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $payload).Hash.ToLowerInvariant()
$payloadDevicePath = '/data/local/tmp/liba9tas_g4_multi_hook_runtime_v1.so'
& $armCompiler $loaderSource $trapSource $callSource `
    "-I$(Join-Path $portRoot 'src')" `
    "-DA9TAS_NATIVE_ARM64_PAYLOAD_PATH=$payloadDevicePath" `
    "-DA9TAS_NATIVE_ARM64_PAYLOAD_SHA256=$payloadHash" `
    -O2 -std=c++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti `
    -fPIE -pie -static-libstdc++ "-Wl,-z,relro,-z,now" `
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" `
    -ldl -o $loader
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 early-loader build failed' }

$headerText = (& $readelf -h $loader) -join "`n"
if ($LASTEXITCODE -ne 0 -or $headerText -notmatch 'Machine:\s+AArch64' -or
    $headerText -notmatch 'Type:\s+DYN') {
    throw 'native ARM64 early-loader has the wrong ELF identity'
}
$programs = (& $readelf -l $loader) -join "`n"
if ($LASTEXITCODE -ne 0 -or $programs -notmatch 'GNU_RELRO' -or
    $programs -match 'LOAD\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+RWE') {
    throw 'native ARM64 early-loader hardening or W-X policy failed'
}
Assert-A9TasElfLoadAlignment -ReadElf $readelf -ElfPath $loader `
    -ExpectedAlignment 0x4000 -Label 'native ARM64 early-loader'
$dynamic = (& $readelf -d $loader) -join "`n"
if ($LASTEXITCODE -ne 0 -or $dynamic -match 'libc[+][+]_shared[.]so') {
    throw 'native ARM64 early-loader must not depend on a deployed libc++_shared.so'
}
$symbols = (& $nm -C $loader) -join "`n"
foreach ($symbol in @(' main', 'GetRegisters', 'SetRegistersExact')) {
    if ($symbols -notmatch [regex]::Escape($symbol)) {
        throw "native ARM64 early-loader symbol missing: $symbol"
    }
}

& $hostCompiler $selftest "-I$portRoot" -c -O2 -std=c++20 -Wall -Wextra -Werror `
    -o $selftestObject
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 loader transaction selftest failed' }
$selftestSymbols = (& $nm -C $selftestObject) -join "`n"
if ($selftestSymbols -notmatch 'a9tas_native_arm64_loader_transaction_selftest_v1') {
    throw 'native ARM64 loader transaction marker missing'
}

python $policy
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 early-loader policy failed' }

$sourceText = (Get-Content -LiteralPath $loaderSource -Raw) +
              (Get-Content -LiteralPath $transactionHeader -Raw)
foreach ($token in @('UniqueSignalCatcher(pid)',
                      'PTRACE_O_EXITKILL',
                      'trap_resolver::Resolve(pid,&trap_report)',
                      'call::CallStoppedThread(tid,call_origin,remote_dlopen',
                      'ledger.stack_restored=WriteAndReadback',
                      'ledger.registers_restored=call::SetRegistersExact(tid,original)',
                      '!tx::DetachSafe(ledger)',
                      'KillUncertain(pid)',
                      'HasExactPayloadMap(ReadMaps(pid),payload_path,payload)')) {
    if (-not $sourceText.Contains($token)) {
        throw "native ARM64 loader transaction invariant missing: $token"
    }
}

$loaderHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $loader).Hash.ToLowerInvariant()
$selftestHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $selftestObject).Hash.ToLowerInvariant()
Write-Output "NATIVE_ARM64_EARLY_LOADER_BUILD passed=1 arm64_executable=1 transaction_selftest=1 immutable_brk=1 signal_catcher=1 stack_register_rollback=1 backend_enabled=0 device_access=0"
Write-Output "loader_sha256=$loaderHash"
Write-Output "payload_sha256=$payloadHash"
Write-Output "selftest_sha256=$selftestHash"
