param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_recording_elf_resolver_build_only.cpp"
$header = Join-Path $root "src\natural_action_recording_elf_resolver_v1.h"
$payload = Join-Path $root "build\natural-action-recording-payload-v1\liba9tas_natural_action_recording_v1_review_only.so"
$policy = Join-Path $root "tools\test_natural_action_recording_elf_resolver_v1.py"
$outDir = Join-Path $root "build\natural-action-recording-elf-resolver-v1"
$object = Join-Path $outDir "natural_action_recording_elf_resolver_v1_review_only.o"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($path in @($source, $header, $payload, $policy, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing recording resolver input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "Recording resolver compile failed" }
python -B $policy $header $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Recording resolver policy failed" }
$hash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_RECORDING_ELF_RESOLVER_BUILD passed=1 deployed=0 device_access=0"
Write-Output "review_object_sha256=$hash"

