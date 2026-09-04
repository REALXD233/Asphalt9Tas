param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_frame_callback_deferred_registration_v1.cpp"
$policy = Join-Path $root "tools\test_frame_callback_deferred_registration_policy_v1.py"
$outDir = Join-Path $root "build\frame-callback-deferred-registration-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$payload = Join-Path $outDir "liba9tas_frame_callback_deferred_registration_v1_build_only.so"
$disassembly = Join-Path $outDir "liba9tas_frame_callback_deferred_registration_v1_build_only.disasm.txt"

& $compiler $source "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--no-undefined" `
    "-Wl,--build-id=sha1" "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "FC-2 ARM64 build-only payload build failed" }

& $objdump "-d" "--demangle" $payload | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-2 payload disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-2 payload policy verification failed" }

$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "FC2_DEFERRED_REGISTRATION_BUILD passed=1 runtime=disabled deployed=0 device_access=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "disassembly_sha256=$disasmHash"

