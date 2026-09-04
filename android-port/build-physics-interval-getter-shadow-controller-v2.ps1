param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build-physics-interval-getter-payload-v2.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "physics interval payload prerequisite failed" }
$controller = Join-Path $root "src\physics_interval_getter_shadow_controller_v2.cpp"
$planner = Join-Path $root "src\physics_interval_getter_shadow_transaction_v2.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_physics_interval_getter_v2_build.cpp"
$helper = Join-Path $root "tools\run_physics_interval_getter_shadow_v2_preload.sh"
$policy = Join-Path $root "tools\test_physics_interval_getter_shadow_controller_v2_policy.py"
$parserTest = Join-Path $root "tools\test_parse_physics_interval_shadow_receipt_v2.py"
$payload = Join-Path $root "build\physics-interval-getter-payload-v2\liba9tas_physics_interval_getter_v2_passive.so"
$outDir = Join-Path $root "build\physics-interval-getter-shadow-controller-v2"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe"
foreach ($path in @($compiler, $readelf, $controller, $planner, $bootstrapSource,
                     $helper, $policy, $parserTest, $payload)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "missing shadow controller input: $path"
    }
}
$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLower()
if ($payloadHash -ne "f49b78eff51b606436f291c837ab50c297286ae3f43a10352dc2aa77ca8a62df") {
    throw "physics interval passive payload hash pin mismatch"
}
$payloadSymbols = (& $readelf --dyn-syms --wide $payload) -join "`n"
foreach ($proof in @(
    '00000000000051e0\s+8\s+OBJECT\s+GLOBAL.*a9tas_physics_interval_getter_wrapper_data_v2',
    '00000000000051e8\s+8\s+OBJECT\s+GLOBAL.*a9tas_physics_interval_getter_control_data_v2',
    '00000000000051f0\s+8\s+OBJECT\s+GLOBAL.*a9tas_physics_interval_getter_evidence_data_v2',
    '00000000000051f8\s+8\s+OBJECT\s+GLOBAL.*a9tas_physics_interval_getter_intervals_data_v2',
    '0000000000005200\s+8\s+OBJECT\s+GLOBAL.*a9tas_physics_interval_getter_events_data_v2',
    '0000000000005208\s+8\s+OBJECT\s+GLOBAL.*a9tas_physics_interval_getter_shadow_data_v2',
    '0000000000005210\s+8\s+OBJECT\s+GLOBAL.*a9tas_physics_interval_getter_continue_data_v2'
)) {
    if ($payloadSymbols -notmatch $proof) { throw "payload locator RVA proof failed: $proof" }
}
$output = Join-Path $outDir "a9tas_physics_interval_shadow_controller_v2"
$selftest = Join-Path $outDir "a9tas_physics_interval_shadow_controller_v2_selftest"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_physics_interval_getter_v2.so"
$common = @("-O2", "-std=c++20", "-fno-exceptions", "-fno-rtti",
            "-Wall", "-Wextra", "-Werror", "-static-libstdc++",
            "-Wl,--no-undefined")
& $compiler $controller $planner @common `
    "-DA9TAS_PHYSICS_INTERVAL_SHADOW_NO_MAIN=1" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "shadow controller build failed" }
& $compiler $controller $planner @common `
    "-DA9TAS_PHYSICS_INTERVAL_SHADOW_NO_MAIN=1" `
    "-DA9TAS_PHYSICS_INTERVAL_SHADOW_CONTROLLER_SELFTEST=1" `
    "-Wno-unused-function" "-Wno-unused-const-variable" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "shadow controller selftest build failed" }
& $compiler $bootstrapSource "-I$(Join-Path $root 'src')" `
    "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" `
    "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "physics interval preload bootstrap build failed" }
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
$bootstrapSymbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
if ($bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Type:\s+DYN" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "physics interval preload bootstrap ABI validation failed"
}
python -B $policy $controller
if ($LASTEXITCODE -ne 0) { throw "shadow controller policy failed" }
python -B $parserTest
if ($LASTEXITCODE -ne 0) { throw "shadow receipt parser selftest failed" }
foreach ($artifact in @($output, $selftest, $bootstrap)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLower()
    Write-Output "$([IO.Path]::GetFileName($artifact)) sha256=$hash"
}
Write-Output "PHYSICS_INTERVAL_SHADOW_CONTROLLER_V2_BUILD passed=1 payload_pinned=1 preload_ready=1 runtime=disabled deployed=0 game_writes=0"
