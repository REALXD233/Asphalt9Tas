param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $projectRoot "src"
$outputDir = Join-Path $projectRoot "build\same-thread-probe-v1"
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$arm64Compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readElf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($tool in @($arm64Compiler, $x64Compiler, $readElf)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "NDK tool not found: $tool" }
}

$payload = Join-Path $outputDir "liba9tas_same_thread_probe_v1.so"
& $arm64Compiler "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-fvisibility=hidden" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" `
    (Join-Path $sourceDir "payload_same_thread_probe_v1.cpp") "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "same-thread ARM64 probe payload build failed" }

$bootstrap = Join-Path $outputDir "liba9tas_bootstrap_same_thread_probe_v1.so"
$payloadDevicePath = '/data/local/tmp/liba9tas_same_thread_probe_v1.so'
& $x64Compiler "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" `
    (Join-Path $sourceDir "bootstrap_same_thread_probe_v1_build.cpp") `
    "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "same-thread x86_64 bootstrap build failed" }

$payloadHeader = (& $readElf -h $payload | Out-String)
$bootstrapHeader = (& $readElf -h $bootstrap | Out-String)
if ($payloadHeader -notmatch 'AArch64') { throw "payload ELF machine is not AArch64" }
if ($bootstrapHeader -notmatch 'Advanced Micro Devices X86-64') {
    throw "bootstrap ELF machine is not x86-64"
}
$payloadSymbols = (& $readElf --dyn-syms $payload | Out-String)
$bootstrapSymbols = (& $readElf --dyn-syms $bootstrap | Out-String)
if ($payloadSymbols -notmatch 'a9tas_same_thread_probe_v1') {
    throw "payload probe export missing"
}
if ($bootstrapSymbols -notmatch 'a9tas_bootstrap_same_thread_probe_trampoline') {
    throw "bootstrap trampoline getter export missing"
}
if ($bootstrapSymbols -notmatch 'a9tas_bootstrap_same_thread_probe_status') {
    throw "bootstrap probe status export missing"
}

$controller = Join-Path $outputDir "a9tas_same_thread_probe_controller_v1"
& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    (Join-Path $sourceDir "same_thread_probe_controller_v1.cpp") `
    "-ldl" "-o" $controller
if ($LASTEXITCODE -ne 0) { throw "same-thread x86_64 controller build failed" }
$controllerHeader = (& $readElf -h $controller | Out-String)
if ($controllerHeader -notmatch 'Advanced Micro Devices X86-64') {
    throw "controller ELF machine is not x86-64"
}

$frameController = Join-Path $outputDir "a9tas_same_thread_frame_probe_controller_v1"
& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    (Join-Path $sourceDir "same_thread_frame_probe_controller_v1.cpp") `
    "-ldl" "-o" $frameController
if ($LASTEXITCODE -ne 0) { throw "FrameThread probe controller build failed" }
$frameControllerHeader = (& $readElf -h $frameController | Out-String)
if ($frameControllerHeader -notmatch 'Advanced Micro Devices X86-64') {
    throw "FrameThread controller ELF machine is not x86-64"
}

$dualHostController = Join-Path $outputDir "a9tas_dual_thread_host_probe_controller_v1"
& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    (Join-Path $sourceDir "dual_thread_host_probe_controller_v1.cpp") `
    "-ldl" "-o" $dualHostController
if ($LASTEXITCODE -ne 0) { throw "dual-thread host controller build failed" }
$dualHostHeader = (& $readElf -h $dualHostController | Out-String)
if ($dualHostHeader -notmatch 'Advanced Micro Devices X86-64') {
    throw "dual-thread host controller ELF machine is not x86-64"
}

$dualHostFixture = Join-Path $outputDir "a9tas_dual_thread_host_probe_fixture_v1"
& $x64Compiler "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" `
    (Join-Path $sourceDir "dual_thread_host_probe_fixture_v1.cpp") `
    "-o" $dualHostFixture
if ($LASTEXITCODE -ne 0) { throw "dual-thread host fixture build failed" }
$dualHostFixtureHeader = (& $readElf -h $dualHostFixture | Out-String)
if ($dualHostFixtureHeader -notmatch 'Advanced Micro Devices X86-64') {
    throw "dual-thread host fixture ELF machine is not x86-64"
}

function Get-Hash256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

Write-Output "BUILD same_thread_probe_v1"
Write-Output "payload=$payload"
Write-Output "payload_sha256=$(Get-Hash256 $payload)"
Write-Output "bootstrap=$bootstrap"
Write-Output "bootstrap_sha256=$(Get-Hash256 $bootstrap)"
Write-Output "controller=$controller"
Write-Output "controller_sha256=$(Get-Hash256 $controller)"
Write-Output "frame_controller=$frameController"
Write-Output "frame_controller_sha256=$(Get-Hash256 $frameController)"
Write-Output "dual_host_controller=$dualHostController"
Write-Output "dual_host_controller_sha256=$(Get-Hash256 $dualHostController)"
Write-Output "dual_host_fixture=$dualHostFixture"
Write-Output "dual_host_fixture_sha256=$(Get-Hash256 $dualHostFixture)"
Write-Output "device_payload_path=$payloadDevicePath"
