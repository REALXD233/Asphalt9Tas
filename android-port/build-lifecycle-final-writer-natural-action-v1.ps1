param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_lifecycle_final_writer_natural_action_replay_v1.cpp"
$integration = Join-Path $root "src\final_writer_unified_integration_v1.h"
$runtime = Join-Path $root "src\final_writer_natural_action_runtime_v1.h"
$policy = Join-Path $root "tools\test_lifecycle_final_writer_natural_action_policy_v1.py"
$writerPayload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$actionPayload = Join-Path $root "build\natural-action-replay-payload-v1\liba9tas_natural_action_replay_v1_review_only.so"
$outDir = Join-Path $root "build\lifecycle-final-writer-natural-action-v1"
$object = Join-Path $outDir "lifecycle_final_writer_natural_action_v1_review_only.o"
$candidate = Join-Path $outDir "a9tas_lifecycle_final_writer_natural_action_v1_review_only"
$disassembly = Join-Path $outDir "lifecycle_final_writer_natural_action_v1_review_only.disasm.txt"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"

foreach ($path in @($source, $integration, $runtime, $policy, $writerPayload,
                     $actionPayload, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing composite lifecycle replay input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "Composite lifecycle review build failed" }
& $compiler $object "-static-libstdc++" "-Wl,--no-undefined" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "Composite lifecycle retained link failed" }
$header = (& $readelf -h $candidate) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN") {
    throw "Composite lifecycle retained ELF mismatch"
}
& $objdump "-d" "--demangle" $object | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Composite lifecycle disassembly failed" }
python -B $policy $object
if ($LASTEXITCODE -ne 0) { throw "Composite lifecycle policy failed" }
python -B (Join-Path $root "tools\test_natural_action_replay_elf_resolver_v1.py") $actionPayload
if ($LASTEXITCODE -ne 0) { throw "Natural-action replay payload identity failed" }
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "LIFECYCLE_FINAL_WRITER_NATURAL_ACTION_BUILD passed=1 lifecycle_tick0=1 per_frame_mailbox=1 dual_receipt=1 deployed=0 device_access=0"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_candidate_sha256=$candidateHash"
Write-Output "review_disassembly_sha256=$disasmHash"
