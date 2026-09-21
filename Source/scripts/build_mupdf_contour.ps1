[CmdletBinding()]
param(
    [string]$RepositoryRoot = '',
    [string]$SourceArchive = '',
    [string]$OutputDirectory = '',
    [string]$MakePath = '',
    [string]$LlvmBin = '',
    [string]$CMakePath = '',
    [string]$MakeShell = 'C:/Progra~1/Git/bin/sh.exe',
    [ValidatePattern('^[A-Z]:$')]
    [string]$BuildDrive = 'V:'
)

$ErrorActionPreference = 'Stop'
$expectedArchive = '21C7F064903154F1C3A7458BEE81F130FC36F9B5147EA13328F9980E02D2DEA2'
$expectedLibrary = 'AF6AF9CFD09ABAFF312706E25FA318C2810B6CBB19A8F8D3725292749044ABA4'
$expectedThird = '4A4EA017E9EA7FECA645AC9F982FEC3D84C7AA3B18A8DB5A4163D22D07D6D001'
$projectSourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = $projectSourceRoot
}
$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
if ([string]::IsNullOrWhiteSpace($SourceArchive)) {
    $SourceArchive = Join-Path $RepositoryRoot '.tools\mupdf\1.28.0\mupdf-1.28.0-source.tar.gz'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepositoryRoot 'out\mupdf-contour-win'
}
if ([string]::IsNullOrWhiteSpace($MakePath)) {
    $MakePath = Join-Path $RepositoryRoot '.tools\make\4.4.1\package\tools\install\bin\make.exe'
}
if ([string]::IsNullOrWhiteSpace($LlvmBin)) {
    $LlvmBin = Join-Path $RepositoryRoot '.tools\llvm-mingw\llvm-mingw-20260407-ucrt-x86_64\bin'
}
if ([string]::IsNullOrWhiteSpace($CMakePath)) {
    $CMakePath = Join-Path $RepositoryRoot '.tools\cmake\cmake-4.3.4-windows-x86_64\bin\cmake.exe'
}
$verifyScript = Join-Path $projectSourceRoot 'cmake\VerifyMuPdfContour.cmake'

foreach ($required in @(
        $SourceArchive, $MakePath, $LlvmBin, $CMakePath, $MakeShell, $verifyScript)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required MuPDF contour input is missing: $required"
    }
}
$SourceArchive = (Resolve-Path -LiteralPath $SourceArchive).Path
$MakePath = (Resolve-Path -LiteralPath $MakePath).Path
$LlvmBin = (Resolve-Path -LiteralPath $LlvmBin).Path
$CMakePath = (Resolve-Path -LiteralPath $CMakePath).Path

function Assert-UnderRoot([string]$Path, [string]$Label) {
    if (-not $Path.StartsWith($RepositoryRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must stay below RepositoryRoot: $Path"
    }
}

$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    $OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
    if (Get-ChildItem -LiteralPath $OutputDirectory -Force | Select-Object -First 1) {
        throw "OutputDirectory must be new or empty: $OutputDirectory"
    }
} else {
    $parent = Split-Path -Parent $OutputDirectory
    if (-not (Test-Path -LiteralPath $parent)) {
        throw "OutputDirectory parent does not exist: $parent"
    }
    $OutputDirectory = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
}
Assert-UnderRoot $OutputDirectory 'OutputDirectory'

function Assert-Sha256([string]$Path, [string]$Expected, [string]$Label) {
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToUpperInvariant()
    if ($actual -ne $Expected) {
        throw "$Label hash mismatch. Expected $Expected, got ${actual}: $Path"
    }
}

