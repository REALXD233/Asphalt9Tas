param([string]$Classes = '', [string]$JsonJar = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$jdk = Get-ChildItem -LiteralPath (Join-Path $root '.toolchains/jdk17') -Filter javac.exe -Recurse -File | Select-Object -First 1
$java = Join-Path (Split-Path -Parent $jdk.FullName) 'java.exe'
if (-not $Classes) {
    $latest = Get-ChildItem -LiteralPath (Join-Path $root 'A9TasAndroid/app/build/manual') -Directory |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $Classes = Join-Path $latest.FullName 'classes'
}
if (-not $JsonJar) { $JsonJar = Join-Path $root '.toolchains/test-libs/json-20240303.jar' }
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $JsonJar).Hash -ne
    '3CF6CD6892E32E2B4C1C39E0F52F5248A2F5B37646FDFBB79A66B46B618414ED') {
    throw 'Expected org.json:json:20240303 from Maven Central'
}
$android = Join-Path $root '.toolchains/android-sdk/platforms/android-35/android.jar'
$output = Join-Path $root ('build/library-host-test/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $output | Out-Null
$sources = @(Get-ChildItem -LiteralPath (Join-Path $root 'tests/host') -Filter '*.java' -Recurse -File |
    ForEach-Object FullName)
& $jdk.FullName -encoding UTF-8 -cp "$JsonJar;$android;$Classes" -d $output @sources
if ($LASTEXITCODE -ne 0) { throw 'Host test compilation failed' }
& $java -ea -cp "$output;$JsonJar;$Classes;$android" dev.a9tas.android.LibraryRegressionMain `
    (Join-Path $root 'A9TasAndroid/app/src/main/assets/selftest/a9tas1-synthetic.a9tas') `
    (Join-Path $output 'data')
if ($LASTEXITCODE -ne 0) { throw 'Production Java archive tests failed' }
