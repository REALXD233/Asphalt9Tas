param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$source = Join-Path $root "src\controller_shadow_transaction_core_selftest_v1.cpp"
$header = Join-Path $root "src\controller_shadow_transaction_core_v1.h"
$protocol = Join-Path $root "src\controller_shadow_coordinator_protocol_v1.h"
$outDir = Join-Path $root "build\controller-shadow-transaction-core-v1"
$output = Join-Path $outDir "controller_shadow_transaction_core_selftest_v1.o"

foreach ($path in @($compiler, $source, $header, $protocol)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing controller-shadow transaction input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

& $compiler $source "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-c" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "controller-shadow transaction ARM64 build failed" }

$headerHash = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$protocolHash = (Get-FileHash -LiteralPath $protocol -Algorithm SHA256).Hash.ToLowerInvariant()
$selftestHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
$objectHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "CONTROLLER_SHADOW_TRANSACTION_CORE_BUILD passed=1 arm64=1 prepare_read_only=1 commit_vptr_writes=1 conditional_rollback=1 device_access=0 deployed=0"
Write-Output "header_sha256=$headerHash"
Write-Output "protocol_sha256=$protocolHash"
Write-Output "selftest_sha256=$selftestHash"
Write-Output "object_sha256=$objectHash"
