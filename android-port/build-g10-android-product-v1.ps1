param(
    [switch]$SkipApkBuild,
    [switch]$DeveloperTest,
    [switch]$NoBuiltInProfiles,
    [ValidateSet('Debug','Release')]
    [string]$SigningMode = 'Debug',
    [string]$ReleaseKeystorePath = '',
    [string]$ReleaseKeyAlias = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$productVersionCode = if ($NoBuiltInProfiles) { 34 } elseif ($DeveloperTest) { 33 } else { 32 }
$productVersionName = if ($NoBuiltInProfiles) {
    '0.8.0-profile-autogen-empty-devtest'
} elseif ($DeveloperTest) {
    '0.8.0-profile-autogen-devtest'
} else {
    '0.8.0-profile-autogen'
}
$project = Join-Path $root 'A9TasAndroid'
$policy = Join-Path $root 'tools/test_g10_android_product_policy_v1.py'
$runtimeBackendPolicy = Join-Path $root 'tools/test_android_runtime_backend_registry_v1.py'
$archiveSelftestGenerator = Join-Path $root 'tools/make_g10_archive_selftest_v1.py'
$arm64RuntimeSync = Join-Path $root 'sync-native-arm64-runtime-assets-v1.ps1'
$identityProbeBuild = Join-Path $root 'build-android-identity-probe-v1.ps1'
$practiceProbeBuild = Join-Path $root 'build-practice-mode-readonly-probe-v1.ps1'
$profileAutogenBuild = Join-Path $root 'build-android-arm64-profile-autogen-v1.ps1'
& (Join-Path $root 'test-root-shell-host-v1.ps1') | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Root shell execution regression failed' }
$iconBuilder = Join-Path $root 'tools/build_android_icon_v2.ps1'
$toolchains = Join-Path $root '.toolchains'
$java = Get-ChildItem -LiteralPath (Join-Path $toolchains 'jdk17') -Filter 'java.exe' `
    -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
$sdk = Join-Path $toolchains 'android-sdk'
$releaseKeystoreResolved = ''
if ($DeveloperTest -and $SigningMode -ne 'Debug') {
    throw 'DeveloperTest must remain debug-signed.'
}
if ($NoBuiltInProfiles -and -not $DeveloperTest) {
    throw 'NoBuiltInProfiles is a developer-test-only build mode.'
}
if ($SigningMode -eq 'Release') {
    if ([string]::IsNullOrWhiteSpace($ReleaseKeystorePath) -or
        [string]::IsNullOrWhiteSpace($ReleaseKeyAlias)) {
        throw 'Release signing requires -ReleaseKeystorePath and -ReleaseKeyAlias.'
    }
    if (-not (Test-Path -LiteralPath $ReleaseKeystorePath -PathType Leaf)) {
        throw 'Release keystore file does not exist.'
    }
    if ([string]::IsNullOrWhiteSpace($env:A9TAS_RELEASE_STORE_PASSWORD) -or
        [string]::IsNullOrWhiteSpace($env:A9TAS_RELEASE_KEY_PASSWORD)) {
        throw 'Release signing passwords must be supplied through A9TAS_RELEASE_STORE_PASSWORD and A9TAS_RELEASE_KEY_PASSWORD.'
    }
    $releaseKeystoreResolved = (Resolve-Path -LiteralPath $ReleaseKeystorePath).Path
    $workspacePrefix = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
    if ([IO.Path]::GetFullPath($releaseKeystoreResolved).StartsWith(
            $workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Release keystore must live outside the source workspace.'
    }
}

$archiveSelftest = Join-Path $project 'app/src/main/assets/selftest/a9tas1-synthetic.a9tas'
$runtimeManifest = Join-Path $project 'app/src/main/assets/runtime/manifest.json'
if (-not (Test-Path -LiteralPath $runtimeManifest -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $runtimeManifest) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'config/runtime-manifest-template.json') `
        -Destination $runtimeManifest
}
& $identityProbeBuild | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Android identity probe build failed' }
& $practiceProbeBuild | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Practice-mode probe build failed' }
& $profileAutogenBuild | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'ARM64 Profile autogen build failed' }
& $profileAutogenBuild -HostMachine x86_64 | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'x86_64-host Profile autogen build failed' }
& $arm64RuntimeSync | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Native ARM64 runtime asset synchronization failed' }
& $iconBuilder | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Android launcher icon generation failed' }
python -B $archiveSelftestGenerator $archiveSelftest
if ($LASTEXITCODE -ne 0) { throw 'G10 A9TAS1 decoder sample generation failed' }
python -B $runtimeBackendPolicy
if ($LASTEXITCODE -ne 0) { throw 'Android runtime backend registry policy failed' }
python -B $policy
if ($LASTEXITCODE -ne 0) { throw 'G10 Android product policy failed' }

