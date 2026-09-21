[CmdletBinding()]
param(
    [string]$RepositoryRoot = '',
    [string]$ToolRoot = '',
    [switch]$Offline
)

$ErrorActionPreference = 'Stop'
$projectSourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = $projectSourceRoot
}
$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path

if ([string]::IsNullOrWhiteSpace($ToolRoot)) {
    $ToolRoot = Join-Path $RepositoryRoot '.tools'
}
if (-not (Test-Path -LiteralPath $ToolRoot -PathType Container)) {
    throw 'Source-local .tools is missing; provide ToolRoot explicitly'
}
$ToolRoot = (Resolve-Path -LiteralPath $ToolRoot).Path

$bridge = Join-Path $RepositoryRoot 'third_party\resvg_bridge'
$toolchainBin = Join-Path $ToolRoot `
    'rust\rustup\toolchains\1.98.0-x86_64-pc-windows-gnullvm\bin'
$cargo = Join-Path $toolchainBin 'cargo.exe'
$rustc = Join-Path $toolchainBin 'rustc.exe'
$llvmBin = Join-Path $ToolRoot 'llvm-mingw\llvm-mingw-20260407-ucrt-x86_64\bin'
$linker = Join-Path $llvmBin 'x86_64-w64-mingw32-clang.exe'
foreach ($required in @($bridge, $cargo, $rustc, $linker)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required resvg build input is missing: $required"
    }
}

if ((& $rustc --version) -notmatch '^rustc 1\.98\.0\b') {
    throw 'The pinned resvg bridge requires Rust 1.98.0'
}

$oldPath = $env:PATH
$oldLinker = $env:CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_LINKER
$oldCargoHome = $env:CARGO_HOME
$oldRustupHome = $env:RUSTUP_HOME
try {
    $env:PATH = "$toolchainBin;$llvmBin;$oldPath"
    $env:CARGO_HOME = Join-Path $ToolRoot 'rust\cargo'
    $env:RUSTUP_HOME = Join-Path $ToolRoot 'rust\rustup'
    $env:CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_LINKER = $linker
    $common = @('--manifest-path', (Join-Path $bridge 'Cargo.toml'), '--locked',
                '--target', 'x86_64-pc-windows-gnullvm')
    if ($Offline) {
        $common += '--offline'
    }
    & $cargo test @common
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cargo build @common --profile production
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    $env:PATH = $oldPath
    $env:CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_LINKER = $oldLinker
    $env:CARGO_HOME = $oldCargoHome
    $env:RUSTUP_HOME = $oldRustupHome
}

$library = Join-Path $bridge `
    'target\x86_64-pc-windows-gnullvm\production\libvove_resvg_bridge.a'
if (-not (Test-Path -LiteralPath $library -PathType Leaf)) {
    throw "resvg bridge library was not produced: $library"
}
Write-Output "RESVG_BRIDGE_LIBRARY=$library"
Write-Output "RESVG_BRIDGE_BYTES=$((Get-Item -LiteralPath $library).Length)"
Write-Output "RESVG_BRIDGE_SHA256=$((Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash)"
