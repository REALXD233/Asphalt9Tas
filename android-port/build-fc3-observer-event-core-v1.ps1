param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\fc3_observer_event_core_v1.cpp"
$protocol = Join-Path $root "src\fc3_replay_observer_state_machine_v1.cpp"
$policy = Join-Path $root "tools\test_fc3_observer_event_core_policy_v1.py"
$outDir = Join-Path $root "build\fc3-observer-event-core-v1"
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

$passive = Join-Path $outDir "fc3_observer_event_core_v1_build_only"
$review = Join-Path $outDir "fc3_observer_event_core_v1_review_only.o"
$disassembly = Join-Path $outDir "fc3_observer_event_core_v1_review.disasm.txt"

& $compiler $source $protocol "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-DA9TAS_FC3_PROTOCOL_NO_MAIN=1" `
    "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "FC-3 event-core passive build failed" }

& $compiler $source "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-DA9TAS_FC3_EVENT_CORE_REVIEW=1" `
    "-c" "-o" $review
if ($LASTEXITCODE -ne 0) { throw "FC-3 event-core review build failed" }

& $objdump "-d" "--demangle" $review | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "FC-3 event-core disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $review $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "FC-3 event-core policy failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$reviewHash = (Get-FileHash -LiteralPath $review -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "FC3_EVENT_CORE_BUILD passed=1 runtime=disabled deployed=0 device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "review_object_sha256=$reviewHash"
Write-Output "disassembly_sha256=$disasmHash"
