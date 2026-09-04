param([string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root "src\startline_pause_gate_v1.cpp"
$out = Join-Path $root "build\startline-pause-gate-v1"
$cc = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\x86_64-linux-android24-clang++.cmd"
foreach($path in @($source,$cc)){if(-not(Test-Path $path)){throw "missing: $path"}}
New-Item -ItemType Directory -Path $out -Force | Out-Null
$object = Join-Path $out "startline_pause_gate_v1.o"
& $cc -O2 -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -DA9TAS_STARTLINE_PAUSE_GATE_NO_MAIN=1 -c $source -o $object
if($LASTEXITCODE -ne 0){throw "startline pause gate build failed"}
Push-Location (Join-Path $root "tools")
try{python -B -m unittest test_startline_pause_gate_v1; $testCode=$LASTEXITCODE}finally{Pop-Location}
if($testCode -ne 0){throw "startline pause gate tests failed"}
Write-Output "STARTLINE_PAUSE_GATE_BUILD passed=1 runtime=disabled device_access=0 game_writes=0"
Write-Output "startline_pause_gate_v1.o_sha256=$((Get-FileHash $object -Algorithm SHA256).Hash.ToLower())"
