param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\hwbp_authoritative_neutral_observer_v1.cpp"
$sessionSource = Join-Path $root "src\authoritative_replay_session_v1.cpp"
$tickSource = Join-Path $root "src\authoritative_tick_state_machine_v1.cpp"
$pipelineSource = Join-Path $root "src\hwbp_pipeline_order_observer_v1.cpp"
$schedulerSource = Join-Path $root "src\hwbp_scheduler_observer_v1.cpp"
$adapterSource = Join-Path $root "src\authoritative_unified_adapter_v1.cpp"
$phaseSource = Join-Path $root "src\unified_tick_executor_core_v1.cpp"
$vehicleHeader = Join-Path $root "src\vehicle_state_resolver_v1.h"
$recordingHeader = Join-Path $root "src\unified_tick_recording_v1.h"
$policy = Join-Path $root "tools\test_authoritative_neutral_observer_cpp_policy_v1.py"
$outDir = Join-Path $root "build\authoritative-neutral-observer-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

foreach ($binding in @(
    @($schedulerSource, "234b8b4977bad1dd4373126db8de4152604cbcda898307b493ddad4c0e1a885b", "scheduler HWBP layer"),
    @($pipelineSource, "ca0d220f5196c1b349f1a3e5af36a5a240008c984d2de1fcc783daf49503db49", "pipeline HWBP layer"),
    @($adapterSource, "d2325e1bcd5ac50905783961465f1817afd388ed60958d2b645b83a4e8579a35", "authoritative adapter"),
    @($sessionSource, "d0f9c73dd50c7702def0b05dc8c2727c3d3d7820467e920184b7f0e41f11e12c", "authoritative session"),
    @($tickSource, "24e1fe0ea15a88b1293bdb247bd401e30e19f1ffc8e727b810a1e6997b1daab3", "authoritative tick core"),
    @($phaseSource, "5b9f81122769371be6825ce8f8c10dc66f74d83860c43251fc6c0452ec7ea5ac", "proven phase core"),
    @($vehicleHeader, "3b8cf3e88f0380344bfcd8f8caa384ccfb7eeb2dd738d0cdcb01f603af641860", "vehicle resolver"),
    @($recordingHeader, "0db4169b85970a0887067887074ecd0f1d4dd00f107786cea5044f1b60f366d0", "recording ABI")
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

$candidate = Join-Path $outDir "a9tas_hwbp_authoritative_neutral_observer_v1"
$reviewObject = Join-Path $outDir "hwbp_authoritative_neutral_observer_v1.o"
$disassembly = Join-Path $outDir "hwbp_authoritative_neutral_observer_v1.disasm.txt"

& $compiler $source $sessionSource $tickSource "-O2" "-std=c++20" `
    "-static-libstdc++" "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" `
    "-Werror" "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1" `
    "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1" "-Wl,--no-undefined" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "authoritative neutral observer build failed" }

& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-c" "-o" $reviewObject
if ($LASTEXITCODE -ne 0) { throw "authoritative neutral observer object build failed" }

& $objdump "-d" "--demangle" $reviewObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "authoritative neutral observer disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $candidate $reviewObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "authoritative neutral observer policy failed" }

$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLower()
$objectHash = (Get-FileHash -LiteralPath $reviewObject -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "AUTHORITATIVE_NEUTRAL_OBSERVER_BUILD passed=1 runtime_candidate=1 deployed=0 device_access=0 gameplay_writes=0"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "review_object_sha256=$objectHash"
Write-Output "disassembly_sha256=$disasmHash"
