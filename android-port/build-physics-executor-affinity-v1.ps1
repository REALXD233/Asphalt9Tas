param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\payload_physics_executor_affinity_v1.cpp"
$outDir = Join-Path $root "build\physics-executor-affinity-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "aarch64-linux-android24-clang++.cmd"
$x64Compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $x64Compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$output = Join-Path $outDir "liba9tas_physics_executor_affinity_v1_build_only.so"
$bootstrap = Join-Path $outDir "liba9tas_bootstrap_physics_executor_affinity_v1_build_only.so"
$disassembly = Join-Path $outDir "physics_executor_affinity_v1.disasm.txt"
& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-fPIC" "-shared" "-Wl,--no-undefined" "-llog" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "P1 ARM64 observer build failed" }
& $x64Compiler "-shared" "-fPIC" "-O2" "-std=c++20" `
    "-static-libstdc++" "-Wall" "-Wextra" "-Werror" `
    (Join-Path $root "src\bootstrap_physics_executor_affinity_v1_build.cpp") `
    "-llog" "-ldl" "-o" $bootstrap
if ($LASTEXITCODE -ne 0) { throw "P1 x86_64 bootstrap build failed" }

$header = (& $readelf -h $output) -join "`n"
if ($header -notmatch "Machine:\s+AArch64") {
    throw "P1 observer is not ARM64"
}
$bootstrapHeader = (& $readelf -h $bootstrap) -join "`n"
if ($bootstrapHeader -notmatch "Machine:\s+Advanced Micro Devices X86-64") {
    throw "P1 bootstrap is not x86_64"
}
$symbols = (& $readelf --dyn-syms --wide $output) -join "`n"
$dynamic = (& $readelf -d $output) -join "`n"
if ($dynamic -match "libc\+\+_shared\.so") {
    throw "P1 observer must not depend on namespace-sensitive libc++_shared.so"
}
foreach ($name in @(
    "a9tas_physics_executor_affinity_v1_protocol",
    "a9tas_physics_executor_affinity_v1_status",
    "a9tas_physics_executor_affinity_v1_report",
    "a9tas_physics_executor_affinity_v1_report_size",
    "a9tas_physics_executor_affinity_v1_arm",
    "a9tas_physics_executor_affinity_v1_report_storage",
    "a9tas_physics_executor_affinity_v1_control_storage",
    "a9tas_physics_executor_affinity_v1_control_size")) {
    if ($symbols -notmatch [regex]::Escape($name)) {
        throw "Missing required P1 export: $name"
    }
}
if ($symbols -match "PhysicsExecutorAffinityEntryV1|PhysicsExecutorAffinityCaptureV1") {
    throw "P1 hidden hook symbols leaked into the dynamic export table"
}
$bootstrapSymbols = (& $readelf --dyn-syms --wide $bootstrap) -join "`n"
foreach ($name in @(
    "a9tas_bootstrap_stage",
    "a9tas_bootstrap_same_thread_probe_status",
    "a9tas_bootstrap_same_thread_probe_trampoline")) {
    if ($bootstrapSymbols -notmatch [regex]::Escape($name)) {
        throw "Missing required P1 bootstrap export: $name"
    }
}

& $objdump "-d" "--demangle" $output | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "P1 disassembly failed" }
$python = Get-Command python.exe -ErrorAction Stop
& $python.Source (Join-Path $root "tools\verify_physics_executor_affinity_v1.py") `
    $disassembly
if ($LASTEXITCODE -ne 0) { throw "P1 disassembly audit failed" }
& $python.Source (Join-Path $root "tools\test_parse_physics_executor_affinity_v1.py")
if ($LASTEXITCODE -ne 0) { throw "P1 report parser tests failed" }

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLower()
$bootstrapHash = (Get-FileHash -LiteralPath $bootstrap -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "PHYSICS_EXECUTOR_AFFINITY_V1_BUILD passed=1 passive=1 arm_return=-100 parser_tests=6"
Write-Output "sha256=$hash"
Write-Output "bootstrap_sha256=$bootstrapHash"
Write-Output "disassembly_sha256=$disasmHash"
