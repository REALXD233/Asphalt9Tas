param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build-camera-raceview-record-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "RaceView record payload prerequisite failed" }

$outDir = Join-Path $root "build\camera-raceview-record-transaction-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$controllerSource = Join-Path $root "src\camera_raceview_record_transaction_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_camera_raceview_record_v1_build.cpp"
$policy = Join-Path $root "tools\test_camera_raceview_record_transaction_policy_v1.py"
$payload = Join-Path $root "build\camera-raceview-record-v1\liba9tas_camera_raceview_record_v1_build_only.so"
$controller = Join-Path $outDir "a9tas_camera_raceview_record_transaction_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_camera_raceview_record_v1.so"
foreach ($path in @($compiler, $readelf, $controllerSource, $bootstrapSource, $policy, $payload)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing RaceView transaction input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
if ($payloadHash -ne "c253eecb2244756edb53d54ad5f546301522378b7c83e4bbaaa1d491342b2639") {
    throw "RaceView transaction payload pin mismatch"
}
$payloadSymbols = (& $readelf --dyn-syms --wide $payload) -join "`n"
foreach ($proof in @(
    '0000000000001bf0\s+\d+\s+FUNC\s+GLOBAL.*a9tas_camera_raceview_record_callback_v1',
    '0000000000004e00\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_raceview_record_control_storage_v1',
    '0000000000004ec0\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_raceview_record_evidence_storage_v1',
    '0000000000004ec8\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_raceview_record_frames_storage_v1'
)) {
    if ($payloadSymbols -notmatch $proof) { throw "RaceView payload RVA proof failed: $proof" }
}

& $compiler $controllerSource "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -Wall -Wextra -Werror "-Wl,--no-undefined" -o $controller
if ($LASTEXITCODE -ne 0) { throw "RaceView transaction controller build failed" }

& $compiler $bootstrapSource "-I$(Join-Path $root 'src')" -shared -fPIC `
    -O2 -std=c++20 -static-libstdc++ -Wall -Wextra -Werror `
    "-Wl,--build-id=sha1" -llog -ldl -o $bootstrap
if ($LASTEXITCODE -ne 0) { throw "RaceView preload bootstrap build failed" }

$controllerHeader = (& $readelf -h $controller) -join "`n"
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
$bootstrapSymbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
if ($controllerHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Type:\s+DYN" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "RaceView transaction ELF/ABI validation failed"
}

python -B $policy
if ($LASTEXITCODE -ne 0) { throw "RaceView transaction policy failed" }

Write-Output "CAMERA_RACEVIEW_RECORD_TRANSACTION_BUILD passed=1 live_candidate=local_only deployed=0 device_access=0"
Write-Output "controller_sha256=$((Get-FileHash -LiteralPath $controller -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "bootstrap_sha256=$((Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant())"
