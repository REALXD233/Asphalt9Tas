param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "fc2_payload_elf_resolver_selftest.cpp"
$policy = Join-Path $root "tools\test_fc2_payload_elf_resolver_v1.py"
$payloadPolicy = Join-Path $root "tools\test_frame_callback_deferred_registration_policy_v1.py"
$payload = Join-Path $root "build\frame-callback-deferred-registration-v1\liba9tas_frame_callback_deferred_registration_v1_build_only.so"
$outDir = Join-Path $root "build\fc2-payload-elf-resolver-v1"
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
    throw "FC-2 payload must be built before resolver review"
}

$reviewObject = Join-Path $outDir "fc2_payload_elf_resolver_v1_review_only.o"
$disassembly = Join-Path $outDir "fc2_payload_elf_resolver_v1_review_only.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-c" "-o" $reviewObject
if ($LASTEXITCODE -ne 0) { throw "FC-2 resolver review object build failed" }

$header = (& $readelf -h $reviewObject) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+REL") {
    throw "FC-2 resolver review object ELF is invalid"
}
& $objdump "-d" "--demangle" $reviewObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-2 resolver disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $payloadPolicy $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-2 payload policy regression failed" }
& $python.Source $policy $payload
if ($LASTEXITCODE -ne 0) { throw "FC-2 resolver policy verification failed" }

$objectHash = (Get-FileHash -LiteralPath $reviewObject -Algorithm SHA256).Hash.ToLower()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "FC2_ELF_RESOLVER_BUILD passed=1 runtime=disabled review_object=unlinked device_access=0 guest_calls=0"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_disassembly_sha256=$disassemblyHash"
