param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\barrel_stabilization_replay_core_v1.cpp"
$policy = Join-Path $root "tools\test_barrel_stabilization_replay_core_policy_v1.py"
$outDir = Join-Path $root "build\barrel-stabilization-replay-core-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
foreach ($tool in @($compiler, $readelf, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$passive = Join-Path $outDir "barrel_stabilization_replay_core_v1_build_only"
$selftestObject = Join-Path $outDir "barrel_stabilization_replay_core_v1_selftest.o"
$disassembly = Join-Path $outDir "barrel_stabilization_replay_core_v1_selftest.disasm.txt"

& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "barrel replay passive build failed" }

& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" `
    "-DA9TAS_BARREL_STABILIZATION_REPLAY_SELFTEST=1" `
    "-DA9TAS_BARREL_STABILIZATION_REPLAY_NO_MAIN=1" `
    "-c" "-o" $selftestObject
if ($LASTEXITCODE -ne 0) { throw "barrel replay review-object build failed" }

& $objdump "-d" "--demangle" $selftestObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "barrel replay disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $selftestObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "barrel replay policy failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$objectHash = (Get-FileHash -LiteralPath $selftestObject -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "BARREL_STABILIZATION_REPLAY_BUILD passed=1 runtime=disabled deployed=0 device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "selftest_object_sha256=$objectHash"
Write-Output "disassembly_sha256=$disasmHash"
