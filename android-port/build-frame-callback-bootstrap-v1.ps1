param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_frame_callback_bootstrap_v1.cpp"
$policy = Join-Path $root "tools\test_frame_callback_bootstrap_policy_v1.py"
$outDir = Join-Path $root "build\frame-callback-bootstrap-v1"
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

$output = Join-Path $outDir "liba9tas_frame_callback_bootstrap_v1_build_only.so"
$disassembly = Join-Path $outDir "frame_callback_bootstrap_v1.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-fno-stack-protector" `
    "-Wall" "-Wextra" "-Werror" "-fPIC" "-shared" `
    "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "FC-0 ARM64 build failed" }

& $objdump "-d" "--demangle" $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-0 disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $output $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-0 policy verification failed" }

$disasmText = Get-Content -LiteralPath $disassembly -Raw
if ($disasmText -notmatch "<a9tas_fc0_frame_callback_passthrough_v1>:") {
    throw "FC-0 wrapper missing from disassembly"
}
if ($disasmText -notmatch "blr\s+x[0-9]+") {
    throw "FC-0 wrapper has no indirect original-callback call"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "FRAME_CALLBACK_BOOTSTRAP_FC0 passed=1 passive=1 arm_return=-100 device_access=0"
Write-Output "sha256=$hash"
Write-Output "disassembly_sha256=$disasmHash"
