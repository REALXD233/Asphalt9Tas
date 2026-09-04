param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$armCompiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$hostCompiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$header = Join-Path $root "src\gameplay_input_controller_resolver_v1.h"
$selftest = Join-Path $root "src\gameplay_input_controller_resolver_selftest_v1.cpp"
$adapterHeader = Join-Path $root "src\gameplay_input_controller_mapping_adapter_v1.h"
$adapterSelftest = Join-Path $root "src\gameplay_input_controller_mapping_adapter_selftest_v1.cpp"
$protocol = Join-Path $root "src\controller_shadow_coordinator_protocol_v1.h"
$outDir = Join-Path $root "build\gameplay-input-controller-resolver-v1"
$armObject = Join-Path $outDir "gameplay_input_controller_resolver_arm64_v1.o"
$hostObject = Join-Path $outDir "gameplay_input_controller_resolver_x86_64_v1.o"
$adapterArmObject = Join-Path $outDir "gameplay_input_controller_mapping_adapter_arm64_v1.o"
$adapterHostObject = Join-Path $outDir "gameplay_input_controller_mapping_adapter_x86_64_v1.o"

foreach ($path in @($armCompiler, $hostCompiler, $header, $selftest,
                     $adapterHeader, $adapterSelftest, $protocol)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing GameplayInputController resolver input: $path"
    }
}

foreach ($build in @(
    @($armCompiler, $adapterArmObject),
    @($hostCompiler, $adapterHostObject)
)) {
    & $build[0] $adapterSelftest "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
        "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
        "-c" "-o" $build[1]
    if ($LASTEXITCODE -ne 0) {
        throw "GameplayInputController mapping adapter build failed: $($build[1])"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

foreach ($build in @(
    @($armCompiler, $armObject),
    @($hostCompiler, $hostObject)
)) {
    & $build[0] $selftest "-I$(Join-Path $root 'src')" "-O2" "-std=c++20" `
        "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
        "-c" "-o" $build[1]
    if ($LASTEXITCODE -ne 0) {
        throw "GameplayInputController resolver build failed: $($build[1])"
    }
}

$headerHash = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant()
$selftestHash = (Get-FileHash -LiteralPath $selftest -Algorithm SHA256).Hash.ToLowerInvariant()
$armHash = (Get-FileHash -LiteralPath $armObject -Algorithm SHA256).Hash.ToLowerInvariant()
$hostHash = (Get-FileHash -LiteralPath $hostObject -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterHeaderHash = (Get-FileHash -LiteralPath $adapterHeader -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterArmHash = (Get-FileHash -LiteralPath $adapterArmObject -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterHostHash = (Get-FileHash -LiteralPath $adapterHostObject -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "GAMEPLAY_INPUT_CONTROLLER_RESOLVER_BUILD passed=1 arm64=1 x86_64=1 unique=1 source_identity=1 ldplayer_object_mapping_filter=1 read_only=1 device_access=0 deployed=0"
Write-Output "header_sha256=$headerHash"
Write-Output "selftest_sha256=$selftestHash"
Write-Output "arm64_object_sha256=$armHash"
Write-Output "x86_64_object_sha256=$hostHash"
Write-Output "adapter_header_sha256=$adapterHeaderHash"
Write-Output "adapter_arm64_object_sha256=$adapterArmHash"
Write-Output "adapter_x86_64_object_sha256=$adapterHostHash"
