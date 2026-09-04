param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$transactionSource = Join-Path $root "src\barrel_yaw_tail_host_transaction_selftest_v1.cpp"
$resolverSource = Join-Path $root "src\barrel_yaw_tail_payload_elf_resolver_selftest_v1.cpp"
$payload = Join-Path $root "build\barrel-yaw-tail-payload-v1\liba9tas_barrel_yaw_tail_v1_build_only.so"
$policy = Join-Path $root "tools\test_barrel_yaw_tail_host_transaction_policy_v1.py"
$outDir = Join-Path $root "build\barrel-yaw-tail-host-transaction-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($tool in @($compiler, $readelf, $payload)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required artifact unavailable: $tool"
    }
}

$transaction = Join-Path $outDir "barrel_yaw_tail_host_transaction_v1_selftest"
$resolver = Join-Path $outDir "barrel_yaw_tail_payload_elf_resolver_v1_selftest"
$common = @("-I$(Join-Path $root 'src')", "-O1", "-std=c++20",
            "-static-libstdc++", "-fno-exceptions", "-fno-rtti",
            "-fno-inline", "-Wall", "-Wextra", "-Werror",
            "-Wl,--no-undefined")
& $compiler $transactionSource @common "-o" $transaction
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw host transaction build failed" }
& $compiler $resolverSource @common "-o" $resolver
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw payload resolver build failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $transaction $resolver $payload $readelf
if ($LASTEXITCODE -ne 0) { throw "BarrelYaw host transaction policy failed" }

foreach ($artifact in @($transaction, $resolver)) {
    $hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output "$([IO.Path]::GetFileName($artifact)) sha256=$hash"
}
Write-Output "BARREL_YAW_HOST_TRANSACTION_BUILD passed=1 runtime=disabled deployed=0 device_access=0 game_writes=0"
