param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$out = Join-Path $root "build\authoritative-natural-handoff-v1"
$policy = Join-Path $root "tools\test_authoritative_natural_handoff_cpp_policy_v1.py"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$bin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$cc = Join-Path $bin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $bin "llvm-readelf.exe"
$objdump = Join-Path $bin "llvm-objdump.exe"
$sources = @(
    (Join-Path $src "authoritative_natural_handoff_v1.cpp"),
    (Join-Path $src "authoritative_adapter_bridge_v1.cpp"),
    (Join-Path $src "authoritative_steering_transport_v1.cpp"),
    (Join-Path $src "authoritative_replay_session_v1.cpp"),
    (Join-Path $src "authoritative_tick_state_machine_v1.cpp")
)
foreach ($item in @($cc, $readelf, $objdump, $policy) + $sources) {
    if (-not (Test-Path -LiteralPath $item)) { throw "missing handoff input: $item" }
}
$defs = @(
    "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_STEERING_TRANSPORT_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_NO_MAIN=1"
)
$flags = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror")
$passive = Join-Path $out "authoritative_natural_handoff_v1_build_only"
$selftest = Join-Path $out "authoritative_natural_handoff_v1_selftest"
$object = Join-Path $out "authoritative_natural_handoff_v1.o"
$disasm = Join-Path $out "authoritative_natural_handoff_v1.disasm.txt"
& $cc @flags "-static-libstdc++" @defs @sources "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "handoff passive build failed" }
& $cc @flags "-static-libstdc++" @defs "-DA9TAS_AUTHORITATIVE_NATURAL_HANDOFF_SELFTEST=1" @sources "-Wl,--no-undefined" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "handoff selftest build failed" }
& $cc @flags @defs "-DA9TAS_AUTHORITATIVE_NATURAL_HANDOFF_SELFTEST=1" "-DA9TAS_AUTHORITATIVE_NATURAL_HANDOFF_NO_MAIN=1" "-c" $sources[0] "-o" $object
if ($LASTEXITCODE -ne 0) { throw "handoff review object failed" }
& $objdump "-d" "--demangle" $object | Set-Content -LiteralPath $disasm
if ($LASTEXITCODE -ne 0) { throw "handoff disassembly failed" }
python $policy $passive $selftest $object $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "handoff policy failed" }
foreach ($artifact in @($passive, $selftest, $object, $disasm)) {
    Write-Output "$([IO.Path]::GetFileName($artifact))_sha256=$((Get-FileHash $artifact -Algorithm SHA256).Hash.ToLower())"
}
Write-Output "AUTHORITATIVE_NATURAL_HANDOFF_BUILD passed=1 runtime=disabled deployed=0 device_access=0 game_writes=0"

