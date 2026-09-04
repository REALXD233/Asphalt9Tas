param(
    [switch]$SkipNativeBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$artifactSourcePath = Join-Path $PSScriptRoot 'A9CameraAndroid/app/src/main/java/dev/a9camera/android/CameraArtifacts.java'
$artifactSource = Get-Content -LiteralPath $artifactSourcePath -Raw
if ($artifactSource -notmatch 'assetSha256\(context, artifact\.asset\)') {
    throw 'Camera artifact deployment must derive identity from the signed APK asset'
}
if ($artifactSource -match 'final String hash' -or
        $artifactSource -match '(?i)\b[A-Z0-9_]*SHA(?:256)?\s*=\s*"[0-9a-f]{64}"') {
    throw 'Stale hardcoded camera artifact SHA detected'
}
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $root 'A9CameraAndroid'
$assets = Join-Path $project 'app/src/main/assets/runtime'
$profileAssets = Join-Path $project 'app/src/main/assets/profiles'
$toolchains = Join-Path $root '.toolchains'
$sdk = Join-Path $toolchains 'android-sdk'
$java = Get-ChildItem -LiteralPath (Join-Path $toolchains 'jdk17') -Filter 'java.exe' `
    -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not $SkipNativeBuild) {
    & (Join-Path $root 'build-camera-tool-runtime-v2.ps1') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Camera Tool runtime build failed' }
    & (Join-Path $root 'build-camera-manager-graph-v1.ps1') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Camera manager graph build failed' }
}

python (Join-Path $root 'tools/build_camera_profiles_v1.py') `
    --registry (Join-Path $root 'A9TasAndroid/app/src/main/assets/profiles/registry.json') `
    --assets-root (Join-Path $root 'A9TasAndroid/app/src/main/assets') `
    --output $profileAssets
if ($LASTEXITCODE -ne 0) { throw 'Camera build-profile generation failed' }

New-Item -ItemType Directory -Force -Path $assets | Out-Null
$runtime = Join-Path $root 'build/camera-tool-runtime-v2'
$graph = Join-Path $root 'build/camera-manager-graph-v1'
$copies = @{
    (Join-Path $runtime 'liba9tas_camera_tool_runtime_v2_build_only.so') = 'liba9tas_camera_tool_runtime_v2_build_only.so'
    (Join-Path $runtime 'a9tas_camera_tool_runtime_transaction_v2') = 'a9tas_camera_tool_runtime_transaction_v2_x86_64'
    (Join-Path $runtime 'a9tas_camera_tool_runtime_input_stream_v2') = 'a9tas_camera_tool_runtime_input_stream_v2_x86_64'
    (Join-Path $runtime 'a9tas_camera_tool_runtime_transaction_v2_arm64') = 'a9tas_camera_tool_runtime_transaction_v2_arm64'
    (Join-Path $runtime 'a9tas_camera_tool_runtime_input_stream_v2_arm64') = 'a9tas_camera_tool_runtime_input_stream_v2_arm64'
    (Join-Path $runtime 'a9tas_camera_tool_native_arm64_loader_v1') = 'a9tas_camera_tool_native_arm64_loader_v1'
    (Join-Path $runtime 'liba9tas_bootstrap_camera_tool_runtime_v2.so') = 'liba9tas_bootstrap_camera_tool_runtime_v2.so'
    (Join-Path $graph 'a9tas_camera_manager_graph_check_v1_arm64') = 'a9tas_camera_manager_graph_check_v1_arm64'
    (Join-Path $graph 'a9tas_camera_manager_graph_check_v1_x86_64') = 'a9tas_camera_manager_graph_check_v1_x86_64'
    (Join-Path $graph 'a9tas_gameplay_hud_graph_check_v1_arm64') = 'a9tas_gameplay_hud_graph_check_v1_arm64'
    (Join-Path $graph 'a9tas_gameplay_hud_graph_check_v1_x86_64') = 'a9tas_gameplay_hud_graph_check_v1_x86_64'
    (Join-Path $root 'build/a9tas_injector') = 'a9tas_injector_camera_tool_runtime_v2'
}
foreach ($entry in $copies.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Key -PathType Leaf)) {
        throw "Missing Camera Tool APK runtime: $($entry.Key)"
    }
    Copy-Item -LiteralPath $entry.Key -Destination (Join-Path $assets $entry.Value) -Force
}

