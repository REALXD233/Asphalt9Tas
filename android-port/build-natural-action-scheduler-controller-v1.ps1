param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_controller_v1.cpp"
$reportHeader = Join-Path $root "src\natural_action_scheduler_report_v1.h"
$validator = Join-Path $root "tools\validate_natural_action_scheduler_report_v1.py"
$audit = Join-Path $root "tools\audit_natural_action_scheduler_controller_v1.py"
$payload = Join-Path $root "build\natural-action-scheduler-payload-v1\liba9tas_natural_action_scheduler_v1_review_only.so"
$outDir = Join-Path $root "build\natural-action-scheduler-controller-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$object = Join-Path $outDir "natural_action_scheduler_controller_v1_review_only.o"
$disassembly = Join-Path $outDir "natural_action_scheduler_controller_v1_review_only.disasm.txt"
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool unavailable: $tool" }
}
if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
    throw "Pinned action scheduler payload is unavailable; build it first"
}

& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_NAL_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_ACTION_CONTROLLER_REVIEW=1" `
    "-I$(Join-Path $root 'src')" "-c" "-o" $object
if ($LASTEXITCODE -ne 0) { throw "natural scheduler controller review object build failed" }
& $objdump "-d" "--demangle" $object | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "natural scheduler controller disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $validator "--selftest"
if ($LASTEXITCODE -ne 0) { throw "natural scheduler report validator failed" }
& $python.Source $audit $object $disassembly $payload $readelf $source `
    $reportHeader $validator $MyInvocation.MyCommand.Path $outDir
if ($LASTEXITCODE -ne 0) { throw "natural scheduler controller audit failed" }
$objectHash = (Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_SCHEDULER_CONTROLLER_V1_BUILD passed=1 activation_count=1 runtime=disabled linked_live=0 review_unlinked=1 runner=0 device_access=0"
Write-Output "review_object_sha256=$objectHash"
Write-Output "review_disassembly_sha256=$disasmHash"