if ($SkipApkBuild) {
    Write-Output 'G10_ANDROID_PRODUCT_BUILD passed=1 apk=NOT_RUN policy_only=1'
    exit 0
}
if ($null -eq $java -or
    -not (Test-Path -LiteralPath (Join-Path $sdk 'platforms/android-35/android.jar'))) {
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
$signedApk = Join-Path $work $(if ($SigningMode -eq 'Release') {
    'a9tas-release.apk'
} else {
    'app-debug.apk'
})
New-Item -ItemType Directory -Force -Path $classes,$dex,$generated | Out-Null
$manifestForBuild = Join-Path $project 'app/src/main/AndroidManifest.xml'
if ($DeveloperTest) {
    $manifestForBuild = Join-Path $work 'AndroidManifest-devtest.xml'
    $developerApplicationId = if ($NoBuiltInProfiles) {
        'dev.a9tas.android.autogentest'
    } else {
        'dev.a9tas.android.devtest'
    }
    $developerLabel = if ($NoBuiltInProfiles) { 'A9 TAS AUTO' } else { 'A9 TAS DEV' }
    $developerManifest = [IO.File]::ReadAllText(
        (Join-Path $project 'app/src/main/AndroidManifest.xml'))
    $developerManifest = $developerManifest.Replace(
        'package="dev.a9tas.android"', "package=`"$developerApplicationId`"")
    $developerManifest = $developerManifest.Replace(
        'android:label="A9 TAS"', "android:label=`"$developerLabel`"")
    $developerManifest = $developerManifest.Replace(
        'android:name=".MainActivity"', 'android:name="dev.a9tas.android.MainActivity"')
    $developerManifest = $developerManifest.Replace(
        'android:name=".TasForegroundService"',
        'android:name="dev.a9tas.android.TasForegroundService"')
    $developerManifest = $developerManifest.Replace(
        'android:name=".DiagnosticShareProvider"',
        'android:name="dev.a9tas.android.DiagnosticShareProvider"')
    $developerManifest = $developerManifest.Replace(
        '${applicationId}.diagnostics', "$developerApplicationId.diagnostics")
    [IO.File]::WriteAllText($manifestForBuild, $developerManifest,
        [Text.UTF8Encoding]::new($false))
}

& $aapt2 compile --dir (Join-Path $project 'app/src/main/res') -o $compiledResources
if ($LASTEXITCODE -ne 0) { throw 'aapt2 resource compilation failed' }
& $aapt2 link -o $unsignedApk -I $androidJar `
    --manifest $manifestForBuild --custom-package dev.a9tas.android `
    --min-sdk-version 24 --target-sdk-version 35 --version-code $productVersionCode `
    --version-name $productVersionName --java $generated `
    $compiledResources
if ($LASTEXITCODE -ne 0) { throw 'aapt2 APK link failed' }

# The manual aapt2/d8 pipeline does not run Gradle Lint. Analyze a standalone
# source shadow so minSdk 24 is enforced against framework and Java APIs.
$lintProject = Join-Path $root "build/api24-lint-$stamp"
$lintSource = Join-Path $lintProject 'src'
New-Item -ItemType Directory -Force -Path $lintSource | Out-Null
Copy-Item -LiteralPath (Join-Path $project 'app/src/main/java/dev') `
    -Destination $lintSource -Recurse -Force
$manifestSource = Join-Path $project 'app/src/main/AndroidManifest.xml'
$manifestText = [IO.File]::ReadAllText($manifestSource)
$manifestText = $manifestText.Replace(
    '    package="dev.a9tas.android">',
    "    package=`"dev.a9tas.android`">`r`n" +
    '    <uses-sdk android:minSdkVersion="24" android:targetSdkVersion="35" />')
[IO.File]::WriteAllText((Join-Path $lintProject 'AndroidManifest.xml'), $manifestText,
    [Text.UTF8Encoding]::new($false))
$sources = @(Get-ChildItem -LiteralPath (Join-Path $project 'app/src/main/java'),$generated `
    -Filter '*.java' -File -Recurse | Select-Object -ExpandProperty FullName)
if ($sources.Count -eq 0) { throw 'No Java sources found' }
$javacArgs = @('-encoding','UTF-8','-source','17','-target','17',
    '-classpath',$androidJar,'-d',$classes)
& $javac @javacArgs @sources
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

$lintClasses = Join-Path $lintProject 'bin/classes'
New-Item -ItemType Directory -Force -Path $lintClasses | Out-Null
Copy-Item -LiteralPath (Join-Path $classes 'dev') -Destination $lintClasses `
    -Recurse -Force
& $lint --check NewApi --exitcode --sdk-home $sdk --compile-sdk-version 35 `
    --java-language-level 17 $lintProject
if ($LASTEXITCODE -ne 0) { throw 'Android API-24 NewApi lint failed' }

$classesJar = Join-Path $work 'classes.jar'
& $jar cf $classesJar -C $classes .
if ($LASTEXITCODE -ne 0) { throw 'Java class archive failed' }
# Pass one archive to d8. Expanding every inner/anonymous class can exceed the
# Windows command-line limit as the product UI grows.
& $d8 --lib $androidJar --min-api 24 --output $dex $classesJar
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }
& $jar uf $unsignedApk -C $dex classes.dex
if ($LASTEXITCODE -ne 0) { throw 'classes.dex packaging failed' }
$stageArgs = @('-B', (Join-Path $root 'tools/stage_android_assets_v1.py'),
    (Join-Path $project 'app/src/main/assets'), $work)
if ($NoBuiltInProfiles) { $stageArgs += '--no-profiles' }
$assetPackageRoot = & python @stageArgs
if ($LASTEXITCODE -ne 0) { throw 'Manifest-based asset staging failed' }
& $jar uf $unsignedApk -C $assetPackageRoot assets
if ($LASTEXITCODE -ne 0) { throw 'asset packaging failed' }

$keystore = Join-Path $project 'debug.keystore'
$signerLabel = 'debug'
if ($SigningMode -eq 'Release') {
    $keystore = $releaseKeystoreResolved
    & $apksigner sign --ks $keystore --ks-key-alias $ReleaseKeyAlias `
        --ks-pass env:A9TAS_RELEASE_STORE_PASSWORD `
        --key-pass env:A9TAS_RELEASE_KEY_PASSWORD --out $signedApk $unsignedApk
    $signerLabel = 'release'
} else {
    if (-not (Test-Path -LiteralPath $keystore -PathType Leaf)) {
        & (Join-Path $env:JAVA_HOME 'bin/keytool.exe') -genkeypair -noprompt `
            -keystore $keystore -storepass android -keypass android `
            -alias androiddebugkey -dname 'CN=Android Debug,O=Android,C=US' `
            -keyalg RSA -keysize 2048 -validity 10000
        if ($LASTEXITCODE -ne 0) { throw 'debug signing key generation failed' }
    }
    & $apksigner sign --ks $keystore --ks-pass pass:android --key-pass pass:android `
        --out $signedApk $unsignedApk
}
if ($LASTEXITCODE -ne 0) { throw 'APK signing failed' }
$verifyOutput = @(& $apksigner verify --verbose --print-certs $signedApk 2>&1)
$verifyExit = $LASTEXITCODE
$verifyOutput | Out-Host
if ($verifyExit -ne 0) { throw 'APK signature verification failed' }
if ($SigningMode -eq 'Release' -and
    ($verifyOutput -join "`n") -match 'CN=Android Debug') {
    throw 'Release artifact is signed by a debug certificate.'
}
$signerDigestLine = $verifyOutput | Where-Object {
    $_ -match '^Signer #1 certificate SHA-256 digest:'
} | Select-Object -First 1
if ($null -eq $signerDigestLine) { throw 'Signer certificate digest is unavailable.' }
$signerDigest = (($signerDigestLine -split ':',2)[1]).Trim().ToLowerInvariant()
if ($signerDigest -notmatch '^[0-9a-f]{64}$') { throw 'Signer certificate digest is invalid.' }

