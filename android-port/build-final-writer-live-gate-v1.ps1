param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d",
    [switch]$EmitOneShotLiveCandidate,
    [string]$ExpectedReviewObjectSha256 = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "build\final-writer-live-gate-v1"
$reviewObject = Join-Path $root "build\final-writer-unified-v1\final_writer_unified_v1_review_only.o"
$payload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$bootstrapSource = Join-Path $root "src\bootstrap_final_writer_replay_v1_build.cpp"
$helper = Join-Path $root "tools\run_final_writer_preload_v1.sh"
$policy = Join-Path $root "tools\test_final_writer_live_gate_policy_v1.py"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_final_writer_v1.so"
$candidate = Join-Path $outDir "a9tas_final_writer_live_candidate_v1"
$disassembly = Join-Path $outDir "a9tas_final_writer_live_candidate_v1.disasm.txt"

& (Join-Path $root "build-final-writer-unified-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "Final-writer unified prerequisite failed" }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($path in @($reviewObject, $payload, $bootstrapSource, $helper, $policy,
                    $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing final-writer live-gate input: $path"
    }
}

& $compiler $bootstrapSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Final-writer preload bootstrap build failed" }
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
$bootstrapSymbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
if ($bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Type:\s+DYN" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "Final-writer bootstrap ABI validation failed"
}

if (-not $EmitOneShotLiveCandidate) {
    if (Test-Path -LiteralPath $candidate) { Remove-Item -LiteralPath $candidate -Force }
    if (Test-Path -LiteralPath $disassembly) { Remove-Item -LiteralPath $disassembly -Force }
    $python = Get-Command python.exe -ErrorAction Stop
    & $python.Source $policy
    if ($LASTEXITCODE -ne 0) { throw "Final-writer live-gate policy failed" }
    $bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output "FINAL_WRITER_LIVE_GATE_BUILD passed=1 controller=not_emitted bootstrap=local_only device_access=0"
    Write-Output "bootstrap_sha256=$bootstrapHash"
    return
}

$actualObjectHash = (Get-FileHash -LiteralPath $reviewObject -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ExpectedReviewObjectSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
    $ExpectedReviewObjectSha256.ToLowerInvariant() -ne $actualObjectHash) {
    throw "Explicit review-object SHA-256 acknowledgement mismatch"
}
& $compiler $reviewObject "-static-libstdc++" "-Wl,--no-undefined" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "One-shot final-writer candidate link failed" }
$candidateHeader = (& $readelf -h $candidate) -join "`n"
if ($candidateHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $candidateHeader -notmatch "Type:\s+DYN") {
    Remove-Item -LiteralPath $candidate -Force
    throw "One-shot final-writer candidate ELF mismatch"
}
& $objdump "-d" "--demangle" $candidate | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) {
    Remove-Item -LiteralPath $candidate -Force
    throw "One-shot final-writer disassembly failed"
}
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "FINAL_WRITER_LIVE_GATE_BUILD passed=1 controller=one_shot_local_only deployed=0 device_access=0"
Write-Output "review_object_sha256=$actualObjectHash"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
