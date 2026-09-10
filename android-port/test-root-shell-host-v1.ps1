$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$javac = Get-ChildItem -LiteralPath (Join-Path $root '.toolchains/jdk17') -Filter javac.exe -Recurse -File | Select-Object -First 1
$java = Join-Path (Split-Path -Parent $javac.FullName) 'java.exe'
$out = Join-Path $root ('build/root-shell-host/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sources = @(Get-ChildItem (Join-Path $root 'tests/root-shell') -Recurse -Filter '*.java' | ForEach-Object FullName)
$sources += Join-Path $root 'A9TasAndroid/app/src/main/java/dev/a9tas/android/RootShell.java'
& $javac.FullName -encoding UTF-8 -d $out @sources
if ($LASTEXITCODE -ne 0) { throw 'Root shell host compilation failed' }
& $java -ea -cp $out dev.a9tas.android.RootShellRegression
if ($LASTEXITCODE -ne 0) { throw 'Root shell host regression failed' }