if ($null -eq $java -or -not (Test-Path -LiteralPath (Join-Path $sdk 'platforms/android-35/android.jar'))) {
    throw 'Portable JDK 17 or Android platform 35 is not installed.'
}
$env:JAVA_HOME = Split-Path -Parent (Split-Path -Parent $java.FullName)
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$buildTools = Join-Path $sdk 'build-tools/35.0.0'
$androidJar = Join-Path $sdk 'platforms/android-35/android.jar'
$aapt2 = Join-Path $buildTools 'aapt2.exe'
$d8 = Join-Path $buildTools 'd8.bat'
$apksigner = Join-Path $buildTools 'apksigner.bat'
$lint = Join-Path $sdk 'cmdline-tools/latest/bin/lint.bat'
$javac = Join-Path $env:JAVA_HOME 'bin/javac.exe'
$jar = Join-Path $env:JAVA_HOME 'bin/jar.exe'
foreach ($required in @($aapt2,$d8,$apksigner,$lint,$javac,$jar,$androidJar)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing Android build tool: $required"
    }
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$work = Join-Path $project "app/build/manual/$stamp"
$classes = Join-Path $work 'classes'
$dex = Join-Path $work 'dex'
$generated = Join-Path $work 'generated'
$compiledResources = Join-Path $work 'resources.zip'
$unsignedApk = Join-Path $work 'unsigned.apk'
$signedApk = Join-Path $work 'a9-camera-debug.apk'
New-Item -ItemType Directory -Force -Path $classes,$dex,$generated | Out-Null

& $aapt2 compile --dir (Join-Path $project 'app/src/main/res') -o $compiledResources
if ($LASTEXITCODE -ne 0) { throw 'aapt2 resource compilation failed' }
& $aapt2 link -o $unsignedApk -I $androidJar `
    --manifest (Join-Path $project 'app/src/main/AndroidManifest.xml') `
    --min-sdk-version 24 --target-sdk-version 35 --version-code 11 `
    --version-name '0.11.0' --java $generated $compiledResources
if ($LASTEXITCODE -ne 0) { throw 'aapt2 APK link failed' }

$sources = @(Get-ChildItem -LiteralPath (Join-Path $project 'app/src/main/java'),$generated `
    -Filter '*.java' -File -Recurse | Select-Object -ExpandProperty FullName)
if ($sources.Count -eq 0) { throw 'No Java sources found' }

