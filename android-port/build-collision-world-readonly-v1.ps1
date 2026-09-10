param([string]$NdkRoot='D:/AsphaltTAS/toolchains/android-ndk-r27d')
$ErrorActionPreference='Stop'
$portRoot=Split-Path -Parent $MyInvocation.MyCommand.Path
$bin=Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin'
foreach($target in @(@('aarch64','arm64'),@('x86_64','x86_64'))) {
    $compiler=Join-Path $bin ($target[0]+'-linux-android24-clang++.cmd')
    $output=Join-Path $portRoot ('build/collision_world_readonly_probe_'+$target[1]+'_v1')
    & $compiler -std=c++17 -O2 -Wall -Wextra -Werror -static-libstdc++ '-Wl,-z,max-page-size=16384' (Join-Path $portRoot 'src/collision_world_readonly_probe_v1.cpp') -o $output
    if($LASTEXITCODE -ne 0){throw 'Read-only world probe build failed'}
    Get-FileHash -LiteralPath $output -Algorithm SHA256
}
