param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "natural_action_lifecycle_elf_resolver_selftest.cpp"
$policy = Join-Path $root "tools\test_natural_action_lifecycle_elf_resolver_v1.py"
$payloadPolicy = Join-Path $root "tools\test_natural_action_callback_lifecycle_policy_v1.py"
$payload = Join-Path $root "build\natural-action-callback-lifecycle-v1\liba9tas_natural_action_callback_lifecycle_v1_build_only.so"
$outDir = Join-Path $root "build\natural-action-lifecycle-elf-resolver-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}
if (-not (Test-Path -LiteralPath $payload)) {
    throw "Lifecycle payload must be built before resolver review"
}

$reviewObject = Join-Path $outDir "natural_action_lifecycle_elf_resolver_v1_review_only.o"
$disassembly = Join-Path $outDir "natural_action_lifecycle_elf_resolver_v1_review_only.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-c" "-o" $reviewObject
if ($LASTEXITCODE -ne 0) { throw "lifecycle resolver review object build failed" }

$header = (& $readelf -h $reviewObject) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+REL") {
    throw "lifecycle resolver review object ELF is invalid"
}
& $objdump "-d" "--demangle" $reviewObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "lifecycle resolver disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $payloadPolicy $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "lifecycle payload policy regression failed" }
& $python.Source $policy $payload
if ($LASTEXITCODE -ne 0) { throw "lifecycle resolver policy verification failed" }

$objectHash = (Get-FileHash -LiteralPath $reviewObject -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_LIFECYCLE_ELF_RESOLVER_BUILD passed=1 runtime=disabled review_object=unlinked device_access=0 guest_calls=0"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_disassembly_sha256=$disassemblyHash"
