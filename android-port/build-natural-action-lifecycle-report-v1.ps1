param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_report_selftest_v1.cpp"
$validator = Join-Path $root "tools\validate_natural_action_lifecycle_report_v1.py"
$outDir = Join-Path $root "build\natural-action-lifecycle-report-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe"
foreach ($tool in @($compiler, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool unavailable: $tool" }
}
$selftest = Join-Path $outDir "natural_action_lifecycle_report_v1_selftest"
& $compiler $source "-O2" "-std=c++20" "-Wall" "-Wextra" "-Werror" `
    "-static" "-fPIE" "-I$(Join-Path $root 'src')" "-o" $selftest
if ($LASTEXITCODE -ne 0) { throw "lifecycle report ABI selftest build failed" }
$header = (& $readelf -h $selftest) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+EXEC") { throw "lifecycle report selftest ELF mismatch" }
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $validator --selftest
if ($LASTEXITCODE -ne 0) { throw "lifecycle report validator selftest failed" }
$hash = (Get-FileHash -LiteralPath $selftest -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_LIFECYCLE_REPORT_V1_BUILD passed=1 runtime=disabled device_access=0"
Write-Output "selftest_sha256=$hash"
