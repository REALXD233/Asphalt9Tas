param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build-camera-raceview-same-state-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "same-state payload prerequisite failed" }
$outDir = Join-Path $root "build\camera-raceview-same-state-transaction-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$source = Join-Path $root "src\camera_raceview_same_state_transaction_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_camera_raceview_same_state_v1_build.cpp"
$policy = Join-Path $root "tools\test_camera_raceview_same_state_transaction_policy_v1.py"
$payload = Join-Path $root "build\camera-raceview-same-state-v1\liba9tas_camera_raceview_same_state_v1_build_only.so"
$output = Join-Path $outDir "a9tas_camera_raceview_same_state_transaction_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_camera_raceview_same_state_v1.so"
foreach ($path in @($compiler,$readelf,$source,$bootstrapSource,$policy,$payload)) {
  if (-not (Test-Path $path -PathType Leaf)) { throw "Missing same-state transaction input: $path" }
}
New-Item $outDir -ItemType Directory -Force | Out-Null
$hash=(Get-FileHash $payload -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -ne "58e888e4fb606fe3ec1b497c97a2a64c0b4dfc7dbd647ad0c01fa0558efd6ddf") { throw "same-state payload pin mismatch" }
$symbols=(& $readelf --dyn-syms --wide $payload)-join "`n"
foreach($proof in @(
 '0000000000001ab0\s+\d+\s+FUNC\s+GLOBAL.*a9tas_camera_raceview_same_state_callback_v1',
 '0000000000004dc0\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_raceview_same_state_control_storage_v1',
 '0000000000004e80\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_raceview_same_state_evidence_storage_v1',
 '0000000000004e88\s+8\s+OBJECT\s+GLOBAL.*a9tas_camera_raceview_same_state_frames_storage_v1')) {
  if($symbols -notmatch $proof){throw "same-state payload RVA proof failed: $proof"}
}
& $compiler $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 -static-libstdc++ `
  -fno-exceptions -fno-rtti -fno-stack-protector -Wall -Wextra -Werror `
  "-Wl,--no-undefined" -o $output
if($LASTEXITCODE -ne 0){throw "same-state transaction build failed"}
& $compiler $bootstrapSource "-I$(Join-Path $root 'src')" -shared -fPIC -O2 `
  -std=c++20 -static-libstdc++ -Wall -Wextra -Werror `
  "-Wl,--build-id=sha1" -llog -ldl -o $bootstrap
if($LASTEXITCODE -ne 0){throw "same-state preload bootstrap build failed"}
$header=(& $readelf -h $output)-join "`n"
$bootstrapHeader=(& $readelf -h $bootstrap)-join "`n"
$bootstrapSymbols=(& $readelf --dyn-syms --wide $bootstrap)-join "`n"
if($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
   $bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
   $bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
   $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {throw "same-state controller/bootstrap ELF mismatch"}
python -B $policy
if($LASTEXITCODE -ne 0){throw "same-state transaction policy failed"}
Write-Output "CAMERA_RACEVIEW_SAME_STATE_TRANSACTION_BUILD passed=1 live_candidate=local_only deployed=0 device_access=0"
Write-Output "controller_sha256=$((Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "bootstrap_sha256=$((Get-FileHash $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant())"
