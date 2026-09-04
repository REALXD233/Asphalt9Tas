param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$planBuilder = Join-Path $root "tools\build_barrel_successor_replacement_plan_v1.py"
$generator = Join-Path $root "tools\generate_barrel_successor_source_v1.py"
$policy = Join-Path $root "tools\test_barrel_successor_executor_policy_v1.py"
$semanticSource = Join-Path $root "src\barrel_stabilization_replay_core_v1.cpp"
$runtimeHeader = Join-Path $root "src\barrel_successor_runtime_v1.h"
$payload = Join-Path $root "build\barrel-yaw-tail-payload-v1\liba9tas_barrel_yaw_tail_v1_build_only.so"
$outDir = Join-Path $root "build\barrel-successor-executor-v1"
$plan = Join-Path $outDir "replacement-plan.json"
$stage = Join-Path $outDir ("source-" + [guid]::NewGuid().ToString("N"))
$object = Join-Path $outDir "barrel_successor_executor_v1.o"
$semanticObject = Join-Path $outDir "barrel_stabilization_replay_core_v1.o"
$candidate = Join-Path $outDir "a9tas_barrel_successor_v1_review_only"
$disassembly = Join-Path $outDir "barrel_successor_executor_v1.disasm.txt"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"

foreach ($path in @($planBuilder, $generator, $policy, $semanticSource,
                     $runtimeHeader, $payload, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing barrel successor executor input: $path"
    }
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
python -B $planBuilder $plan
if ($LASTEXITCODE -ne 0) { throw "Barrel successor plan build failed" }
python -B $generator --android-port-root $root --replacements-json $plan --check-only
if ($LASTEXITCODE -ne 0) { throw "Barrel successor source preflight failed" }
python -B $generator --android-port-root $root --replacements-json $plan --output-dir $stage
if ($LASTEXITCODE -ne 0) { throw "Barrel successor source generation failed" }

& $compiler (Join-Path $stage "barrel_successor_entry_v1.cpp") `
    "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-I$stage" "-I$(Join-Path $root 'src')" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "Barrel successor executor compile failed" }
& $compiler $semanticSource "-DA9TAS_BARREL_STABILIZATION_REPLAY_NO_MAIN=1" `
    "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-c" "-o" $semanticObject
if ($LASTEXITCODE -ne 0) { throw "Barrel semantic core compile failed" }
& $compiler $object $semanticObject "-static-libstdc++" `
    "-Wl,--no-undefined" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "Barrel successor executor link failed" }

$header = (& $readelf "-h" $candidate) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN") {
    throw "Barrel successor executor ELF mismatch"
}
& $objdump "-d" "--demangle" $object | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Barrel successor disassembly failed" }
python -B $policy $root $stage $candidate
if ($LASTEXITCODE -ne 0) { throw "Barrel successor executor policy failed" }

$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath (Join-Path $stage "hwbp_unified_tick_executor_v1.cpp") -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "BARREL_SUCCESSOR_EXECUTOR_BUILD passed=1 baseline_unchanged=1 classifier=0 deployed=0 device_access=0"
Write-Output "generated_source=$stage"
Write-Output "generated_source_sha256=$sourceHash"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_candidate_sha256=$candidateHash"