# Product boundary: this APK is a standalone camera utility. It must not acquire the
# TAS app's licensing, practice-mode, recording, or replay-session dependencies.
$ownedSources = @(Get-ChildItem -LiteralPath (Join-Path $project 'app/src/main/java') `
    -Filter '*.java' -File -Recurse)
$ownedText = ($ownedSources | ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join "`n"
$forbiddenProductTokens = @(
    'LicenseManager', 'CardKey', 'ActivationToken', 'PracticeModeGuard',
    'TasSession', 'ReplayRecording', 'ContinuousLap'
)
foreach ($token in $forbiddenProductTokens) {
    if ($ownedText.Contains($token)) {
        throw "Standalone Camera APK contains forbidden TAS/licensing dependency: $token"
    }
}
$manifestText = [IO.File]::ReadAllText((Join-Path $project 'app/src/main/AndroidManifest.xml'))
if (-not $manifestText.Contains('package="dev.a9camera.android"')) {
    throw 'Standalone Camera APK package identity drifted'
}
if ($manifestText.Contains('android.permission.INTERNET')) {
    throw 'Standalone Camera APK must remain offline and may not request INTERNET'
}
& $javac -encoding UTF-8 -source 17 -target 17 -classpath $androidJar -d $classes @sources
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

$lintProject = Join-Path $root "build/a9camera-api24-lint-$stamp"
$lintSource = Join-Path $lintProject 'src'
$lintClasses = Join-Path $lintProject 'bin/classes'
New-Item -ItemType Directory -Force -Path $lintSource,$lintClasses | Out-Null
Copy-Item -LiteralPath (Join-Path $project 'app/src/main/java/dev') -Destination $lintSource -Recurse -Force
Copy-Item -LiteralPath (Join-Path $classes 'dev') -Destination $lintClasses -Recurse -Force
$manifest = [IO.File]::ReadAllText((Join-Path $project 'app/src/main/AndroidManifest.xml'))
$manifest = $manifest.Replace('    package="dev.a9camera.android">',
    "    package=`"dev.a9camera.android`">`r`n    <uses-sdk android:minSdkVersion=`"24`" android:targetSdkVersion=`"35`" />")
[IO.File]::WriteAllText((Join-Path $lintProject 'AndroidManifest.xml'), $manifest,
    [Text.UTF8Encoding]::new($false))
& $lint --check NewApi --exitcode --sdk-home $sdk --compile-sdk-version 35 `
    --java-language-level 17 $lintProject
if ($LASTEXITCODE -ne 0) { throw 'Android API-24 NewApi lint failed' }

$classesJar = Join-Path $work 'classes.jar'
& $jar cf $classesJar -C $classes .
if ($LASTEXITCODE -ne 0) { throw 'Java archive failed' }
& $d8 --lib $androidJar --min-api 24 --output $dex $classesJar
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }
& $jar uf $unsignedApk -C $dex classes.dex
if ($LASTEXITCODE -ne 0) { throw 'classes.dex packaging failed' }
& $jar uf $unsignedApk -C (Join-Path $project 'app/src/main') assets
if ($LASTEXITCODE -ne 0) { throw 'asset packaging failed' }

$keystore = Join-Path $project 'debug.keystore'
if (-not (Test-Path -LiteralPath $keystore -PathType Leaf)) {
    & (Join-Path $env:JAVA_HOME 'bin/keytool.exe') -genkeypair -noprompt `
        -keystore $keystore -storepass android -keypass android `
        -alias androiddebugkey -dname 'CN=A9 Camera,O=Research,C=US' `
        -keyalg RSA -keysize 2048 -validity 10000
    if ($LASTEXITCODE -ne 0) { throw 'debug signing key generation failed' }
}
& $apksigner sign --ks $keystore --ks-pass pass:android --key-pass pass:android `
    --out $signedApk $unsignedApk
if ($LASTEXITCODE -ne 0) { throw 'APK signing failed' }
& $apksigner verify --verbose --print-certs $signedApk | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed' }

$apk = Join-Path $project 'app/build/outputs/apk/debug/a9-camera-debug.apk'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $apk) | Out-Null
Copy-Item -LiteralPath $signedApk -Destination $apk -Force
$entries = @(& $jar tf $apk)
foreach ($required in @('classes.dex','assets/runtime/liba9tas_camera_tool_runtime_v2_build_only.so',
        'assets/runtime/a9tas_camera_tool_native_arm64_loader_v1',
        'assets/runtime/a9tas_camera_tool_runtime_transaction_v2_arm64',
        'assets/runtime/a9tas_camera_tool_runtime_input_stream_v2_arm64',
        'assets/runtime/a9tas_camera_manager_graph_check_v1_arm64',
        'assets/runtime/a9tas_camera_tool_runtime_transaction_v2_x86_64',
        'assets/runtime/a9tas_camera_tool_runtime_input_stream_v2_x86_64',
        'assets/runtime/a9tas_camera_manager_graph_check_v1_x86_64',
        'assets/runtime/a9tas_gameplay_hud_graph_check_v1_arm64',
        'assets/runtime/a9tas_gameplay_hud_graph_check_v1_x86_64',
        'assets/profiles/439fd7f94ef570d9.a9camera.bin',
        'assets/profiles/671522d4614abcce.a9camera.bin',
        'assets/profiles/registry.json',
        'assets/runtime/liba9tas_bootstrap_camera_tool_runtime_v2.so',
        'assets/runtime/a9tas_injector_camera_tool_runtime_v2')) {
    if ($entries -notcontains $required) { throw "APK entry missing: $required" }
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $apk).Hash.ToLowerInvariant()
Write-Output "A9_CAMERA_ANDROID_BUILD passed=1 min_sdk=24 arm64=1 x86_64_nativebridge=1 package=dev.a9camera.android apk=$apk sha256=$hash"
Write-Output 'A9_CAMERA_PRODUCT_BOUNDARY passed=1 standalone=1 offline=1 license_gate=0 card_key=0 practice_gate=0 tas_session_dependency=0'
