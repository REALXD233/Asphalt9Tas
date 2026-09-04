param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d",
    [string]$LegacyGameActionRpcSha256 = "34eab8d47a8f4ee68bf9eb4a0dfa9983c1e88dd349d4477f1355ab3b06b94a90"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $projectRoot "src"
$outputDir = Join-Path $projectRoot "build\producer-action-observe-v1"
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$arm64Compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readElf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($tool in @($arm64Compiler, $x64Compiler, $readElf)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "NDK tool not found: $tool" }
}

$payload = Join-Path $outputDir "liba9tas_producer_action_observe_v1.so"
& $arm64Compiler "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-fvisibility=hidden" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" `
    (Join-Path $sourceDir "payload_producer_action_observe_v1.cpp") `
    "-llog" "-ldl" "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "producer action observe payload build failed" }

$bootstrap = Join-Path $outputDir "liba9tas_bootstrap_producer_action_observe_v1.so"
& $x64Compiler "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" `
    (Join-Path $sourceDir "bootstrap_producer_action_observe_v1_build.cpp") `
    "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "producer action observe bootstrap build failed" }

$payloadHeader = (& $readElf -h $payload | Out-String)
$bootstrapHeader = (& $readElf -h $bootstrap | Out-String)
if ($payloadHeader -notmatch "AArch64") { throw "payload is not AArch64" }
if ($bootstrapHeader -notmatch "Advanced Micro Devices X86-64") {
    throw "bootstrap is not x86-64"
}
$payloadSymbols = (& $readElf --dyn-syms $payload | Out-String)
$bootstrapSymbols = (& $readElf --dyn-syms $bootstrap | Out-String)
if ($payloadSymbols -notmatch "a9tas_producer_action_observe_v1") {
    throw "producer action observe export missing"
}
if ($bootstrapSymbols -notmatch "a9tas_bootstrap_same_thread_probe_trampoline") {
    throw "bootstrap trampoline getter missing"
}
foreach ($forbiddenImport in @(
    "pthread_create", "socket", "bind", "listen", "accept4", "chmod", "unlink"
)) {
    if ($payloadSymbols -match "UND\s+$forbiddenImport(?:@|\s|$)") {
        throw "producer observe payload contains forbidden server import: $forbiddenImport"
    }
}

python (Join-Path $projectRoot "tools\test_producer_action_observe_policy_v1.py")
if ($LASTEXITCODE -ne 0) { throw "producer action observe policy tests failed" }

# The shared-source extraction must not alter the already reviewed isolated
# game-action RPC artifact. Rebuild it and demand exact binary parity.
& (Join-Path $projectRoot "build-game-action-rpc-v1.ps1") -NdkRoot $NdkRoot | Out-Host
if ($LASTEXITCODE -ne 0) { throw "legacy game-action RPC rebuild failed" }
$legacy = Join-Path $projectRoot "build\game-action-rpc-v1\liba9tas_game_action_rpc_v1.so"
$legacyHash = (Get-FileHash -LiteralPath $legacy -Algorithm SHA256).Hash.ToLowerInvariant()
if ($legacyHash -ne $LegacyGameActionRpcSha256) {
    throw "legacy game-action RPC binary parity failed"
}

function Get-Hash256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

Write-Output "PT_NB1_BUILD_ONLY passed=1 deployed=0 server_threads=0 server_imports=0 activations=0 game_calls=0"
Write-Output "payload=$payload"
Write-Output "payload_sha256=$(Get-Hash256 $payload)"
Write-Output "bootstrap=$bootstrap"
Write-Output "bootstrap_sha256=$(Get-Hash256 $bootstrap)"
Write-Output "legacy_game_action_rpc_sha256=$legacyHash parity=1"
