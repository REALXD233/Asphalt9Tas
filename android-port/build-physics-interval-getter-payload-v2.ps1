param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_physics_interval_getter_v2.cpp"
$header = Join-Path $root "src\physics_interval_getter_payload_v2.h"
$policy = Join-Path $root "tools\test_physics_interval_getter_payload_v2_policy.py"
$abiVerifier = Join-Path $root "tools\verify_physics_interval_getter_payload_v2_abi.py"
$outDir = Join-Path $root "build\physics-interval-getter-payload-v2"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android24-clang++.cmd"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$nm = Join-Path $toolBin "llvm-nm.exe"
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "ARM64 compiler missing" }
foreach ($tool in @($objdump, $nm)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Tool missing: $tool" }
}
$payload = Join-Path $outDir "liba9tas_physics_interval_getter_v2_passive.so"
$selftest = Join-Path $outDir "a9tas_physics_interval_getter_payload_v2_selftest"
$disassembly = Join-Path $outDir "physics_interval_getter_payload_v2_selftest.disasm.txt"
$common = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti",
            "-Wall", "-Wextra", "-Werror")
& $compiler $source @common "-fPIC" "-shared" "-static-libstdc++" `
    "-Wl,--no-undefined" "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "passive payload build failed" }
& $compiler $source @common "-static-libstdc++" `
    "-DA9TAS_PHYSICS_INTERVAL_GETTER_PAYLOAD_SELFTEST=1" `
    "-Wl,--no-undefined" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "payload selftest build failed" }
python -B $policy $header $source
if ($LASTEXITCODE -ne 0) { throw "payload policy failed" }
& $objdump "-d" "--demangle" $selftest | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "payload disassembly failed" }
python -B $abiVerifier $disassembly
if ($LASTEXITCODE -ne 0) { throw "payload ABI verification failed" }
$forbidden = & $nm "-u" $payload | Select-String -Pattern `
    'ptrace|pwrite|process_vm_writev|mprotect'
if ($forbidden) { throw "passive payload imports forbidden runtime mutation symbol" }
foreach ($artifact in @($payload, $selftest, $disassembly)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower()
    Write-Output "$([IO.Path]::GetFileName($artifact)) sha256=$hash"
}
Write-Output "PHYSICS_INTERVAL_GETTER_PAYLOAD_V2_BUILD passed=1 passive=1 installer=absent deployed=0 game_access=0 game_writes=0"
