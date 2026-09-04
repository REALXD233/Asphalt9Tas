param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $projectRoot "src\payload_game_action_rpc_v1.cpp"
$outputDir = Join-Path $projectRoot "build\game-action-rpc-v1"
$compiler = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android24-clang++.cmd"
$readelf = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe"
$output = Join-Path $outputDir "liba9tas_game_action_rpc_v1.so"

if (-not (Test-Path -LiteralPath $compiler)) {
    throw "NDK compiler not found: $compiler"
}
if (-not (Test-Path -LiteralPath $readelf)) {
    throw "llvm-readelf not found: $readelf"
}
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

& $compiler "-shared" "-fPIC" "-O2" "-std=c++20" "-static-libstdc++" `
    "-fvisibility=hidden" "-fno-exceptions" "-fno-rtti" `
    "-Wall" "-Wextra" "-Werror" "-Wl,--build-id=sha1" `
    $source "-llog" "-ldl" "-o" $output
if ($LASTEXITCODE -ne 0) { throw "game action RPC v1 payload build failed" }

$header = (& $readelf -h $output) -join "`n"
if ($header -notmatch "AArch64") { throw "unexpected output architecture" }
$symbols = (& $readelf --dyn-syms $output) -join "`n"
if ($symbols -notmatch "a9tas_nitro_rpc_v1_status") {
    throw "required status export missing"
}
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Built $output"
Write-Output "mode=observe-only execute_compile_gate=0 hooks=0 code_patches=0"
Write-Output "SHA256 $hash"
