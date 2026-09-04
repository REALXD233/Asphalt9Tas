param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\barrel_rbx_two_store_transaction_selftest_v1.cpp"
$semantic = Join-Path $root "src\barrel_stabilization_replay_core_v1.cpp"
$policy = Join-Path $root "tools\test_barrel_rbx_two_store_transaction_policy_v1.py"
$outDir = Join-Path $root "build\barrel-rbx-two-store-transaction-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($tool in @($compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$output = Join-Path $outDir "barrel_rbx_two_store_transaction_v1_selftest"
& $compiler $source $semantic "-I$(Join-Path $root 'src')" `
    "-O1" "-std=c++20" "-static-libstdc++" `
    "-DA9TAS_BARREL_STABILIZATION_REPLAY_NO_MAIN=1" `
    "-fno-exceptions" "-fno-rtti" "-fno-inline" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--no-undefined" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "BarrelRBX two-store build failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $output $readelf
if ($LASTEXITCODE -ne 0) { throw "BarrelRBX two-store policy failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "BARREL_RBX_TWO_STORE_BUILD passed=1 runtime=disabled deployed=0 device_access=0 game_writes=0"
Write-Output "selftest_sha256=$hash"
