param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $root "build-camera-tool-transaction-v1.ps1") -NdkRoot $NdkRoot
if ($LASTEXITCODE -ne 0) { throw "Camera Tool transaction prerequisite failed" }
$outDir = Join-Path $root "build\camera-tool-stream-v1"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$source = Join-Path $root "src\camera_tool_stream_controller_v1.cpp"
$policy = Join-Path $root "tools\test_camera_tool_stream_policy_v1.py"
$output = Join-Path $outDir "a9tas_camera_tool_stream_v1"
foreach ($path in @($compiler,$readelf,$source,$policy)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing Camera Tool stream input: $path"
    }
}
New-Item -Path $outDir -ItemType Directory -Force | Out-Null
& $compiler $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 `
    -static-libstdc++ -fno-exceptions -fno-rtti -fno-stack-protector `
    -Wall -Wextra -Werror "-Wl,--no-undefined" -o $output
if ($LASTEXITCODE -ne 0) { throw "Camera Tool stream build failed" }
$header = (& $readelf -h $output) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64") {
    throw "Camera Tool stream ELF architecture mismatch"
}
python -B $policy
if ($LASTEXITCODE -ne 0) { throw "Camera Tool stream policy failed" }
Write-Output "CAMERA_TOOL_STREAM_BUILD passed=1 persistent=1 payload_only=1 device_access=0"
Write-Output "stream_sha256=$((Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant())"