$apk = if ($NoBuiltInProfiles) {
    Join-Path $project 'app/build/outputs/apk/debug/a9tas-0.8.0-profile-autogen-empty-devtest.apk'
} elseif ($DeveloperTest) {
    Join-Path $project 'app/build/outputs/apk/debug/a9tas-0.8.0-profile-autogen-devtest.apk'
} elseif ($SigningMode -eq 'Release') {
        Join-Path $project 'app/build/outputs/apk/release/a9tas-0.8.0-profile-autogen.apk'
} else {
    Join-Path $project 'app/build/outputs/apk/debug/app-debug.apk'
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $apk) | Out-Null
Copy-Item -LiteralPath $signedApk -Destination $apk -Force
$entries = @(& $jar tf $apk)
$requiredEntries = @(
    'classes.dex',
    'assets/runtime/manifest.json',
    'assets/runtime/liba9tas_g4_multi_hook_runtime_v1.so',
    'assets/runtime/a9tas_native_arm64_early_loader_v1',
    'assets/runtime/a9tas_native_arm64_g4_input_action_controller_v1',
    'assets/runtime/a9tas_native_arm64_g8_profile_physics_interval_readonly_observer_v1',
    'assets/runtime/a9tas_identity_probe_arm64_v1',
    'assets/runtime/a9tas_identity_probe_x86_64_v1',
    'assets/runtime/a9tas_practice_mode_readonly_probe_arm64_v1',
    'assets/runtime/a9tas_practice_mode_readonly_probe_x86_64_v1',
    'assets/runtime/a9tas_arm64_profile_autogen_v1',
    'assets/profiles/registry.json'
)
if (-not $NoBuiltInProfiles) {
    $requiredEntries += 'assets/profiles/439fd7f94ef570d9.a9profile.bin'
}
foreach ($entry in $requiredEntries) {
    if ($entries -notcontains $entry) { throw "APK entry missing: $entry" }
}
if (@($entries | Where-Object { $_ -match '\\' }).Count -ne 0) {
    throw 'APK contains non-portable backslash entry names'
}
if ($NoBuiltInProfiles) {
    $embeddedProfiles = @($entries | Where-Object {
        $_ -match '^assets/profiles/.+[.]a9profile[.]bin$'
    })
    if ($embeddedProfiles.Count -ne 0) {
        throw 'NoBuiltInProfiles APK unexpectedly contains a game BuildProfile.'
    }
    $verifyAssets = Join-Path $work 'verify-assets'
    New-Item -ItemType Directory -Force -Path $verifyAssets | Out-Null
    Push-Location $verifyAssets
    try {
        & $jar xf $apk 'assets/profiles/registry.json'
        if ($LASTEXITCODE -ne 0) { throw 'Profile registry extraction failed.' }
    } finally {
        Pop-Location
    }
    $packagedRegistry = Get-Content -Raw -LiteralPath (
        Join-Path $verifyAssets 'assets/profiles/registry.json') | ConvertFrom-Json
    if ($packagedRegistry.schema -ne 1 -or @($packagedRegistry.profiles).Count -ne 0) {
        throw 'NoBuiltInProfiles APK registry is not empty.'
    }
}
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $apk).Hash.ToLowerInvariant()
Write-Output "G10_ANDROID_PRODUCT_BUILD passed=1 apk=$apk sha256=$hash " +
    "policy_only=0 compiler=javac17 packager=aapt2 dexer=d8 signer=$signerLabel " +
    "signer_cert_sha256=$signerDigest developer_test=$(if ($DeveloperTest) { 1 } else { 0 }) " +
    "built_in_profiles=$(if ($NoBuiltInProfiles) { 0 } else { 1 })"
