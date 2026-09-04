param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "build\fc2-runner-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& (Join-Path $root "build-frame-callback-deferred-registration-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "FC-2 payload prerequisite build failed" }
& (Join-Path $root "build-fc2-frame-callback-transaction-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "FC-2 transaction prerequisite build failed" }

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool unavailable: $tool" }
}

$controllerSource = Join-Path $root "src\fc2_frame_callback_transaction_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_frame_callback_fc2_v1_build.cpp"
$controller = Join-Path $outDir "a9tas_fc2_frame_callback_controller_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_frame_callback_fc2_v1.so"
$disassembly = Join-Path $outDir "a9tas_fc2_frame_callback_controller_v1.disasm.txt"

& $compiler $controllerSource "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-DA9TAS_FC2_LIVE_CANDIDATE=1" `
    "-Wl,--no-undefined" "-o" $controller
if ($LASTEXITCODE -ne 0) { throw "FC-2 guarded controller candidate build failed" }

& $compiler $bootstrapSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "FC-2 preload bootstrap build failed" }

$controllerHeader = (& $readelf -h $controller) -join "`n"
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
if ($controllerHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $controllerHeader -notmatch "Type:\s+DYN") {
    throw "FC-2 guarded controller candidate ELF mismatch"
}
if ($bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Type:\s+DYN") {
    throw "FC-2 preload bootstrap ELF mismatch"
}
$bootstrapSymbols = (& $readelf --dyn-syms $bootstrap) -join "`n"
if ($bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "FC-2 preload bootstrap status ABI missing"
}
& $objdump "-d" "--demangle" $controller | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-2 guarded controller disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source (Join-Path $root "tools\validate_fc2_report_v1.py") --selftest
if ($LASTEXITCODE -ne 0) { throw "FC-2 report validator selftest failed" }
$runnerPolicy = Join-Path $root "tools\test_run_fc2_frame_callback_policy_v1.py"
if (-not (Test-Path -LiteralPath $runnerPolicy -PathType Leaf)) {
    throw "FC-2 runner policy is missing"
}
& $python.Source $runnerPolicy
if ($LASTEXITCODE -ne 0) { throw "FC-2 runner policy failed" }
& $python.Source (Join-Path $root "tools\audit_fc2_live_candidate_v1.py") `
    (Join-Path $root "build\frame-callback-deferred-registration-v1\liba9tas_frame_callback_deferred_registration_v1_build_only.so") `
    $controller $bootstrap $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-2 cross-artifact audit failed" }

& (Join-Path $root "run-fc2-frame-callback-v1.ps1") -Mode OfflineValidateOnly
if ($LASTEXITCODE -ne 0) { throw "FC-2 offline runner validation failed" }

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
Write-Output "FC2_RUNNER_BUILD passed=1 deployed=0 device_access=0 live_candidate=local_only"
Write-Output "controller_sha256=$(Get-Hash $controller)"
Write-Output "bootstrap_sha256=$(Get-Hash $bootstrap)"
Write-Output "disassembly_sha256=$(Get-Hash $disassembly)"
