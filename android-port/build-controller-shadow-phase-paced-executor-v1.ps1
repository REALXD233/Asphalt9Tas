param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\controller_shadow_m1_executor_v1.cpp"
$policy = Join-Path $root "tools\test_controller_shadow_phase_paced_executor_policy_v1.py"
$coreBuild = Join-Path $root "build-controller-shadow-phase-paced-core-v1.ps1"
$legacyCandidate = Join-Path $root "build\controller-shadow-m1-executor-v1\a9tas_controller_shadow_m1_executor_v1_review_only"
$outDir = Join-Path $root "build\controller-shadow-phase-paced-executor-v1"
$object = Join-Path $outDir "controller_shadow_phase_paced_executor_v1_review_only.o"
$disassembly = Join-Path $outDir "controller_shadow_phase_paced_executor_v1_review_only.disasm.txt"
$candidate = Join-Path $outDir "a9tas_controller_shadow_phase_paced_executor_v1_reviewed"
$candidateDisassembly = Join-Path $outDir "a9tas_controller_shadow_phase_paced_executor_v1_reviewed.disasm.txt"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"

foreach ($path in @($source, $policy, $coreBuild, $legacyCandidate,
                     $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing phase-paced executor build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$legacyHash = (Get-FileHash -LiteralPath $legacyCandidate -Algorithm SHA256).Hash.ToLowerInvariant()
if ($legacyHash -ne "b1c913b28ff7e77c4245419ef1c540270cbff94113513ae5749e23950dfdc795") {
    throw "Superseded single-address candidate changed unexpectedly"
}

& $coreBuild -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "Phase-paced core build failed" }
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $source
if ($LASTEXITCODE -ne 0) { throw "Phase-paced executor policy failed" }

& $compiler $source "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-DA9TAS_M1_EXECUTOR_REVIEW=1" `
    "-DA9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1=1" `
    "-fno-exceptions" "-fno-rtti" "-ffunction-sections" `
    "-fdata-sections" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "Phase-paced executor review object build failed" }

$header = (& $readelf "-h" $object) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+REL") {
    throw "Phase-paced executor review object ELF mismatch"
}
& $objdump "-d" "--demangle" $object | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Phase-paced executor disassembly failed" }

& $compiler $object "-static-libstdc++" "-Wl,--no-undefined" `
    "-Wl,--gc-sections" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "Phase-paced reviewed candidate link failed" }
$linkedHeader = (& $readelf "-h" $candidate) -join "`n"
if ($linkedHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $linkedHeader -notmatch "Type:\s+DYN") {
    throw "Phase-paced reviewed candidate ELF mismatch"
}
& $objdump "-d" "--demangle" $candidate | Set-Content -LiteralPath $candidateDisassembly
if ($LASTEXITCODE -ne 0) { throw "Phase-paced candidate disassembly failed" }

$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateDisassemblyHash = (Get-FileHash -LiteralPath $candidateDisassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "PHASE_PACED_EXECUTOR_BUILD passed=1 runtime_artifact=reviewed_candidate review_object=1 legacy_candidate_unchanged=1 device_access=0 deployed=0"
Write-Output "source_sha256=$sourceHash"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_disassembly_sha256=$disassemblyHash"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "candidate_disassembly_sha256=$candidateDisassemblyHash"
