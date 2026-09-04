param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_controller_v1.cpp"
$policy = Join-Path $root "tools\test_natural_action_lifecycle_controller_policy_v1.py"
$outDir = Join-Path $root "build\natural-action-lifecycle-controller-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool unavailable: $tool" }
}
$inert = Join-Path $outDir "natural_action_lifecycle_controller_v1_build_only"
$review = Join-Path $outDir "natural_action_lifecycle_controller_v1_review_only.o"
$disassembly = Join-Path $outDir "natural_action_lifecycle_controller_v1_review_only.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-I$(Join-Path $root 'src')" "-o" $inert
if ($LASTEXITCODE -ne 0) { throw "inert lifecycle controller build failed" }
& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
    "-DA9TAS_NAL_CONTROLLER_REVIEW=1" "-I$(Join-Path $root 'src')" `
    "-c" "-o" $review
if ($LASTEXITCODE -ne 0) { throw "lifecycle controller review object build failed" }
$inertHeader = (& $readelf -h $inert) -join "`n"
$reviewHeader = (& $readelf -h $review) -join "`n"
if ($inertHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $inertHeader -notmatch "Type:\s+DYN" -or
    $reviewHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $reviewHeader -notmatch "Type:\s+REL") {
    throw "lifecycle controller ELF identity mismatch"
}
& $objdump "-d" "--demangle" $review | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "controller review disassembly failed" }
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy
if ($LASTEXITCODE -ne 0) { throw "controller policy failed" }
$inertHash = (Get-FileHash -LiteralPath $inert -Algorithm SHA256).Hash.ToLowerInvariant()
$reviewHash = (Get-FileHash -LiteralPath $review -Algorithm SHA256).Hash.ToLowerInvariant()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_LIFECYCLE_CONTROLLER_V1_BUILD passed=1 runtime=disabled linked_live=0 review_unlinked=1 action_calls=0 device_access=0"
Write-Output "inert_sha256=$inertHash"
Write-Output "review_object_sha256=$reviewHash"
Write-Output "review_disassembly_sha256=$disasmHash"
