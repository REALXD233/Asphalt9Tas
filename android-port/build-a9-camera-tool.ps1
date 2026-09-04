param(
    [ValidateSet('Build','Publish')]
    [string]$Mode = 'Publish'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$workspace = Split-Path -Parent $root
$dotnet = Join-Path $workspace '.toolchains\dotnet\dotnet.exe'
$project = Join-Path $root 'A9CameraTool\A9CameraTool.csproj'
$nugetConfig = Join-Path $root 'A9CameraTool\NuGet.Config'
$output = Join-Path $root 'build\A9CameraTool'
$cliAppData = Join-Path $root 'build\.dotnet-appdata'
$packageCache = Join-Path $workspace '.nuget\packages'

# Keep all CLI state inside the workspace. This makes the build reproducible in
# restricted environments and prevents accidental dependence on a user-wide
# NuGet configuration.
$env:DOTNET_CLI_HOME = $workspace
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:APPDATA = $cliAppData
$env:NUGET_PACKAGES = $packageCache
New-Item -ItemType Directory -Force -Path $cliAppData, $packageCache | Out-Null

if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) {
    throw "Project-local .NET SDK is missing: $dotnet"
}
if (-not (Test-Path -LiteralPath $project -PathType Leaf)) {
    throw "A9 Camera Tool project is missing: $project"
}
if (-not (Test-Path -LiteralPath $nugetConfig -PathType Leaf)) {
    throw "Project-local NuGet configuration is missing: $nugetConfig"
}

if ($Mode -eq 'Build') {
    & $dotnet build $project --configuration Release --configfile $nugetConfig `
        --ignore-failed-sources -p:NuGetAudit=false --nologo
} else {
    & $dotnet publish $project --configuration Release --runtime win-x64 `
        --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
        --configfile $nugetConfig -p:NuGetAudit=false --output $output --nologo
}
if ($LASTEXITCODE -ne 0) { throw "A9 Camera Tool $Mode failed" }

$exe = Join-Path $output 'A9 Camera Tool.exe'
if ($Mode -eq 'Publish') {
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "Published executable missing: $exe"
    }
    $hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Output "A9_CAMERA_TOOL_PUBLISHED path=$exe sha256=$hash"
}
