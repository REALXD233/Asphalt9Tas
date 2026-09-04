param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\vehicle_state_probe_v1.cpp"
$outputDir = Join-Path $projectRoot "build\staging"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
$output = Join-Path $outputDir "a9tas_vehicle_state_probe_v1"
$resolverSource = Join-Path $projectRoot "src\vehicle_state_resolver_check_v1.cpp"
$resolverOutput = Join-Path $outputDir "a9tas_vehicle_state_resolver_check_v1"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "NDK compiler not found: $compiler"
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fPIE" "-pie" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" $source "-o" $output
if ($LASTEXITCODE -ne 0) { throw "vehicle-state probe build failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Built $output"
Write-Output "SHA256 $hash"

& $compiler "-O2" "-std=c++20" "-static-libstdc++" "-fPIE" "-pie" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" $resolverSource "-o" $resolverOutput
if ($LASTEXITCODE -ne 0) { throw "vehicle-state resolver check build failed" }
$resolverHash = (Get-FileHash -LiteralPath $resolverOutput -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Built $resolverOutput"
Write-Output "SHA256 $resolverHash"
