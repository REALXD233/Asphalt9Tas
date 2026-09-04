param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$resolver = Join-Path $root "src\controller_shadow_coordinator_elf_resolver_v1.h"
$selftest = Join-Path $root "src\controller_shadow_coordinator_elf_resolver_selftest_v1.cpp"
$protocol = Join-Path $root "src\controller_shadow_coordinator_protocol_v1.h"
$payload = Join-Path $root "build\controller-shadow-coordinator-v1\liba9tas_controller_shadow_coordinator_v1_build_only.so"
$outDir = Join-Path $root "build\controller-shadow-elf-resolver-v1"
$object = Join-Path $outDir "controller_shadow_coordinator_elf_resolver_selftest_v1.o"

foreach ($path in @($compiler, $readelf, $resolver, $selftest, $protocol, $payload)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing controller-shadow ELF resolver input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $compiler $selftest "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "controller-shadow ELF resolver ARM64 build failed" }

$symbols = & $readelf "--dyn-syms" $payload
if ($LASTEXITCODE -ne 0) { throw "controller-shadow payload symbol audit failed" }
foreach ($required in @(
    "a9tas_controller_tick_wrapper_v1",
    "a9tas_controller_shadow_storage_data_v1",
    "a9tas_controller_shadow_control_data_v1",
    "a9tas_controller_shadow_evidence_data_v1",
    "a9tas_controller_shadow_frames_data_v1",
    "a9tas_controller_shadow_storage_size_v1",
    "a9tas_controller_shadow_control_size_v1",
    "a9tas_controller_shadow_evidence_size_v1",
    "a9tas_controller_shadow_frame_size_v1",
    "a9tas_controller_shadow_frame_capacity_v1",
    "a9tas_controller_shadow_prefix_size_v1",
    "a9tas_controller_shadow_update_slot_v1"
)) {
    $matches = @($symbols | Select-String -SimpleMatch $required)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one controller-shadow payload export: $required"
    }
}

$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
if ($payloadHash -ne "636d0759cb8ccd4830c7de2b82b6a45b148f98ce4656ada53045e54df3c2f7de") {
    throw "Controller-shadow payload hash no longer matches the resolver pin"
}
$resolverHash = (Get-FileHash -LiteralPath $resolver -Algorithm SHA256).Hash.ToLowerInvariant()
$selftestHash = (Get-FileHash -LiteralPath $selftest -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "CONTROLLER_SHADOW_ELF_RESOLVER_BUILD passed=1 arm64=1 hash_pinned=1 exact_exports=12 read_only=1 device_access=0 deployed=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "resolver_sha256=$resolverHash"
Write-Output "selftest_sha256=$selftestHash"
Write-Output "object_sha256=$objectHash"
