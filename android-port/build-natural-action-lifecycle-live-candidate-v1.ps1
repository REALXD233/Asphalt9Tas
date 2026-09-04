param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\natural_action_lifecycle_controller_v1.cpp"
$bootstrapSource = Join-Path $root "src\bootstrap_natural_action_lifecycle_v1_build.cpp"
$payload = Join-Path $root "build\natural-action-callback-lifecycle-v1\liba9tas_natural_action_callback_lifecycle_v1_build_only.so"
$audit = Join-Path $root "tools\audit_natural_action_lifecycle_candidate_v1.py"
$outDir = Join-Path $root "build\natural-action-lifecycle-live-candidate-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool unavailable: $tool" }
}
if (-not (Test-Path -LiteralPath $payload -PathType Leaf)) {
    throw "Pinned lifecycle payload is unavailable; build it first"
}

$candidate = Join-Path $outDir "natural_action_lifecycle_candidate_v1"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_nal_v1.so"
$disassembly = Join-Path $outDir "natural_action_lifecycle_candidate_v1.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wno-unused-function" "-DA9TAS_NAL_CONTROLLER_REVIEW=1" `
    "-I$(Join-Path $root 'src')" "-o" $candidate
if ($LASTEXITCODE -ne 0) { throw "linked lifecycle candidate build failed" }
& $compiler $bootstrapSource "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--build-id=sha1" "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "lifecycle preload bootstrap build failed" }
$header = (& $readelf -h $candidate) -join "`n"
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
if ($header -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $header -notmatch "Type:\s+DYN" -or
    $bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64" -or
    $bootstrapHeader -notmatch "Type:\s+DYN") {
    throw "linked lifecycle candidate ELF identity mismatch"
}
$bootstrapSymbols = (& $readelf --dyn-syms $bootstrap) -join "`n"
if ($bootstrapSymbols -notmatch "a9tas_bootstrap_status" -or
    $bootstrapSymbols -notmatch "a9tas_bootstrap_stage") {
    throw "lifecycle preload bootstrap status ABI missing"
}
& $objdump "-d" "--demangle" $candidate | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "linked candidate disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $audit $candidate $payload $readelf $source $MyInvocation.MyCommand.Path
if ($LASTEXITCODE -ne 0) { throw "linked lifecycle candidate audit failed" }
$candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_LIFECYCLE_LIVE_CANDIDATE_V1_BUILD passed=1 linked=1 deployed=0 device_access=0 action_calls=0"
Write-Output "candidate_sha256=$candidateHash"
Write-Output "bootstrap_sha256=$bootstrapHash"
Write-Output "disassembly_sha256=$disassemblyHash"
