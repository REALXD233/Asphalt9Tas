param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\controller_shadow_m1_preflight_v1.cpp"
$policy = Join-Path $root "tools\test_controller_shadow_m1_preflight_policy_v1.py"
$outDir = Join-Path $root "build\controller-shadow-m1-preflight-v1"
$candidate = Join-Path $outDir "a9tas_controller_shadow_m1_preflight_v1_review_only"
$object = Join-Path $outDir "controller_shadow_m1_preflight_v1_review_only.o"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$nm = Join-Path $toolBin "llvm-nm.exe"

foreach ($path in @($source, $policy, $compiler, $readelf, $nm)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing M1 preflight build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source -B $policy $source
if ($LASTEXITCODE -ne 0) { throw "M1 read-only preflight source policy failed" }

& $compiler $source "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-DA9TAS_M1_PREFLIGHT_REVIEW=1" "-fno-exceptions" "-fno-rtti" `
    "-ffunction-sections" "-fdata-sections" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "M1 read-only preflight object build failed" }
& $compiler $object "-static-libstdc++" "-Wl,--no-undefined" `
    "-Wl,--gc-sections" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "M1 read-only preflight link failed" }
$header = (& $readelf "-h" $candidate) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN") {
    throw "M1 preflight candidate ELF mismatch"
}
$undefinedSymbols = (& $nm "--undefined-only" $candidate) -join "`n"
if ($undefinedSymbols -match "(?im)(^|\s)(ptrace|pwrite|pwrite64|process_vm_writev)(@|$)") {
    throw "M1 preflight candidate unexpectedly imports a process mutation primitive"
}

$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "M1_CONTROLLER_PREFLIGHT_BUILD passed=1 x86_64=1 read_only=1 staged_diagnostics=1 base_mode=1 process_generation=1 unique_controller=1 countdown_lifecycle=1 payloads=3 factory_payloads=2 natural_registered=1 target_blob_bound=1 attach=0 process_writes=0 device_access=0 deployed=0"
Write-Output "source_sha256=$sourceHash"
Write-Output "object_sha256=$objectHash"
Write-Output "candidate_sha256=$candidateHash"
