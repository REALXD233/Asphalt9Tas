param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_controller_shadow_coordinator_v1.cpp"
$protocol = Join-Path $root "src\controller_shadow_coordinator_protocol_v1.h"
$outDir = Join-Path $root "build\controller-shadow-coordinator-v1"
$payload = Join-Path $outDir "liba9tas_controller_shadow_coordinator_v1_build_only.so"
$disassembly = Join-Path $outDir "controller_shadow_coordinator_v1.disasm.txt"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"

foreach ($path in @($source, $protocol, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing controller-shadow coordinator input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $compiler $source "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-shared" "-fPIC" "-static-libstdc++" "-fno-exceptions" "-fno-rtti" `
    "-fno-stack-protector" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-Wl,--build-id=sha1" "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "controller-shadow coordinator build failed" }

& $objdump "-d" "--demangle" $payload | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "controller-shadow coordinator disassembly failed" }
$disassemblyText = Get-Content -LiteralPath $disassembly -Raw
$valueC = [regex]::Match($disassemblyText, '(?s)<ProxyGetValueC>:(.*?)<ProxyGetSteering>:')
$steering = [regex]::Match($disassemblyText, '(?s)<ProxyGetSteering>:(.*?)<ProxyGetBrake>:')
if (-not $valueC.Success -or
    $valueC.Groups[1].Value -notmatch '(?s)f9400400.*f9400009.*f9400929.*d61f0120' -or
    $valueC.Groups[1].Value -match 'd65f03c0') {
    throw "ProxyGetValueC must tail-call the live source +0x10 getter without clobbering X8"
}
if (-not $steering.Success -or
    $steering.Groups[1].Value -notmatch '(?s)b9401009.*b9000109.*d65f03c0') {
    throw "ProxyGetSteering aggregate-return ABI audit failed"
}
$brake = [regex]::Match($disassemblyText, '(?s)<ProxyGetBrake>:(.*?)<ProxyNoop>')
if (-not $brake.Success -or
    $brake.Groups[1].Value -notmatch '(?s)b9401409.*b9000109.*d65f03c0') {
    throw "ProxyGetBrake aggregate-return ABI audit failed"
}

$symbols = & $readelf "--dyn-syms" $payload
if ($LASTEXITCODE -ne 0) { throw "controller-shadow coordinator symbol audit failed" }
foreach ($required in @(
    "a9tas_controller_tick_wrapper_v1",
    "a9tas_controller_shadow_coordinator_arm_v1",
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
        throw "Expected exactly one payload export: $required"
    }
}

$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolHash = (Get-FileHash -LiteralPath $protocol -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "CONTROLLER_SHADOW_COORDINATOR_BUILD passed=1 build_only=1 guest_code_patches=0 installer=0 deployed=0 device_access=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "source_sha256=$sourceHash"
Write-Output "protocol_sha256=$protocolHash"
Write-Output "disassembly_sha256=$disassemblyHash"
