param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bridge = Join-Path $root "src\authoritative_adapter_bridge_v1.cpp"
$transport = Join-Path $root "src\authoritative_steering_transport_v1.cpp"
$session = Join-Path $root "src\authoritative_replay_session_v1.cpp"
$tick = Join-Path $root "src\authoritative_tick_state_machine_v1.cpp"
$policy = Join-Path $root "tools\test_authoritative_adapter_bridge_cpp_policy_v1.py"
$outDir = Join-Path $root "build\authoritative-adapter-bridge-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($required in @($bridge, $transport, $session, $tick, $policy, $compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required bridge input unavailable: $required"
    }
}

$common = @(
    "-O2", "-std=c++20", "-static-libstdc++", "-fno-exceptions",
    "-fno-rtti", "-Wall", "-Wextra", "-Werror",
    "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_STEERING_TRANSPORT_NO_MAIN=1"
)
$objectCommon = @($common | Where-Object { $_ -ne "-static-libstdc++" })
$passive = Join-Path $outDir "authoritative_adapter_bridge_v1_build_only"
$selftest = Join-Path $outDir "authoritative_adapter_bridge_v1_selftest"
$reviewObject = Join-Path $outDir "authoritative_adapter_bridge_v1.o"
$disassembly = Join-Path $outDir "authoritative_adapter_bridge_v1.disasm.txt"

& $compiler @common $bridge $transport $session $tick "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "authoritative adapter bridge passive build failed" }

& $compiler @common "-DA9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_SELFTEST=1" `
    $bridge $transport $session $tick "-Wl,--no-undefined" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "authoritative adapter bridge selftest build failed" }

& $compiler @objectCommon "-DA9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_SELFTEST=1" `
    "-DA9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_NO_MAIN=1" "-c" $bridge "-o" $reviewObject
if ($LASTEXITCODE -ne 0) { throw "authoritative adapter bridge review object failed" }

& $objdump "-d" "--demangle" $reviewObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "authoritative adapter bridge disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $selftest $reviewObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "authoritative adapter bridge policy failed" }

foreach ($artifact in @($passive, $selftest, $reviewObject, $disassembly)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower()
    Write-Output "$([IO.Path]::GetFileName($artifact))_sha256=$hash"
}
Write-Output "AUTHORITATIVE_ADAPTER_BRIDGE_BUILD passed=1 runtime=disabled deployed=0 frames=344 pair_slots=688 device_access=0 game_writes=0"
