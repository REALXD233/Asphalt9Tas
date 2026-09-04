param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_barrel_yaw_tail_v1.cpp"
$semantic = Join-Path $root "src\barrel_stabilization_replay_core_v1.cpp"
$policy = Join-Path $root "tools\test_barrel_yaw_tail_payload_policy_v1.py"
$outDir = Join-Path $root "build\barrel-yaw-tail-payload-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$output = Join-Path $outDir "liba9tas_barrel_yaw_tail_v1_build_only.so"
$disassembly = Join-Path $outDir "barrel_yaw_tail_v1.disasm.txt"
& $compiler $source $semantic "-I$(Join-Path $root 'src')" `
    "-O2" "-std=c++20" "-static-libstdc++" `
    "-DA9TAS_BARREL_STABILIZATION_REPLAY_NO_MAIN=1" `
    "-fno-exceptions" "-fno-rtti" "-fno-stack-protector" `
    "-Wall" "-Wextra" "-Werror" "-fPIC" "-shared" `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw tail ARM64 build failed" }

& $objdump "-d" "--demangle" $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw tail disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $output $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw tail policy verification failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolHash = (Get-FileHash -LiteralPath (Join-Path $root 'src\barrel_yaw_tail_payload_protocol_v1.h') -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "BARREL_YAW_TAIL_BUILD passed=1 build_only=1 source_bound=1 deployed=0 device_access=0"
Write-Output "payload_sha256=$hash"
Write-Output "source_sha256=$sourceHash"
Write-Output "protocol_sha256=$protocolHash"
Write-Output "disassembly_sha256=$disasmHash"
