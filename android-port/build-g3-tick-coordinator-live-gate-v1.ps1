param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$portRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$payloadBuild = Join-Path $portRoot "build-g3-multi-hook-runtime-v1.ps1"
$payloadSource = Join-Path $portRoot "src\payload_g3_multi_hook_runtime_v1.cpp"
$protocol = Join-Path $portRoot "src\g3_multi_hook_runtime_v1.h"
$bootstrapSource = Join-Path $portRoot "src\bootstrap_g3_tick_coordinator_v1_build.cpp"
$bootstrapCore = Join-Path $portRoot "src\bootstrap.cpp"
$controllerSource = Join-Path $portRoot "src\g3_tick_coordinator_controller_v1.cpp"
$g2ControllerSource = Join-Path $portRoot "src\g2_physics_interval_controller_v1.cpp"
$carrierSource = Join-Path $portRoot "src\habi1_early_carrier.cpp"
$carrierVerifier = Join-Path $portRoot "tools\verify_hook_abi_selftest_habi1_carrier.py"
$carrierVerifierSelftest = Join-Path $portRoot "tools\test_verify_hook_abi_selftest_habi1_carrier.py"
$carrierLibc = Join-Path $portRoot "evidence\libc_ldplayer9_20260816.so"
$baselineVerifier = Join-Path $portRoot "tools\verify_known_good_900_exact_interval_v2.py"
$baseline = Join-Path $portRoot "baselines\known_good_900_exact_interval_v2.json"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$nm = Join-Path $toolBin "llvm-nm.exe"
$outDir = Join-Path $portRoot "build\g3-tick-coordinator-live-gate-v1"
$payloadDir = Join-Path $portRoot "build\g3-multi-hook-runtime-v1"
$payload = Join-Path $payloadDir "liba9tas_g3_multi_hook_runtime_v1.so"
$devicePayload = "/data/local/tmp/liba9tas_g3_multi_hook_runtime_v1.so"
$deviceBootstrap = "/data/local/tmp/liba9tas_g3_tick_coordinator_bootstrap_v1.so"
$bootstrap = Join-Path $outDir "liba9tas_g3_tick_coordinator_bootstrap_v1.so"
$controller = Join-Path $outDir "a9tas_g3_tick_coordinator_controller_v1"
$policyReport = Join-Path $outDir "g3_early_carrier_v1.policy.json"

foreach ($path in @($payloadBuild,$payloadSource,$protocol,$bootstrapSource,
                     $bootstrapCore,$controllerSource,$g2ControllerSource,
                     $carrierSource,$carrierVerifier,$carrierVerifierSelftest,
                     $carrierLibc,$baselineVerifier,$baseline,$compiler,
                     $readelf,$nm)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing G3 live-Gate build input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $payloadBuild -NdkRoot $NdkRoot | Out-Host
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $payload)) {
    throw "G3 payload prerequisite build failed"
}

