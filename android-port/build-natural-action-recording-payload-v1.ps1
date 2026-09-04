param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_natural_action_recording_v1.cpp"
$protocol = Join-Path $root "src\natural_action_recording_protocol_v1.h"
$policy = Join-Path $root "tools\test_natural_action_recording_payload_policy_v1.py"
$outDir = Join-Path $root "build\natural-action-recording-payload-v1"
$payload = Join-Path $outDir "liba9tas_natural_action_recording_v1_review_only.so"
$disassembly = Join-Path $outDir "natural_action_recording_payload_v1.disasm.txt"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"

foreach ($path in @($source, $protocol, $policy, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing natural-action recording payload input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-fPIC" "-fno-exceptions" `
    "-fno-rtti" "-Wall" "-Wextra" "-Werror" "-shared" `
    "-Wl,--build-id=sha1" "-Wl,--no-undefined" "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "Natural-action recording payload build failed" }
& $objdump "-d" "--demangle" $payload | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Natural-action recording disassembly failed" }
python -B $policy $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Natural-action recording payload policy failed" }
$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
$buildIdLine = (& $readelf -n $payload | Select-String "Build ID:").Line
$buildId = ($buildIdLine -replace '^.*Build ID:\s*', '').Trim()
Write-Output "NATURAL_ACTION_RECORDING_PAYLOAD_BUILD passed=1 preload_only=1 hooks_installed=0 device_access=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "build_id=$buildId"
Write-Output "disassembly_sha256=$disassemblyHash"

