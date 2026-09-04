param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\payload_worker_boundary_probe_v1.cpp"
$outputDirectory = Join-Path $projectRoot "build\staging"
$output = Join-Path $outputDirectory "liba9tas_payload_worker_boundary_v1.so"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android24-clang++.cmd"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "AArch64 compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null

$arguments = @(
    "-shared", "-fPIC", "-O2", "-std=c++20",
    "-static-libstdc++", "-fvisibility=hidden",
    "-fno-exceptions", "-fno-rtti",
    "-Wall", "-Wextra", "-Werror",
    "-Wl,--build-id=sha1", "-Wl,--gc-sections",
    "-llog", "-ldl", $source, "-o", $output
)
& $compiler @arguments
if ($LASTEXITCODE -ne 0) {
    throw "worker-boundary probe build failed (exit=$LASTEXITCODE)"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
Write-Host "Built $output"
Write-Host "SHA256 $hash"
