param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\payload_nitro_rpc_v1.cpp"
$outputDir = Join-Path $projectRoot "build\staging"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android24-clang++.cmd"
$output = Join-Path $outputDir "liba9tas_payload_nitro_rpc_v1.so"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "NDK compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $compiler "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-fvisibility=hidden" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" `
    $source "-llog" "-ldl" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "nitro RPC v1 payload build failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Built $output"
Write-Output "SHA256 $hash"
