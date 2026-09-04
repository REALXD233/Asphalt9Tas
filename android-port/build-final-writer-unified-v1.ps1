param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_final_writer_unified_replay_v1.cpp"
$policy = Join-Path $root "tools\test_final_writer_unified_integration_policy_v1.py"
$payload = Join-Path $root "build\final-writer-replay-v1\liba9tas_final_writer_replay_v1_build_only.so"
$outDir = Join-Path $root "build\final-writer-unified-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($path in @($source, $policy, $payload, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing final-writer unified build input: $path"
    }
}

$passive = Join-Path $outDir "final_writer_unified_v1_build_only"
$liveObject = Join-Path $outDir "final_writer_unified_v1_review_only.o"
$disassembly = Join-Path $outDir "final_writer_unified_v1_review_only.disasm.txt"

& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "Final-writer unified passive build failed" }

& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_FINAL_WRITER_LIVE_CANDIDATE=1" "-c" "-o" $liveObject
if ($LASTEXITCODE -ne 0) { throw "Final-writer unified review-object build failed" }

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$linkCheck = [System.IO.Path]::GetFullPath(
    (Join-Path $tempRoot ("a9tas-final-writer-unified-linkcheck-{0}.so" -f $PID)))
if (-not $linkCheck.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Final-writer unified link-check escaped temp"
}
try {
    & $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
        "-fno-exceptions" "-fno-rtti" "-fPIC" "-shared" `
        "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
        "-DA9TAS_FINAL_WRITER_LIVE_CANDIDATE=1" "-Wl,--no-undefined" `
        "-o" $linkCheck
    if ($LASTEXITCODE -ne 0) { throw "Final-writer unified ephemeral link failed" }
    $header = (& $readelf -h $linkCheck) -join "`n"
    if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
        $header -notmatch "Type:\s+DYN") {
        throw "Final-writer unified ephemeral link artifact invalid"
    }
} finally {
    if (Test-Path -LiteralPath $linkCheck) {
        Remove-Item -LiteralPath $linkCheck -Force
    }
}
if (Test-Path -LiteralPath $linkCheck) {
    throw "Final-writer unified ephemeral link artifact retained"
}

& $objdump "-d" "--demangle" $liveObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Final-writer unified disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $liveObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Final-writer unified policy failed" }
& $python.Source (Join-Path $root "tools\test_final_writer_replay_elf_resolver_v1.py") $payload
if ($LASTEXITCODE -ne 0) { throw "Final-writer payload resolver regression failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $liveObject -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "FINAL_WRITER_UNIFIED_BUILD passed=1 runtime=disabled live_object=unlinked link_check=ephemeral_pass device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_disassembly_sha256=$disasmHash"
