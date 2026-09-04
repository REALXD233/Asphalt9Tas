param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_natural_action_callback_lifecycle_v1.cpp"
$audit = Join-Path $root "tools\audit_natural_action_replay_payload_v1.py"
$outDir = Join-Path $root "build\natural-action-replay-payload-v1"
$payload = Join-Path $outDir "liba9tas_natural_action_replay_v1_review_only.so"
$disassembly = Join-Path $outDir "liba9tas_natural_action_replay_v1_review_only.disasm.txt"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($path in @($source, $audit, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing natural-action replay payload input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" "-shared" "-fPIC" `
    "-static-libstdc++" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--no-undefined" `
    "-Wl,--build-id=sha1" "-DA9TAS_NAL_ACTION_EXECUTE=1" `
    "-DA9TAS_NAL_REPLAY_EXECUTE=1" "-I$(Join-Path $root 'src')" `
    "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "natural-action replay payload build failed" }
& $objdump "-d" "--demangle" $payload | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "natural-action replay payload disassembly failed" }
python -B $audit $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "natural-action replay payload audit failed" }
$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_REPLAY_PAYLOAD_BUILD passed=1 counts_0_1_2=1 persistent=1 direct_nitro_state_writes=0 deployed=0 device_access=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "disassembly_sha256=$disassemblyHash"