$sourceSha = (Get-FileHash -LiteralPath $payloadSource -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolSha = (Get-FileHash -LiteralPath $protocol -Algorithm SHA256).Hash.ToLowerInvariant()
$payloadSha = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$payloadBuildIdLine = (& $readelf "-n" $payload | Select-String "Build ID:" |
    Select-Object -Last 1).Line
$payloadBuildId = ($payloadBuildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($payloadBuildId -notmatch '^[0-9a-f]{40}$') {
    throw "G3 payload build-id missing"
}

$bootstrapFlags = @(
    $bootstrapSource, "-I$(Join-Path $portRoot 'src')", "-shared", "-fPIC",
    "-O2", "-std=c++20", "-static-libstdc++", "-fno-exceptions",
    "-fno-rtti", "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1",
    "-DA9TAS_G3_DEVICE_PAYLOAD_PATH=$devicePayload",
    "-DA9TAS_G3_PAYLOAD_SHA256=$payloadSha",
    "-DA9TAS_G3_PAYLOAD_BUILD_ID=$payloadBuildId",
    "-DA9TAS_G3_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-llog", "-ldl", "-o", $bootstrap
)
& $compiler @bootstrapFlags
if ($LASTEXITCODE -ne 0) { throw "G3 bootstrap build failed" }
$bootstrapSha = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapBuildIdLine = (& $readelf "-n" $bootstrap |
    Select-String "Build ID:" | Select-Object -Last 1).Line
$bootstrapBuildId = ($bootstrapBuildIdLine -replace '^.*Build ID:\s*','').Trim()
if ($bootstrapBuildId -notmatch '^[0-9a-f]{40}$') {
    throw "G3 bootstrap build-id missing"
}

$identityFlags = @(
    "-DA9TAS_HABI1_BOOTSTRAP_PATH=$deviceBootstrap",
    "-DA9TAS_HABI1_BOOTSTRAP_SHA256=$bootstrapSha",
    "-DA9TAS_HABI1_BOOTSTRAP_BUILD_ID=$bootstrapBuildId",
    "-DA9TAS_HABI1_PAYLOAD_PATH=$devicePayload",
    "-DA9TAS_HABI1_PAYLOAD_SHA256=$payloadSha",
    "-DA9TAS_HABI1_PAYLOAD_BUILD_ID=$payloadBuildId",
    "-DA9TAS_HABI1_PAYLOAD_SOURCE_SHA256=$sourceSha",
    "-DA9TAS_HABI1_PAYLOAD_HEADER_SHA256=$protocolSha"
)

$controllerFlags = @(
    $controllerSource, "-I$(Join-Path $portRoot 'src')", "-O2",
    "-std=c++20", "-static-libstdc++", "-fPIE", "-pie",
    "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
    "-Wl,--build-id=sha1"
) + $identityFlags + @("-o", $controller)
& $compiler @controllerFlags
if ($LASTEXITCODE -ne 0) { throw "G3 controller build failed" }

$carrier = Join-Path $outDir "a9tas_habi1_early_carrier_$($sourceSha.Substring(0,16))"
$carrierFlags = @(
    $carrierSource, "-I$(Join-Path $portRoot 'src')", "-O2", "-std=c++20",
    "-static-libstdc++", "-fPIE", "-pie", "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror", "-Wl,--build-id=sha1"
) + $identityFlags + @(
    "-DA9TAS_HABI1_LIBC_PATH=/system/lib64/libc.so",
    "-DA9TAS_HABI1_LIBC_SHA256=0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc",
    "-DA9TAS_HABI1_LIBC_BUILD_ID=f81ba81cf6ceedcdca60be54ad95b51f",
    "-DA9TAS_HABI1_LIBC_TRAP_RVA=0x5b0", "-o", $carrier
)
& $compiler @carrierFlags
if ($LASTEXITCODE -ne 0) { throw "G3 early carrier build failed" }

$carrierPolicyArgs = @(
    "--source", $carrierSource,
    "--controller-source", (Join-Path $portRoot "src\habi1_one_shot_controller.cpp"),
    "--elf", $carrier,
    "--bootstrap-path", $deviceBootstrap,
    "--bootstrap-sha256", $bootstrapSha,
    "--bootstrap-build-id", $bootstrapBuildId,
    "--payload-path", $devicePayload,
    "--payload-sha256", $payloadSha,
    "--payload-build-id", $payloadBuildId,
    "--payload-source-sha256", $sourceSha,
    "--libc-file", $carrierLibc,
    "--libc-device-path", "/system/lib64/libc.so",
    "--libc-sha256", "0e34ebf9663efde513ad4e7e08bc5c6da4d20a41770ce4990e68faa95679e0dc",
    "--libc-build-id", "f81ba81cf6ceedcdca60be54ad95b51f",
    "--libc-trap-rva", "0x5b0"
)
python -B $carrierVerifier @carrierPolicyArgs --report $policyReport
if ($LASTEXITCODE -ne 0) { throw "G3 early carrier policy failed" }
python -B $carrierVerifierSelftest --verifier $carrierVerifier @carrierPolicyArgs
if ($LASTEXITCODE -ne 0) { throw "G3 early carrier policy selftest failed" }
python -B $baselineVerifier $baseline
if ($LASTEXITCODE -ne 0) { throw "known-good v2 baseline drift" }

foreach ($artifact in @($bootstrap,$controller,$carrier)) {
    $identity = (& $readelf "-h" $artifact) -join "`n"
    if ($identity -notmatch 'Machine:\s+Advanced Micro Devices X86-64' -or
        $identity -notmatch 'Type:\s+DYN') {
        throw "G3 x86_64 artifact identity failed: $artifact"
    }
}
$controllerSymbols = (& $nm $controller) -join "`n"
if ($controllerSymbols -notmatch '(?m)\sT\smain\s*$' -or
    $controllerSymbols -notmatch '(?m)\sU\sptrace\s*$') {
    throw "G3 controller entry/ptrace transaction dependency missing"
}
$controllerSha = (Get-FileHash -LiteralPath $controller -Algorithm SHA256).Hash.ToLowerInvariant()
$carrierSha = (Get-FileHash -LiteralPath $carrier -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "G3_LIVE_GATE_BUILD passed=1 deployed=0 device_access=0"
Write-Output "payload_sha256=$payloadSha"
Write-Output "payload_build_id=$payloadBuildId"
Write-Output "bootstrap_sha256=$bootstrapSha"
Write-Output "bootstrap_build_id=$bootstrapBuildId"
Write-Output "controller_sha256=$controllerSha"
Write-Output "carrier_sha256=$carrierSha"
Write-Output "carrier=$carrier"
Write-Output "policy=$policyReport"
