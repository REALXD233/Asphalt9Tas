param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_transaction_selftest_v1.cpp"
$policy = Join-Path $root "tools\test_natural_action_lifecycle_transaction_policy_v1.py"
$model = Join-Path $root "tools\test_natural_action_lifecycle_transaction_v1.py"
$outDir = Join-Path $root "build\natural-action-lifecycle-transaction-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
foreach ($tool in @($compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$selftest = Join-Path $outDir "natural_action_lifecycle_transaction_v1_selftest"
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-static" "-fPIE" `
    "-I$(Join-Path $root 'src')" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "lifecycle transaction selftest build failed" }

$header = (& $readelf -h $selftest) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+EXEC") {
    throw "lifecycle transaction selftest ELF is invalid"
}
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $model
if ($LASTEXITCODE -ne 0) { throw "lifecycle transaction model failed" }
& $python.Source $policy
if ($LASTEXITCODE -ne 0) { throw "lifecycle transaction policy failed" }

$hash = (Get-FileHash -LiteralPath $selftest -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_LIFECYCLE_TRANSACTION_V1_BUILD passed=1 runtime=disabled zero_call_only=1 device_access=0"
Write-Output "selftest_sha256=$hash"
