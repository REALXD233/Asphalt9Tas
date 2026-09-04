param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_natural_action_callback_lifecycle_v1.cpp"
$hostSelftestSource = Join-Path $root "src\natural_action_host_gate_selftest_v1.cpp"
$policy = Join-Path $root "tools\test_natural_action_callback_lifecycle_policy_v1.py"
$model = Join-Path $root "tools\test_natural_action_callback_lifecycle_v1.py"
$outDir = Join-Path $root "build\natural-action-callback-lifecycle-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$arm64 = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x64 = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($arm64, $x64, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$common = @("-O2", "-std=c++20", "-Wall", "-Wextra", "-Werror",
            "-I$(Join-Path $root 'src')")
$payload = Join-Path $outDir "liba9tas_natural_action_callback_lifecycle_v1_build_only.so"
$hostSelftest = Join-Path $outDir "a9tas_natural_action_lifecycle_host_selftest_v1"
$disassembly = Join-Path $outDir "liba9tas_natural_action_callback_lifecycle_v1_build_only.disasm.txt"

& $arm64 $source @common "-shared" "-fPIC" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wl,--no-undefined" `
    "-Wl,--build-id=sha1" "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "persistent natural action lifecycle payload build failed" }
& $x64 $hostSelftestSource @common "-static" "-fPIE" "-o" $hostSelftest
if ($LASTEXITCODE -ne 0) { throw "natural action lifecycle host selftest build failed" }
& $objdump "-d" "--demangle" $payload | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "lifecycle payload disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $model
if ($LASTEXITCODE -ne 0) { throw "natural action lifecycle model failed" }
& $python.Source $policy $payload $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "natural action lifecycle policy failed" }

$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$hostHash = (Get-FileHash -LiteralPath $hostSelftest -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_CALLBACK_LIFECYCLE_V1_BUILD passed=1 persistent=1 execute_compiled=0 deployed=0 device_access=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "host_selftest_sha256=$hostHash"
Write-Output "disassembly_sha256=$disasmHash"
