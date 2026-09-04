param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_natural_action_scheduler_v1_build.cpp"
$payload = Join-Path $root "build\natural-action-scheduler-payload-v1\liba9tas_natural_action_scheduler_v1_review_only.so"
$validator = Join-Path $root "tools\validate_natural_action_scheduler_report_v1.py"
$audit = Join-Path $root "tools\audit_natural_action_scheduler_candidate_v1.py"
$outDir = Join-Path $root "build\natural-action-scheduler-live-candidate-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool unavailable: $tool" }
}
if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
    throw "Pinned natural scheduler payload is unavailable; build it first"
}

$candidate = Join-Path $outDir "natural_action_scheduler_candidate_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_nas_v1.so"
$disassembly = Join-Path $outDir "natural_action_scheduler_candidate_v1.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-DA9TAS_NAL_CONTROLLER_REVIEW=1" `
    "-DA9TAS_NAL_ACTION_CONTROLLER_REVIEW=1" `
    "-I$(Join-Path $root 'src')" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "linked natural scheduler candidate build failed" }
& $compiler $bootstrapSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "natural scheduler preload bootstrap build failed" }
& $objdump "-d" "--demangle" $candidate | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "natural scheduler candidate disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $validator "--selftest"
if ($LASTEXITCODE -ne 0) { throw "natural scheduler report validator failed" }
& $python.Source $audit $candidate $bootstrap $payload $readelf $source `
    $bootstrapSource $MyInvocation.MyCommand.Path
if ($LASTEXITCODE -ne 0) { throw "natural scheduler live candidate audit failed" }
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_SCHEDULER_LIVE_CANDIDATE_V1_BUILD passed=1 linked=1 activation_count=1 deployed=0 device_access=0"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
Write-Output "disassembly_sha256=$disassemblyHash"
