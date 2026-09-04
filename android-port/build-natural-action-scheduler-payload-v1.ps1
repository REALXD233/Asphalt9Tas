param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_natural_action_callback_lifecycle_v1.cpp"
$resolverSource = Join-Path $root "src\natural_action_scheduler_elf_resolver_build_only.cpp"
$passive = Join-Path $root "build\natural-action-callback-lifecycle-v1\liba9tas_natural_action_callback_lifecycle_v1_build_only.so"
$audit = Join-Path $root "tools\audit_natural_action_scheduler_payload_v1.py"
$resolverTest = Join-Path $root "tools\test_natural_action_scheduler_elf_resolver_v1.py"
$outDir = Join-Path $root "build\natural-action-scheduler-payload-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $x64Compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required tool unavailable: $tool" }
}
if (-not (Test-Path -LiteralPath $passive -PathType Leaf)) {
    throw "Pinned passive lifecycle payload is unavailable; build it first"
}

$payload = Join-Path $outDir "liba9tas_natural_action_scheduler_v1_review_only.so"
$disassembly = Join-Path $outDir "liba9tas_natural_action_scheduler_v1_review_only.disasm.txt"
$resolverObject = Join-Path $outDir "natural_action_scheduler_elf_resolver_review_only.o"
& $compiler $source "-O2" "-std=c++20" "-shared" "-fPIC" `
    "-static-libstdc++" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--no-undefined" `
    "-Wl,--build-id=sha1" "-DA9TAS_NAL_ACTION_EXECUTE=1" `
    "-I$(Join-Path $root 'src')" "-o" $payload
if ($LASTEXITCODE -ne 0) { throw "natural scheduler action payload build failed" }
& $x64Compiler $resolverSource "-O2" "-std=c++20" "-Wall" "-Wextra" `
    "-Werror" "-I$(Join-Path $root 'src')" "-c" "-o" $resolverObject
if ($LASTEXITCODE -ne 0) { throw "natural scheduler action resolver review object failed" }
& $objdump "-d" "--demangle" $payload | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "natural scheduler action disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $audit $payload $passive $readelf $objdump $source $MyInvocation.MyCommand.Path
if ($LASTEXITCODE -ne 0) { throw "natural scheduler action payload audit failed" }
& $python.Source $resolverTest $payload
if ($LASTEXITCODE -ne 0) { throw "natural scheduler action resolver identity failed" }
$payloadHash = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant()
$disassemblyHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLowerInvariant()
$resolverHash = (Get-FileHash -LiteralPath $resolverObject -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "NATURAL_ACTION_SCHEDULER_PAYLOAD_V1_BUILD passed=1 action_calls_compiled=1 arm_rejected=1 controller=0 runner=0 deployed=0 device_access=0"
Write-Output "payload_sha256=$payloadHash"
Write-Output "disassembly_sha256=$disassemblyHash"
Write-Output "resolver_object_sha256=$resolverHash"
