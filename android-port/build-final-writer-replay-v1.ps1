param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_final_writer_replay_v1.cpp"
$protocol = Join-Path $root "src\final_writer_replay_protocol_v1.h"
$policy = Join-Path $root "tools\test_final_writer_replay_policy_v1.py"
$resolverPolicy = Join-Path $root "tools\test_final_writer_replay_elf_resolver_v1.py"
$targetPolicy = Join-Path $root "tools\test_final_writer_target_blob_v1.py"
$cursorPolicy = Join-Path $root "tools\test_final_writer_cursor_binding_v1.py"
$transactionPolicy = Join-Path $root "tools\test_final_writer_transaction_core_v1.py"
$storagePolicy = Join-Path $root "tools\test_final_writer_storage_transaction_v1.py"
$outDir = Join-Path $root "build\final-writer-replay-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$output = Join-Path $outDir "liba9tas_final_writer_replay_v1_build_only.so"
$disassembly = Join-Path $outDir "final_writer_replay_v1.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-fno-stack-protector" `
    "-Wall" "-Wextra" "-Werror" "-fPIC" "-shared" `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "Final-writer ARM64 build failed" }

& $objdump "-d" "--demangle" $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Final-writer disassembly failed" }

foreach ($selftest in @(
    "src\final_writer_replay_elf_resolver_selftest_v1.cpp",
    "src\final_writer_cursor_binding_selftest_v1.cpp",
    "src\final_writer_transaction_core_selftest_v1.cpp",
    "src\final_writer_storage_transaction_selftest_v1.cpp"
)) {
    & $compiler (Join-Path $root $selftest) "-I$(Join-Path $root 'src')" `
        "-std=c++20" "-Wall" "-Wextra" "-Werror" "-fsyntax-only"
    if ($LASTEXITCODE -ne 0) { throw "Final-writer host-core syntax proof failed: $selftest" }
}

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $output $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Final-writer policy verification failed" }
& $python.Source $resolverPolicy $output
if ($LASTEXITCODE -ne 0) { throw "Final-writer resolver verification failed" }
foreach ($test in @($targetPolicy, $cursorPolicy, $transactionPolicy, $storagePolicy)) {
    & $python.Source "-m" "unittest" $test "-q"
    if ($LASTEXITCODE -ne 0) { throw "Final-writer host-core unit test failed: $test" }
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolHash = (Get-FileHash -LiteralPath $protocol -Algorithm SHA256).Hash.ToLowerInvariant()
$policyHash = (Get-FileHash -LiteralPath $policy -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "FINAL_WRITER_REPLAY_BUILD passed=1 build_only=1 resolver=1 target_blob=1 cursor_binding=1 storage_transaction=1 deployed=0 device_access=0"
Write-Output "payload_sha256=$hash"
Write-Output "source_sha256=$sourceHash"
Write-Output "protocol_sha256=$protocolHash"
Write-Output "policy_sha256=$policyHash"
Write-Output "disassembly_sha256=$disasmHash"
