param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $root "build\camera-tool-runtime-v2"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$hostCompiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$source = Join-Path $root "src\payload_camera_tool_runtime_v2.cpp"
$protocol = Join-Path $root "src\camera_tool_runtime_protocol_v2.h"
$core = Join-Path $root "src\camera_tool_runtime_core_v2.h"
$policy = Join-Path $root "tools\test_camera_tool_runtime_payload_v2_policy.py"
$transportPolicy = Join-Path $root "tools\test_camera_tool_runtime_transport_v2_policy.py"
$output = Join-Path $outDir "liba9tas_camera_tool_runtime_v2_build_only.so"
$transactionSource = Join-Path $root "src\camera_tool_runtime_transaction_controller_v2.cpp"
$streamSource = Join-Path $root "src\camera_tool_runtime_input_stream_v2.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_camera_tool_runtime_v2_build.cpp"
$transaction = Join-Path $outDir "a9tas_camera_tool_runtime_transaction_v2"
$stream = Join-Path $outDir "a9tas_camera_tool_runtime_input_stream_v2"
$armTransaction = Join-Path $outDir "a9tas_camera_tool_runtime_transaction_v2_arm64"
$armStream = Join-Path $outDir "a9tas_camera_tool_runtime_input_stream_v2_arm64"
$nativeLoaderSource = Join-Path $root "src\native_arm64_early_loader_v1.cpp"
$nativeTrapSource = Join-Path $root "src\native_arm64_immutable_trap_resolver_v1.cpp"
$nativeCallSource = Join-Path $root "src\native_arm64_remote_call_v1.cpp"
$nativeLoader = Join-Path $outDir "a9tas_camera_tool_native_arm64_loader_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_camera_tool_runtime_v2.so"
$disassembly = Join-Path $outDir "camera_tool_runtime_v2.disasm.txt"
foreach ($path in @($compiler,$hostCompiler,$readelf,$objdump,$source,$protocol,$core,$policy,$transportPolicy,$transactionSource,$streamSource,$bootstrapSource,$nativeLoaderSource,$nativeTrapSource,$nativeCallSource)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Camera Tool runtime v2 input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -Wall -Wextra -Werror -fPIC -shared "-Wl,--no-undefined" -o $output
if ($LASTEXITCODE -ne 0) { throw "Camera Tool runtime v2 payload build failed" }
& $objdump -d --demangle $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "Camera Tool runtime v2 disassembly failed" }
python -B $policy $output $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "Camera Tool runtime v2 policy failed" }
& $hostCompiler $transactionSource "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror `
    "-Wl,--no-undefined" "-Wl,--gc-sections" -o $transaction
if ($LASTEXITCODE -ne 0) { throw "Camera Tool runtime v2 transaction build failed" }
& $hostCompiler $streamSource "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -Wall -Wextra -Werror "-Wl,--no-undefined" -o $stream
if ($LASTEXITCODE -ne 0) { throw "Camera Tool runtime v2 input stream build failed" }
& $compiler $transactionSource "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror `
    "-Wl,--no-undefined" "-Wl,--gc-sections" `
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" -o $armTransaction
if ($LASTEXITCODE -ne 0) { throw "Camera Tool ARM64 transaction build failed" }
& $compiler $streamSource "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -Wall -Wextra -Werror "-Wl,--no-undefined" `
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" -o $armStream
if ($LASTEXITCODE -ne 0) { throw "Camera Tool ARM64 input stream build failed" }
$payloadHash = (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
$payloadDevicePath = '/data/local/tmp/liba9tas_camera_tool_runtime_v2_build_only.so'
& $compiler $nativeLoaderSource $nativeTrapSource $nativeCallSource `
    "-I$(Join-Path $root 'src')" `
    "-DA9TAS_NATIVE_ARM64_PAYLOAD_PATH=$payloadDevicePath" `
    "-DA9TAS_NATIVE_ARM64_PAYLOAD_BASENAME=liba9tas_camera_tool_runtime_v2_build_only.so" `
    "-DA9TAS_NATIVE_ARM64_PAYLOAD_SHA256=$payloadHash" `
    -O2 -std=c++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti `
    -fPIE -pie -static-libstdc++ "-Wl,-z,relro,-z,now" `
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" `
    -ldl -o $nativeLoader
if ($LASTEXITCODE -ne 0) { throw "Camera Tool native ARM64 loader build failed" }
& $hostCompiler $bootstrapSource "-I$(Join-Path $root 'src')" -shared -fPIC -O2 `
    -std=c++20 -static-libstdc++ -Wall -Wextra -Werror `
    "-Wl,--build-id=sha1" -llog -ldl -o $bootstrap
if ($LASTEXITCODE -ne 0) { throw "Camera Tool runtime v2 bootstrap build failed" }
python -B $transportPolicy $output $transaction $stream $readelf
if ($LASTEXITCODE -ne 0) { throw "Camera Tool runtime v2 transport policy failed" }
Write-Output "CAMERA_TOOL_RUNTIME_V2_BUILD passed=1 build_only=1 arm64=1 deployed=0 device_access=0"
Write-Output "payload_sha256=$((Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "source_sha256=$((Get-FileHash $source -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "protocol_sha256=$((Get-FileHash $protocol -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "core_sha256=$((Get-FileHash $core -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "disassembly_sha256=$((Get-FileHash $disassembly -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "transaction_sha256=$((Get-FileHash $transaction -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "stream_sha256=$((Get-FileHash $stream -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "arm_transaction_sha256=$((Get-FileHash $armTransaction -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "arm_stream_sha256=$((Get-FileHash $armStream -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "native_loader_sha256=$((Get-FileHash $nativeLoader -Algorithm SHA256).Hash.ToLowerInvariant())"
Write-Output "bootstrap_sha256=$((Get-FileHash $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant())"
