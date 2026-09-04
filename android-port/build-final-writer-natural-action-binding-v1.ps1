param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\final_writer_natural_action_binding_selftest_v1.cpp"
$header = Join-Path $root "src\final_writer_natural_action_binding_v1.h"
$policy = Join-Path $root "tools\test_final_writer_natural_action_binding_policy_v1.py"
$outDir = Join-Path $root "build\final-writer-natural-action-binding-v1"
$output = Join-Path $outDir "final_writer_natural_action_binding_selftest_v1_review_only.o"
$compiler = Join-Path $NdkRoot `
    "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach ($path in @($source, $header, $policy, $compiler)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing final-writer/natural-action binding input: $path"
    }
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $compiler $source "-O2" "-std=c++20" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-I$(Join-Path $root 'src')" "-c" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "final-writer/natural-action binding build failed" }
python -B $policy $output
if ($LASTEXITCODE -ne 0) { throw "final-writer/natural-action binding policy failed" }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "FINAL_WRITER_NATURAL_ACTION_BINDING_BUILD passed=1 compiled_selftest=1 runtime=disabled dual_receipt=1 deployed=0 device_access=0"
Write-Output "review_object_sha256=$hash"
