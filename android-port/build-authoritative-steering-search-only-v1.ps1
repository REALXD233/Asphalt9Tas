param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$out = Join-Path $root "build\authoritative-steering-search-only-v1"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sources = @(
    (Join-Path $src "hwbp_authoritative_steering_v1.cpp"),
    (Join-Path $src "authoritative_natural_handoff_v1.cpp"),
    (Join-Path $src "authoritative_adapter_bridge_v1.cpp"),
    (Join-Path $src "authoritative_steering_transport_v1.cpp"),
    (Join-Path $src "authoritative_replay_session_v1.cpp"),
    (Join-Path $src "authoritative_tick_state_machine_v1.cpp")
)
$tool = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$cc = Join-Path $tool "x86_64-linux-android24-clang++.cmd"
$readelf = Join-Path $tool "llvm-readelf.exe"
$objdump = Join-Path $tool "llvm-objdump.exe"
$policy = Join-Path $root "tools\test_authoritative_steering_search_only_policy_v1.py"
$validator = Join-Path $root "tools\validate_authoritative_steering_search_only_v1.py"
foreach($path in @($cc,$readelf,$objdump,$policy,$validator)+$sources){if(-not(Test-Path $path)){throw "missing: $path"}}
$defs = @(
    "-DA9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY=1",
    "-DA9TAS_AUTHORITATIVE_NATURAL_HANDOFF_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_STEERING_TRANSPORT_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN=1",
    "-DA9TAS_AUTHORITATIVE_TICK_NO_MAIN=1"
)
$flags = @("-O2","-std=c++20","-fno-exceptions","-fno-rtti","-Wall","-Wextra","-Werror")
$candidate = Join-Path $out "a9tas_authoritative_steering_search_only_v1"
$disasm = Join-Path $out "a9tas_authoritative_steering_search_only_v1.disasm.txt"
& $cc @flags "-static-libstdc++" @defs @sources "-Wl,--no-undefined" -o $candidate
if($LASTEXITCODE -ne 0){throw "search-only candidate build failed"}
& $objdump -d --demangle $candidate | Set-Content $disasm
python -B $policy $candidate $readelf $objdump
if($LASTEXITCODE -ne 0){throw "search-only artifact policy failed"}
python -B $validator --selftest
if($LASTEXITCODE -ne 0){throw "search-only validator selftest failed"}
Write-Output "AUTHORITATIVE_SEARCH_ONLY_BUILD passed=1 runtime_candidate=1 deployed=0 gameplay_writes=0"
foreach($path in @($candidate,$disasm)){Write-Output "$([IO.Path]::GetFileName($path))_sha256=$((Get-FileHash $path -Algorithm SHA256).Hash.ToLower())"}
