param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$adapterSource = Join-Path $root "src\authoritative_unified_adapter_v1.cpp"
$sessionSource = Join-Path $root "src\authoritative_replay_session_v1.cpp"
$tickSource = Join-Path $root "src\authoritative_tick_state_machine_v1.cpp"
$phaseSource = Join-Path $root "src\unified_tick_executor_core_v1.cpp"
$policy = Join-Path $root "tools\test_authoritative_unified_adapter_cpp_policy_v1.py"
$outDir = Join-Path $root "build\authoritative-unified-adapter-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$expectedPhase = "5b9f81122769371be6825ce8f8c10dc66f74d83860c43251fc6c0452ec7ea5ac"
$expectedSession = "d0f9c73dd50c7702def0b05dc8c2727c3d3d7820467e920184b7f0e41f11e12c"
$expectedTick = "24e1fe0ea15a88b1293bdb247bd401e30e19f1ffc8e727b810a1e6997b1daab3"
foreach ($binding in @(
    @($phaseSource, $expectedPhase, "proven phase core"),
    @($sessionSource, $expectedSession, "authoritative session core"),
    @($tickSource, $expectedTick, "authoritative tick core")
)) {
    $actual = (Get-FileHash -LiteralPath $binding[0] -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $binding[1]) {
        throw "$($binding[2]) source hash mismatch: $actual"
    }
}

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$passive = Join-Path $outDir "authoritative_unified_adapter_v1_build_only"
$adapterObject = Join-Path $outDir "authoritative_unified_adapter_v1_selftest.o"
$disassembly = Join-Path $outDir "authoritative_unified_adapter_v1_selftest.disasm.txt"

& $compiler $adapterSource $sessionSource $tickSource "-O2" "-std=c++20" `
    "-static-libstdc++" "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" `
    "-Werror" "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1" `
    "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1" "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "authoritative unified adapter passive build failed" }

& $compiler $adapterSource "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" `
    "-DA9TAS_AUTHORITATIVE_UNIFIED_ADAPTER_SELFTEST=1" `
    "-DA9TAS_AUTHORITATIVE_UNIFIED_ADAPTER_NO_MAIN=1" "-c" "-o" $adapterObject
if ($LASTEXITCODE -ne 0) { throw "authoritative unified adapter review-object build failed" }

& $objdump "-d" "--demangle" $adapterObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "authoritative unified adapter disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $adapterObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "authoritative unified adapter policy failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$objectHash = (Get-FileHash -LiteralPath $adapterObject -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "AUTH_UNIFIED_ADAPTER_BUILD passed=1 runtime=disabled deployed=0 device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "selftest_object_sha256=$objectHash"
Write-Output "disassembly_sha256=$disasmHash"

