[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PortableDirectory,
    [string]$OutputDirectory = 'out/packages/release',
    [string]$InnoCompiler = '',
    [string]$Version = '',
    [string]$RuntimeLockFile = '',
    [string]$SourceCommit = '',
    [switch]$InstallerTestIdentity
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$portable = (Resolve-Path -LiteralPath $PortableDirectory).Path
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
}
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'out\packages'))
$allowedPrefix = $allowedRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must stay below $allowedRoot"
}

$compiler = if ([string]::IsNullOrWhiteSpace($InnoCompiler)) {
    Join-Path $projectRoot '.tools\inno-setup\7.1.0\ISCC.exe'
} else {
    [IO.Path]::GetFullPath($InnoCompiler)
}
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw "Inno Setup compiler is missing: $compiler"
}
$cmakeText = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch '(?m)^project\(VO_VE VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES') {
    throw 'Could not read the VO-VE version from CMakeLists.txt'
}
$projectVersion = $Matches[1]
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = $projectVersion
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Version must be strict SemVer: $Version"
}
if ($Version -cne $projectVersion) {
    throw "CMake project version does not match $Version"
}

$artworkRoot = Join-Path $projectRoot 'packaging\windows\artwork'
$artworkManifestPath = Join-Path $artworkRoot 'installer-artwork-manifest.json'
$artworkManifest = Get-Content -LiteralPath $artworkManifestPath -Raw | ConvertFrom-Json
if ($artworkManifest.schema -ne 1 -or $artworkManifest.version -ne $Version) {
    throw "Installer artwork manifest does not match version $Version"
}
$generatorPath = Join-Path $projectRoot $artworkManifest.generator.path
if (-not (Test-Path -LiteralPath $generatorPath -PathType Leaf) -or
    (Get-FileHash -LiteralPath $generatorPath -Algorithm SHA256).Hash -ne
        $artworkManifest.generator.sha256) {
    throw 'Installer artwork generator differs from its manifest'
}
foreach ($input in $artworkManifest.inputs) {
    $path = Join-Path $projectRoot $input.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $input.sha256) {
        throw "Installer artwork input differs from its manifest: $($input.path)"
    }
}
foreach ($file in $artworkManifest.files) {
    $path = Join-Path $artworkRoot $file.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-Item -LiteralPath $path).Length -ne $file.bytes -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Installer artwork differs from its manifest: $($file.path)"
    }
}

$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'packaging\windows-runtime-manifest.json') -Raw |
    ConvertFrom-Json
$expected = @($manifest.files.path | ForEach-Object { ([string]$_).Replace('\', '/') } | Sort-Object)
$actual = @(Get-ChildItem -LiteralPath $portable -File -Recurse |
    ForEach-Object { [IO.Path]::GetRelativePath($portable, $_.FullName).Replace('\', '/') } |
    Sort-Object)
if (Compare-Object $expected $actual) {
    throw 'PortableDirectory differs from the strict Windows runtime manifest'
}

if ([string]::IsNullOrWhiteSpace($RuntimeLockFile)) {
    $RuntimeLockFile = "$portable.lock.json"
}
$runtimeLock = (Resolve-Path -LiteralPath $RuntimeLockFile).Path
$locked = Get-Content -LiteralPath $runtimeLock -Raw | ConvertFrom-Json
if ($locked.schema -ne 1 -or $locked.platform -ne 'windows-x64' -or
    (Compare-Object $expected @($locked.files.path | Sort-Object))) {
    throw 'Runtime lock differs from the strict Windows runtime manifest'
}
foreach ($file in $locked.files) {
    $path = Join-Path $portable $file.path
    if ((Get-Item -LiteralPath $path).Length -ne $file.bytes -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Runtime file differs from its lock: $($file.path)"
    }
}
$runtimeLockHash = (Get-FileHash -LiteralPath $runtimeLock -Algorithm SHA256).Hash
if ([string]::IsNullOrWhiteSpace($SourceCommit)) {
    $SourceCommit = (& git -C $projectRoot rev-parse --verify HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { throw 'Provide SourceCommit when packaging without Git' }
    $SourceCommit = $SourceCommit.Trim()
}
if ($SourceCommit -notmatch '^[a-fA-F0-9]{40}$') {
    throw 'SourceCommit must identify the verified source revision'
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
$script = Join-Path $projectRoot 'packaging\windows\vo-ve.iss'
$compilerArguments = @(
    "/DSourceDir=$portable",
    "/DOutputDir=$output",
    "/DAppVersion=$Version"
)
if ($InstallerTestIdentity) {
    $compilerArguments += '/DInstallerTestIdentity=1'
}
& $compiler @compilerArguments $script
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compiler failed with exit code $LASTEXITCODE"
}

$installer = Join-Path $output "VO-VE-$Version-windows-x64-setup.exe"
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Installer was not produced: $installer"
}
$result = [ordered]@{
    schema = 1
    version = $Version
    platform = 'windows-x64'
    compiler = 'Inno Setup 7.1.0 x64'
    installer_test_identity = [bool]$InstallerTestIdentity
    source_commit = $SourceCommit
    runtime_lock_sha256 = $runtimeLockHash
    bytes = (Get-Item -LiteralPath $installer).Length
    sha256 = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
    source_manifest_sha256 = (Get-FileHash -LiteralPath (Join-Path $projectRoot 'packaging\windows-runtime-manifest.json') -Algorithm SHA256).Hash
}
$resultPath = "$installer.lock.json"
$result | ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding utf8NoBOM
$result | ConvertTo-Json
