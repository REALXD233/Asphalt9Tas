param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\fc3_replay_observer_identity_resolver_v1.cpp"
$policy = Join-Path $root "tools\test_fc3_identity_resolver_policy_v1.py"
$outDir = Join-Path $root "build\fc3-replay-observer-identity-v1"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

$toolBin = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$compiler = Join-Path $toolBin "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $toolBin "llvm-readelf.exe"
$objdump = Join-Path $toolBin "llvm-objdump.exe"
$objcopy = Join-Path $toolBin "llvm-objcopy.exe"
foreach ($tool in @($compiler, $readelf, $objdump, $objcopy)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required NDK tool unavailable: $tool"
    }
}

$passive = Join-Path $outDir "fc3_replay_observer_identity_v1_build_only"
$review = Join-Path $outDir "liba9tas_fc3_identity_resolver_review_only.so"
$intermediate = Join-Path $outDir "fc3_replay_observer_identity_v1.intermediate.o"
$disassembly = Join-Path $outDir "fc3_replay_observer_identity_v1_review.disasm.txt"

& $compiler $source "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "FC-3 identity passive build failed" }

try {
    & $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
        "-fPIC" "-ffunction-sections" "-fdata-sections" `
        "-Wall" "-Wextra" "-Werror" "-Wno-unused-function" `
        "-DA9TAS_FC3_IDENTITY_REVIEW=1" "-c" "-o" $intermediate
    if ($LASTEXITCODE -ne 0) { throw "FC-3 identity review compile failed" }

    # The included legacy scheduler observer has an externally visible renamed
    # main.  Localize that unrelated symbol before --gc-sections so the final
    # review DSO contains only the reachable read-only resolver graph.
    & $objcopy `
        "--localize-symbol=_Z34A9TasSchedulerObserverMain_NotUsediPPc" `
        $intermediate
    if ($LASTEXITCODE -ne 0) { throw "FC-3 identity symbol isolation failed" }

    & $compiler $intermediate "-shared" "-Wl,--gc-sections" `
        "-Wl,--no-undefined" "-o" $review
    if ($LASTEXITCODE -ne 0) { throw "FC-3 identity review link failed" }
} finally {
    if (Test-Path -LiteralPath $intermediate) {
        Remove-Item -LiteralPath $intermediate -Force
    }
}
if (Test-Path -LiteralPath $intermediate) {
    throw "FC-3 identity intermediate object was retained"
}

& $objdump "-d" "--demangle" $review | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-3 identity disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $review $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-3 identity policy failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$reviewHash = (Get-FileHash -LiteralPath $review -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "FC3_IDENTITY_BUILD passed=1 runtime=disabled deployed=0 device_access=0 review=read_only"
Write-Output "passive_sha256=$passiveHash"
Write-Output "review_sha256=$reviewHash"
Write-Output "disassembly_sha256=$disasmHash"
