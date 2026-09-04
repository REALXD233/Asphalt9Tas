param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build-camera-tool-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "Camera Tool payload prerequisite failed" }
$outDir = Join-Path $root "build\camera-tool-transaction-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$source = Join-Path $root "src\camera_tool_transaction_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_camera_tool_v1_build.cpp"
$policy = Join-Path $root "tools\test_camera_tool_transaction_policy_v1.py"
$payload = Join-Path $root "build\camera-tool-v1\liba9tas_camera_tool_v1_build_only.so"
$output = Join-Path $outDir "a9tas_camera_tool_transaction_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_camera_tool_v1.so"
foreach ($path in @($compiler,$readelf,$source,$bootstrapSource,$policy,$payload)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Camera Tool transaction input: $path"
    }
}
New-Item -Path $outDir -ItemType Directory -Force | Out-Null
$payloadHash = (Get-FileHash $payload -Algorithm SHA256).Hash.ToLowerInvariant()
if ($payloadHash -ne "7b2fad97e599943ff6c41d67611caeeb82d517fae5545c1e3f228371b3bdf5c8") {
    throw "Camera Tool payload pin mismatch"
}
$symbols = (& $readelf --dyn-syms --wide $payload) -join "`n"
foreach ($proof in @(
    '0000000000001a50\s+\d+\s+FUNC\s+GLOBAL.*a9tas_camera_tool_callback_v1',
    '0000000000005000\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_tool_control_storage_v1',
    '0000000000005140\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_tool_evidence_storage_v1'
)) {
    if ($symbols -notmatch $proof) { throw "Camera Tool payload RVA proof failed: $proof" }
}
& $compiler $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -Wall -Wextra -Werror "-Wl,--no-undefined" -o $output
if ($LASTEXITCODE -ne 0) { throw "Camera Tool transaction build failed" }
& $compiler $bootstrapSource "-I$(Join-Path $root 'src')" -shared -fPIC -O2 `
    -std=c++20 -static-libstdc++ -Wall -Wextra -Werror `
    "-Wl,--build-id=sha1" -llog -ldl -o $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Camera Tool preload bootstrap build failed" }
$header = (& $readelf -h $output) -join "`n"
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
$bootstrapSymbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "Camera Tool controller/bootstrap ELF mismatch"
}
python -B $policy
if ($LASTEXITCODE -ne 0) { throw "Camera Tool transaction policy failed" }
Write-Output "CAMERA_TOOL_TRANSACTION_BUILD passed=1 live_candidate=local_only deployed=0 device_access=0"
Write-Output "controller_sha256=$((Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "bootstrap_sha256=$((Get-FileHash $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant())"
