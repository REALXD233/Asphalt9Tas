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
$source = Join-Path $portRoot 'src/native_arm64_remote_call_v1.cpp'
$header = Join-Path $portRoot 'src/native_arm64_remote_call_v1.h'
$selftest = Join-Path $portRoot 'native_arm64_remote_call_selftest_v1.cpp'
$output = Join-Path $portRoot 'build/native-arm64-backend-foundation-v1'
$armObject = Join-Path $output 'native_arm64_remote_call_v1.arm64.o'
$selftestObject = Join-Path $output 'native_arm64_remote_call_selftest_v1.host.o'

foreach ($required in @($armCompiler,$hostCompiler,$readelf,$nm,$source,$header,$selftest)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing native ARM64 backend input: $required"
    }
}
New-Item -ItemType Directory -Force -Path $output | Out-Null

& $armCompiler $source "-I$(Join-Path $portRoot 'src')" -c -O2 -std=c++20 `
    -Wall -Wextra -Werror -fno-exceptions -fno-rtti -o $armObject
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 remote-call adapter build failed' }

$headerText = (& $readelf -h $armObject) -join "`n"
if ($LASTEXITCODE -ne 0 -or $headerText -notmatch 'Machine:\s+AArch64') {
    throw 'native ARM64 remote-call adapter has the wrong architecture'
}
$symbols = (& $nm -C $armObject) -join "`n"
foreach ($symbol in @('GetRegisters', 'SetRegistersExact', 'CallStoppedThread')) {
    if ($symbols -notmatch [regex]::Escape($symbol)) {
        throw "native ARM64 adapter symbol missing: $symbol"
    }
}

$sourceText = Get-Content -LiteralPath $source -Raw
foreach ($forbidden in @('PTRACE_POKETEXT','PTRACE_POKEDATA','process_vm_writev')) {
    if ($sourceText.Contains($forbidden)) {
        throw "native ARM64 foundation unexpectedly writes tracee memory: $forbidden"
    }
}
foreach ($requiredToken in @('result.regs[index] = source.x[index]',
                              'result.x[index] = source.regs[index]',
                              'output->registers.x[30] = trap',
                              'report->rollback_succeeded = SetRegistersExact(tid, original)')) {
    if (-not (($sourceText + (Get-Content -LiteralPath $header -Raw)).Contains($requiredToken))) {
        throw "native ARM64 register invariant missing: $requiredToken"
    }
}

& $hostCompiler $selftest "-I$portRoot" -c -O2 -std=c++20 -Wall -Wextra -Werror `
    -o $selftestObject
if ($LASTEXITCODE -ne 0) { throw 'native ARM64 semantic selftest build failed' }
$selftestSymbols = (& $nm -C $selftestObject) -join "`n"
if ($selftestSymbols -notmatch 'a9tas_native_arm64_remote_call_semantic_selftest_v1') {
    throw 'native ARM64 compile-time semantic selftest marker missing'
}

$armHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $armObject).Hash.ToLowerInvariant()
$testHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $selftestObject).Hash.ToLowerInvariant()
Write-Output "NATIVE_ARM64_BACKEND_FOUNDATION passed=1 arm64_object=1 semantic_selftest=1 tracee_memory_writes=0 device_access=0"
Write-Output "arm64_object_sha256=$armHash"
Write-Output "selftest_sha256=$testHash"
