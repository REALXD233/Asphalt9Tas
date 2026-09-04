param(
    [string]$NdkRoot = "C:\Users\Administrator\Documents\a9tasv2\toolchains\android-ndk-r27d"
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
if ($header -notmatch 'Machine:\s+AArch64' -or $header -notmatch 'Type:\s+DYN') {
    throw 'ARM64 Profile autogen ELF identity mismatch'
}
Copy-Item -LiteralPath $output -Destination $asset -Force
$sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $asset).Hash.ToLowerInvariant()
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$backend = @($manifest.artifact_sets | Where-Object id -eq 'arm64-native-rooted-candidate-v1')
if ($backend.Count -ne 1) { throw 'Native ARM64 backend is unavailable' }
$backend = $backend[0]
if ($null -eq $backend.roles.PSObject.Properties['profile_autogen']) {
    $backend.roles | Add-Member -NotePropertyName profile_autogen -NotePropertyValue 'a9tas_arm64_profile_autogen_v1'
} else {
    $backend.roles.profile_autogen = 'a9tas_arm64_profile_autogen_v1'
}
$existing = @($backend.artifacts | Where-Object device_name -eq 'a9tas_arm64_profile_autogen_v1')
if ($existing.Count -eq 0) {
    $backend.artifacts += [pscustomobject]@{
        asset = 'runtime/a9tas_arm64_profile_autogen_v1'
        device_name = 'a9tas_arm64_profile_autogen_v1'
        mode = '0700'
        sha256 = $sha
    }
} elseif ($existing.Count -eq 1) {
    $existing[0].asset = 'runtime/a9tas_arm64_profile_autogen_v1'
    $existing[0].mode = '0700'
    $existing[0].sha256 = $sha
} else { throw 'Duplicate ARM64 Profile autogen artifacts' }
$json = $manifest | ConvertTo-Json -Depth 12
[IO.File]::WriteAllText($manifestPath, $json + "`n", [Text.UTF8Encoding]::new($false))
Write-Output "ARM64_PROFILE_AUTOGEN_BUILD passed=1 api=24 sha256=$sha catalog_header=$catalog"
