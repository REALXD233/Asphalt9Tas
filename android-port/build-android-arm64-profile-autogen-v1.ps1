param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d",
    [ValidateSet('arm64','x86_64')][string]$HostMachine = 'arm64'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root 'src/android_arm64_profile_autogen_v1.cpp'
$catalog = Join-Path $root 'src/a9_arm64_profile_signature_catalog_v1.h'
$compiler = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android24-clang++.cmd'
$readelf = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'
$outDir = Join-Path $root 'build/arm64-profile-autogen-v1'
$output = Join-Path $outDir 'a9tas_arm64_profile_autogen_v1'
$assetDir = Join-Path $root 'A9TasAndroid/app/src/main/assets/runtime'
$asset = Join-Path $assetDir 'a9tas_arm64_profile_autogen_v1'
$manifestPath = Join-Path $assetDir 'manifest.json'
$deviceName = 'a9tas_arm64_profile_autogen_v1'
$backendId = 'arm64-native-rooted-candidate-v1'
$expectedMachine = 'AArch64'
if ($HostMachine -eq 'x86_64') {
    $deviceName = 'a9tas_arm64_profile_autogen_x86_64_v1'
    $backendId = 'x86_64-nativebridge-libnb-0e34ebf9'
    $expectedMachine = 'Advanced Micro Devices X86-64'
    $compiler = Join-Path $NdkRoot 'toolchains/llvm/prebuilt/windows-x86_64/bin/x86_64-linux-android24-clang++.cmd'
    $outDir = Join-Path $root 'build/x86_64-profile-autogen-v1'
    $output = Join-Path $outDir $deviceName
    $asset = Join-Path $assetDir $deviceName
}
foreach ($path in @($source,$catalog,$compiler,$readelf,$manifestPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing ARM64 Profile autogen input: $path"
    }
}
New-Item -ItemType Directory -Force -Path $outDir,$assetDir | Out-Null
& $compiler $source "-I$(Join-Path $root 'src')" -O2 -std=c++20 -fPIE -pie `
    -fno-exceptions -fno-rtti -static-libstdc++ -Wall -Wextra -Werror `
    '-Wl,--build-id=sha1' -o $output
if ($LASTEXITCODE -ne 0) { throw 'ARM64 Profile autogen build failed' }
$header = & $readelf -h $output | Out-String
if ($header -notmatch ('Machine:\s+' + [regex]::Escape($expectedMachine)) -or $header -notmatch 'Type:\s+DYN') {
    throw 'ARM64 Profile autogen ELF identity mismatch'
}
Copy-Item -LiteralPath $output -Destination $asset -Force
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $asset).Hash.ToLowerInvariant()
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$backend = @($manifest.artifact_sets | Where-Object id -eq $backendId)
if ($backend.Count -ne 1) { throw 'Native ARM64 backend is unavailable' }
$backend = $backend[0]
if ($null -eq $backend.roles.PSObject.Properties['profile_autogen']) {
    $backend.roles | Add-Member -NotePropertyName profile_autogen -NotePropertyValue $deviceName
} else {
    $backend.roles.profile_autogen = $deviceName
}
$existing = @($backend.artifacts | Where-Object device_name -eq $deviceName)
if ($existing.Count -eq 0) {
    $backend.artifacts += [pscustomobject]@{
        asset = "runtime/$deviceName"
        device_name = $deviceName
        mode = '0700'
        sha256 = $sha
    }
} elseif ($existing.Count -eq 1) {
    $existing[0].asset = "runtime/$deviceName"
    $existing[0].mode = '0700'
    $existing[0].sha256 = $sha
} else { throw 'Duplicate ARM64 Profile autogen artifacts' }
$json = $manifest | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($manifestPath, $json + "`n", [Text.UTF8Encoding]::new($false))
Write-Output "ARM64_PROFILE_AUTOGEN_BUILD passed=1 host=$HostMachine core=arm64 api=24 sha256=$sha catalog_header=$catalog"
