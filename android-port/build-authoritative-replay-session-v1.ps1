param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sessionSource = Join-Path $root "src\authoritative_replay_session_v1.cpp"
$tickSource = Join-Path $root "src\authoritative_tick_state_machine_v1.cpp"
$policy = Join-Path $root "tools\test_authoritative_replay_session_cpp_policy_v1.py"
$outDir = Join-Path $root "build\authoritative-replay-session-v1"
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

$passive = Join-Path $outDir "authoritative_replay_session_v1_build_only"
$sessionObject = Join-Path $outDir "authoritative_replay_session_v1_selftest.o"
$tickObject = Join-Path $outDir "authoritative_tick_state_machine_v1_companion.o"
$disassembly = Join-Path $outDir "authoritative_replay_session_v1_selftest.disasm.txt"

& $compiler $sessionSource $tickSource "-O2" "-std=c++20" "-static-libstdc++" `
    "-fno-exceptions" "-fno-rtti" "-Wall" "-Wextra" "-Werror" `
    "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1" "-Wl,--no-undefined" "-o" $passive
if ($LASTEXITCODE -ne 0) { throw "authoritative replay session passive build failed" }

& $compiler $sessionSource "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" `
    "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_SELFTEST=1" `
    "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1" "-c" "-o" $sessionObject
if ($LASTEXITCODE -ne 0) { throw "authoritative replay session review-object build failed" }

& $compiler $tickSource "-O2" "-std=c++20" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1" `
    "-c" "-o" $tickObject
if ($LASTEXITCODE -ne 0) { throw "authoritative tick companion-object build failed" }

& $objdump "-d" "--demangle" $sessionObject | Set-Content -LiteralPath $disassembly
if ($LASTEXITCODE -ne 0) { throw "authoritative replay session disassembly failed" }

$python = Get-Command python.exe -ErrorAction Stop
& $python.Source $policy $passive $sessionObject $tickObject $readelf $objdump
if ($LASTEXITCODE -ne 0) { throw "authoritative replay session policy failed" }

$passiveHash = (Get-FileHash -LiteralPath $passive -Algorithm SHA256).Hash.ToLower()
$sessionHash = (Get-FileHash -LiteralPath $sessionObject -Algorithm SHA256).Hash.ToLower()
$tickHash = (Get-FileHash -LiteralPath $tickObject -Algorithm SHA256).Hash.ToLower()
$disasmHash = (Get-FileHash -LiteralPath $disassembly -Algorithm SHA256).Hash.ToLower()
Write-Output "AUTH_REPLAY_SESSION_BUILD passed=1 runtime=disabled deployed=0 device_access=0"
Write-Output "passive_sha256=$passiveHash"
Write-Output "session_object_sha256=$sessionHash"
Write-Output "tick_object_sha256=$tickHash"
Write-Output "disassembly_sha256=$disasmHash"

