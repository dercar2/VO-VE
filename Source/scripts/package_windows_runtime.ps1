[CmdletBinding()]
param(
    [string]$BuildDirectory = 'out/build/autonomous-release2',
    [string]$OutputDirectory = 'out/packages/vove-windows-x64',
    [string]$QtRoot = '',
    [string]$LlvmBin = '',
    [bool]$StripBinaries = $true,
    [switch]$CreateZip
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot 'out\packages'))
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
}
$allowedPrefix = $allowedRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must stay below $allowedRoot"
}

$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
}
$qt = if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    Join-Path $projectRoot '.tools\qt\6.10.3\llvm-mingw_64'
} else {
    [IO.Path]::GetFullPath($QtRoot)
}
$llvm = if ([string]::IsNullOrWhiteSpace($LlvmBin)) {
    Join-Path $projectRoot '.tools\llvm-mingw\llvm-mingw-20260407-ucrt-x86_64\bin'
} else {
    [IO.Path]::GetFullPath($LlvmBin)
}
$manifestPath = Join-Path $projectRoot 'packaging\windows-runtime-manifest.json'
$stripTool = Join-Path $llvm 'llvm-strip.exe'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.platform -ne 'windows-x64') {
    throw 'Unsupported Windows runtime manifest'
}
$stripRequired = $StripBinaries -and @($manifest.files | Where-Object {
    -not [string]::IsNullOrWhiteSpace([string]$_.strip)
}).Count -ne 0
if ($stripRequired -and -not (Test-Path -LiteralPath $stripTool -PathType Leaf)) {
    throw "llvm-strip is missing: $stripTool"
}

if (Test-Path -LiteralPath $output) {
    Remove-Item -LiteralPath $output -Recurse -Force
}
New-Item -ItemType Directory -Path $output | Out-Null

$tokens = @{
    '{build}' = $build
    '{qt}' = $qt
    '{llvm}' = $llvm
    '{project}' = $projectRoot
}
$lockFiles = @()
$totalBytes = [long]0
foreach ($file in $manifest.files) {
    if ([string]::IsNullOrWhiteSpace($file.owner) -or
        [string]::IsNullOrWhiteSpace($file.reason)) {
        throw "Manifest entry lacks owner or reason: $($file.path)"
    }
    $source = [string]$file.source
    foreach ($token in $tokens.Keys) {
        $source = $source.Replace($token, $tokens[$token])
    }
    $source = [IO.Path]::GetFullPath($source)
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required runtime file is missing: $source"
    }
    $relative = ([string]$file.path).Replace('/', [IO.Path]::DirectorySeparatorChar)
    $destination = Join-Path $output $relative
    $destinationParent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
    if ($StripBinaries -and -not [string]::IsNullOrWhiteSpace([string]$file.strip)) {
        $stripArgument = switch ([string]$file.strip) {
            'all' { '--strip-all' }
            'unneeded' { '--strip-unneeded' }
            default { throw "Unsupported strip mode '$($file.strip)' for $($file.path)" }
        }
        & $stripTool $stripArgument $destination
        if ($LASTEXITCODE -ne 0) {
            throw "llvm-strip failed for $destination with exit code $LASTEXITCODE"
        }
    }
    if ([IO.Path]::GetFileName($destination) -ieq 'vove-fileop-helper.exe') {
        $helperBytes = [IO.File]::ReadAllBytes($destination)
        $helperAscii = [Text.Encoding]::ASCII.GetString($helperBytes)
        $helperUtf16 = [Text.Encoding]::Unicode.GetString($helperBytes)
        if ($helperAscii.Contains('VOVE_TEST_TRANSFER_') -or
            $helperUtf16.Contains('VOVE_TEST_TRANSFER_')) {
            throw 'Packaged file-operation helper contains fault-injection hooks'
        }
    }
    $item = Get-Item -LiteralPath $destination
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $totalBytes += [long]$item.Length
    $lockFiles += [ordered]@{
        path = ([string]$file.path).Replace('\', '/')
        bytes = [long]$item.Length
        sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        source_sha256 = $sourceHash
        transform = if ($StripBinaries -and
            -not [string]::IsNullOrWhiteSpace([string]$file.strip)) {
            "strip-$([string]$file.strip)"
        } else {
            'copy-exact'
        }
        owner = [string]$file.owner
        reason = [string]$file.reason
    }
}

$actual = Get-ChildItem -LiteralPath $output -Recurse -File |
    ForEach-Object { [IO.Path]::GetRelativePath($output, $_.FullName).Replace('\', '/') } |
    Sort-Object
$expected = $manifest.files.path | ForEach-Object { ([string]$_).Replace('\', '/') } | Sort-Object
if (Compare-Object $expected $actual) {
    throw 'Packaged runtime differs from the strict manifest allowlist'
}

$lock = [ordered]@{
    schema = 1
    platform = 'windows-x64'
    manifest_sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    total_bytes = $totalBytes
    files = $lockFiles
}
$lockPath = "$output.lock.json"
$lock | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $lockPath -Encoding utf8NoBOM

if ($CreateZip) {
    $zipPath = "$output.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    $zipStream = [IO.File]::Open($zipPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    $archive = $null
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $zipStream, [IO.Compression.ZipArchiveMode]::Create, $false)
        $fixedTimestamp = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        foreach ($relative in $actual) {
            $source = Join-Path $output $relative.Replace('/', [IO.Path]::DirectorySeparatorChar)
            $entry = $archive.CreateEntry(
                $relative, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = $fixedTimestamp
            $entry.ExternalAttributes = 0
            $input = [IO.File]::OpenRead($source)
            $destination = $entry.Open()
            try {
                $input.CopyTo($destination)
            } finally {
                $destination.Dispose()
                $input.Dispose()
            }
        }
    } finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        } else {
            $zipStream.Dispose()
        }
    }
    Write-Output "ZIP_SHA256=$((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash)"
}

Write-Output "PACKAGE_DIRECTORY=$output"
Write-Output "PACKAGE_FILES=$($lockFiles.Count)"
Write-Output "PACKAGE_BYTES=$($lock.total_bytes)"
Write-Output "PACKAGE_LOCK=$lockPath"
