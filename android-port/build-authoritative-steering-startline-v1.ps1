param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$out = Join-Path $root "build\authoritative-steering-startline-v1"
New-Item -ItemType Directory -Path $out -Force | Out-Null

$candidateSource = Join-Path $src "hwbp_authoritative_steering_v1.cpp"
$sources = @(
    $candidateSource,
    (Join-Path $src "startline_prearm_protocol_v1.cpp"),
    (Join-Path $src "authoritative_natural_handoff_v1.cpp"),
    (Join-Path $src "authoritative_adapter_bridge_v1.cpp"),
    (Join-Path $src "authoritative_steering_transport_v1.cpp"),
    (Join-Path $src "authoritative_replay_session_v1.cpp"),
    (Join-Path $src "authoritative_tick_state_machine_v1.cpp")
)
$tool = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$cc = Join-Path $tool "x86_64-linux-android24-clang++.cmd"
$objdump = Join-Path $tool "llvm-objdump.exe"
foreach ($path in @($cc, $objdump) + $sources) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing startline replay build input: $path"
    }
}

$defs = @(
    "-DA9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT=1",
    "-DA9TAS_STARTLINE_PREARM_PROTOCOL_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_NATURAL_HANDOFF_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_STEERING_TRANSPORT_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1"
)
$flags = @(
    "-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
    "-Wno-unused-variable"
)
$candidate = Join-Path $out "a9tas_hwbp_authoritative_steering_startline_v1"
$object = Join-Path $out "hwbp_authoritative_steering_startline_v1.o"
$disasm = Join-Path $out "hwbp_authoritative_steering_startline_v1.disasm.txt"

& $cc @flags "-static-libstdc++" @defs @sources "-Wl,--no-undefined" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "startline replay candidate build failed" }
& $cc @flags @defs "-c" $candidateSource "-o" $object
if ($LASTEXITCODE -ne 0) { throw "startline replay review object build failed" }
& $objdump "-d" "--demangle" $object | Set-Content -LiteralPath $disasm

Write-Host "AUTHORITATIVE_STEERING_STARTLINE_BUILD passed=1 deployed=0"
foreach ($artifact in @($candidate, $object, $disasm)) {
    Write-Host "$([IO.Path]::GetFileName($artifact))_sha256=$((Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower())"
}
