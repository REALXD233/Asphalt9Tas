param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_nitro_prephysics_hook_v1.cpp"
$outDir = Join-Path $root "build\nitro-prephysics-hook-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
if (-not (Test-Path -LiteralPath $compiler) -or
    -not (Test-Path -LiteralPath $readelf)) {
    throw "Required ARM64 NDK tools are unavailable"
}

$output = Join-Path $outDir "liba9tas_nitro_prephysics_hook_v1_passive.so"
& $compiler $source "-O2" "-std=c++20" "-Wall" "-Wextra" "-Werror" `
    "-fPIC" "-shared" "-Wl,--no-undefined" "-I$(Join-Path $root 'src')" `
    "-llog" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "Passive ARM64 payload build failed" }

$header = (& $readelf -h $output) -join "`n"
if ($header -notmatch "Machine:\s+AArch64") {
    throw "Passive payload is not ARM64"
}
$symbols = (& $readelf --dyn-syms --wide $output) -join "`n"
foreach ($name in @(
    "a9tas_nitro_prephysics_v1_protocol",
    "a9tas_nitro_prephysics_v1_status",
    "a9tas_nitro_prephysics_v1_mailbox",
    "a9tas_nitro_prephysics_v1_arm")) {
    if ($symbols -notmatch [regex]::Escape($name)) {
        throw "Missing required passive export: $name"
    }
}
if ($symbols -match "OwnerSubmitStageBodyV1|PhysicsExecuteClaimBodyV1|PhysicsExecuteCompleteBodyV1") {
    throw "Hidden hook-body compile checks leaked into the dynamic export table"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
Write-Output "NITRO_PREPHYSICS_HOOK_V1_BUILD passed=1 passive=1 arm_return=-100 hooks=0"
Write-Output "sha256=$hash"