Assert-Sha256 $SourceArchive $expectedArchive 'MuPDF source archive'
$sourceParent = New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'source')
Push-Location $sourceParent.FullName
try {
    & $CMakePath -E tar xzf $SourceArchive
    $extractExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($extractExitCode -ne 0) { throw 'Failed to extract the pinned MuPDF source archive' }
$SourceRoot = Join-Path $sourceParent.FullName 'mupdf-1.28.0-source'
$versionHeader = Join-Path $SourceRoot 'include\mupdf\fitz\version.h'
$versionMatches = Test-Path -LiteralPath $versionHeader -PathType Leaf
if ($versionMatches) {
    $versionMatches = Select-String -LiteralPath $versionHeader -Quiet `
        -Pattern '^#define\s+FZ_VERSION\s+"1\.28\.0"'
}
if (-not $versionMatches) {
    throw "Extracted MuPDF source version is not 1.28.0: $versionHeader"
}
$buildDirectory = (New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'build')).FullName

$clang = Join-Path $LlvmBin 'clang.exe'
$clangxx = Join-Path $LlvmBin 'clang++.exe'
$llvmAr = Join-Path $LlvmBin 'llvm-ar.exe'
$llvmRanlib = Join-Path $LlvmBin 'llvm-ranlib.exe'
foreach ($tool in @($clang, $clangxx, $llvmAr, $llvmRanlib)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "LLVM tool is missing: $tool" }
}
if ((& $clang --version | Select-Object -First 1) -notmatch '^clang version 22\.1\.3\b') {
    throw 'The pinned Windows contour requires clang 22.1.3'
}
if ((& $MakePath --version | Select-Object -First 1) -ne 'GNU Make 4.4.1') {
    throw 'The pinned Windows contour requires GNU Make 4.4.1'
}

$driveName = $BuildDrive.Substring(0, 1)
if (Get-PSDrive -Name $driveName -ErrorAction SilentlyContinue) {
    throw "BuildDrive is already in use: $BuildDrive"
}
$sourceRelative = $SourceRoot.Substring($RepositoryRoot.Length + 1).Replace('\', '/')
$buildRelative = $buildDirectory.Substring($RepositoryRoot.Length + 1).Replace('\', '/')
$oldPath = $env:PATH
& subst.exe $BuildDrive $RepositoryRoot
if ($LASTEXITCODE -ne 0) { throw "Failed to map $BuildDrive to $RepositoryRoot" }
try {
    $env:PATH = "$LlvmBin;$([System.IO.Path]::GetDirectoryName($MakePath));$oldPath"
    $arguments = @(
        '-C', "$BuildDrive/$sourceRelative", '-j', '4',
        "SHELL=$MakeShell",
        'build=small', 'shared=no', 'USE_SYSTEM_LIBS=no', 'mujs=no', 'html=no',
        'xps=no', 'svg=no', 'extract=no', 'brotli=no', 'tesseract=no', 'barcode=no',
        'archive=no', 'tofu=yes', 'tofu_cjk=yes', 'tofu_cjk_ext=yes',
        'tofu_cjk_lang=yes', 'HAVE_GLUT=no', 'HAVE_X11=no', 'HAVE_CURL=no',
        'HAVE_LIBCRYPTO=no', 'HAVE_OBJCOPY=no', 'threading=no',
        'XCFLAGS=-DARCH_HAS_SSE=0 -DFZ_ENABLE_HYPHEN=0 -DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0',
        'CC=clang', 'CXX=clang++', 'AR=llvm-ar', 'RANLIB=llvm-ranlib', 'LD=ld.lld',
        "OUT=$BuildDrive/$buildRelative", 'libs')
    & $MakePath @arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    $env:PATH = $oldPath
    & subst.exe $BuildDrive /D | Out-Null
}

$library = Join-Path $buildDirectory 'libmupdf.a'
$thirdLibrary = Join-Path $buildDirectory 'libmupdf-third.a'
Assert-Sha256 $library $expectedLibrary 'MuPDF library'
Assert-Sha256 $thirdLibrary $expectedThird 'MuPDF third-party library'
$verifyArguments = @(
    "-DVOVE_MUPDF_SOURCE_ROOT=$SourceRoot",
    "-DVOVE_MUPDF_SOURCE_ARCHIVE=$SourceArchive",
    "-DVOVE_MUPDF_LIBRARY=$library",
    "-DVOVE_MUPDF_THIRD_LIBRARY=$thirdLibrary",
    '-P', $verifyScript)
& $CMakePath @verifyArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Output "MUPDF_WORKSPACE=$OutputDirectory"
Write-Output "MUPDF_SOURCE_ROOT=$SourceRoot"
Write-Output "MUPDF_SOURCE_ARCHIVE=$SourceArchive"
Write-Output "MUPDF_LIBRARY=$library"
Write-Output "MUPDF_THIRD_LIBRARY=$thirdLibrary"
