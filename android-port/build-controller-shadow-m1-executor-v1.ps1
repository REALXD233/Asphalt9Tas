param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\controller_shadow_m1_executor_v1.cpp"
$policy = Join-Path $root "tools\test_controller_shadow_m1_executor_policy_v1.py"
$outDir = Join-Path $root "build\controller-shadow-m1-executor-v1"
$passive = Join-Path $outDir "a9tas_controller_shadow_m1_executor_v1_build_only"
$object = Join-Path $outDir "controller_shadow_m1_executor_v1_review_only.o"
$candidate = Join-Path $outDir "a9tas_controller_shadow_m1_executor_v1_review_only"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$nm = Join-Path $toolBin "llvm-nm.exe"

foreach ($path in @($source, $policy, $compiler, $readelf, $nm)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing M1 executor build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $source
if ($LASTEXITCODE -ne 0) { throw "M1 executor source policy failed" }

& $compiler $source "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-static-libstdc++" "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "M1 executor passive build failed" }
$passiveUndefined = (& $nm "--undefined-only" $passive) -join "`n"
if ($passiveUndefined -match "(?im)(^|\s)(ptrace|pwrite|process_vm_writev)(@|$)") {
    throw "M1 executor passive artifact unexpectedly exposes runtime mutation"
}

& $compiler $source "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-DA9TAS_M1_EXECUTOR_REVIEW=1" "-fno-exceptions" "-fno-rtti" `
    "-ffunction-sections" "-fdata-sections" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "M1 executor review object build failed" }
& $compiler $object "-static-libstdc++" "-Wl,--no-undefined" `
    "-Wl,--gc-sections" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "M1 executor review candidate link failed" }

$header = (& $readelf "-h" $candidate) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN") {
    throw "M1 executor candidate ELF mismatch"
}
$undefined = (& $nm "--undefined-only" $candidate) -join "`n"
foreach ($required in @('ptrace', 'pread', 'pwrite', 'waitpid')) {
    if ($undefined -notmatch "(?im)(^|\s)$required(@|$)") {
        throw "M1 executor candidate lacks required runtime primitive: $required"
    }
}
if ($undefined -match "(?im)(^|\s)process_vm_writev(@|$)") {
    throw "M1 executor unexpectedly imports process_vm_writev"
}

$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "M1_CONTROLLER_EXECUTOR_BUILD passed=1 default_runtime=disabled x86_64=1 hwbp_addresses=1 external_fixed_delta_only=1 frozen_ready_gate=1 controller_payload_input=1 natural_action_mailbox=1 final_writer_payload=1 deployed=0 device_access=0"
Write-Output "source_sha256=$sourceHash"
Write-Output "passive_sha256=$passiveHash"
Write-Output "object_sha256=$objectHash"
Write-Output "candidate_sha256=$candidateHash"
