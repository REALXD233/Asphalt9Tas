param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$out = Join-Path $root "build\authoritative-steering-v1"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$candidateSource = Join-Path $src "hwbp_authoritative_steering_v1.cpp"
$sources = @(
    $candidateSource,
    (Join-Path $src "authoritative_natural_handoff_v1.cpp"),
    (Join-Path $src "authoritative_adapter_bridge_v1.cpp"),
    (Join-Path $src "authoritative_steering_transport_v1.cpp"),
    (Join-Path $src "authoritative_replay_session_v1.cpp"),
    (Join-Path $src "authoritative_tick_state_machine_v1.cpp")
)
$pins = @{
    "authoritative_natural_handoff_v1.cpp"="4d17be549b22f61b793efdd8f1a231a722997f259a707abd7e4496dfdbda7df0"
    "authoritative_natural_handoff_v1.h"="82a773939aec0ad561098f8f9d0444652e3b7becb0df1ce42b673e542d6c02d8"
    "authoritative_adapter_bridge_v1.cpp"="e8a3565fea51d7f88e78cb3ee8e9dbd098c957af38b8f34d58327fa3068fbabc"
    "authoritative_adapter_bridge_v1.h"="4ada67afc06d7bc795ef5f92e3a2afb0fe69cfa7b691507c7aac0eb1fa9439bc"
    "authoritative_steering_transport_v1.cpp"="7b696d64b4e9227b8e4aeda424077a885aded183ff5fe470aabb67c6f16b64e0"
    "authoritative_steering_transport_v1.h"="20eb8ae3dc0a7837b4377ebe32a67f5af139e0791d360eb51d5a2ea892d527ae"
    "authoritative_replay_session_v1.cpp"="d0f9c73dd50c7702def0b05dc8c2727c3d3d7820467e920184b7f0e41f11e12c"
    "authoritative_tick_state_machine_v1.cpp"="24e1fe0ea15a88b1293bdb247bd401e30e19f1ffc8e727b810a1e6997b1daab3"
    "unified_tick_executor_core_v1.cpp"="5b9f81122769371be6825ce8f8c10dc66f74d83860c43251fc6c0452ec7ea5ac"
    "hwbp_pipeline_order_observer_v1.cpp"="ca0d220f5196c1b349f1a3e5af36a5a240008c984d2de1fcc783daf49503db49"
    "hwbp_scheduler_observer_v1.cpp"="234b8b4977bad1dd4373126db8de4152604cbcda898307b493ddad4c0e1a885b"
    "vehicle_state_resolver_v1.h"="3b8cf3e88f0380344bfcd8f8caa384ccfb7eeb2dd738d0cdcb01f603af641860"
    "unified_tick_recording_v1.h"="0db4169b85970a0887067887074ecd0f1d4dd00f107786cea5044f1b60f366d0"
    "natural_preroll_anchor_v1.h"="dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75"
}
foreach ($entry in $pins.GetEnumerator()) {
    $path = Join-Path $src $entry.Key
    if (-not (Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() -ne $entry.Value) {
        throw "pinned dependency mismatch: $($entry.Key)"
    }
}
$tool = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$cc = Join-Path $tool "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $tool "llvm-readelf.exe"
$objdump = Join-Path $tool "llvm-objdump.exe"
$policy = Join-Path $root "tools\test_authoritative_steering_cpp_policy_v1.py"
foreach ($path in @($cc,$readelf,$objdump,$policy)+$sources) { if (-not (Test-Path $path)) { throw "missing: $path" } }
$defs = @("-DA9TAS_AUTHORITATIVE_NATURAL_HANDOFF_NO_MAIN=1","-DA9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_NO_MAIN=1","-DA9TAS_AUTHORITATIVE_STEERING_TRANSPORT_NO_MAIN=1","-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1","-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1")
$flags = @("-O2","-std=c++20","-fno-exceptions","-fno-rtti","-Wall","-Wextra","-Werror")
$candidate = Join-Path $out "a9tas_hwbp_authoritative_steering_v1"
$object = Join-Path $out "hwbp_authoritative_steering_v1.o"
$disasm = Join-Path $out "hwbp_authoritative_steering_v1.disasm.txt"
& $cc @flags "-static-libstdc++" @defs @sources "-Wl,--no-undefined" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "candidate build failed" }
& $cc @flags "-c" $candidateSource "-o" $object
if ($LASTEXITCODE -ne 0) { throw "review object build failed" }
& $objdump "-d" "--demangle" $object | Set-Content $disasm
python $policy $candidate $object $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "candidate policy failed" }
Write-Output "AUTHORITATIVE_STEERING_BUILD passed=1 runtime_candidate=1 deployed=0 frames=recording_declared max_frames=36000 max_pairs=72000"
foreach ($artifact in @($candidate,$object,$disasm)) { Write-Output "$([IO.Path]::GetFileName($artifact))_sha256=$((Get-FileHash $artifact -Algorithm SHA256).Hash.ToLower())" }
