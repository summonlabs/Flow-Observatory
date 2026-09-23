# Flow Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#
# Convenience wrapper that locates a Visual Studio toolchain (via vswhere, which
# ships with every VS 2017+ installation) and runs CMake with Ninja. No machine
# specific path is encoded anywhere: everything is discovered at run time.
#
# Usage:
#   pwsh -File scripts/build.ps1 -Preset Release
#   pwsh -File scripts/build.ps1 -Preset Debug -Asan -Test
[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
  [string]$Preset = 'Release',
  [string]$BuildDir = '',
  [switch]$Asan,
  [switch]$Test,
  [switch]$Bench,
  [switch]$Clean,
  [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($BuildDir)) {
  $suffix = if ($Asan) { '-asan' } else { '' }
  $BuildDir = Join-Path $repoRoot ('build/' + $Preset.ToLower() + $suffix)
}

function Get-VsInstallPath {
  $x86 = [Environment]::GetFolderPath('ProgramFilesX86')
  $vswhere = Join-Path $x86 'Microsoft Visual Studio/Installer/vswhere.exe'
  if (-not (Test-Path $vswhere)) { return $null }
  $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ([string]::IsNullOrEmpty($install)) { return $null }
  return $install.Trim()
}

$vsInstall = Get-VsInstallPath
if ($vsInstall) {
  $devShell = Join-Path $vsInstall 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
  if (Test-Path $devShell) {
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vsInstall -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
  }
}

if ($Clean -and (Test-Path $BuildDir)) {
  Remove-Item -Recurse -Force $BuildDir
}

$configureArgs = @(
  '-S', $repoRoot,
  '-B', $BuildDir,
  '-G', 'Ninja',
  '-DCMAKE_BUILD_TYPE=' + $Preset,
  '-DFLOWOBS_BUILD_TESTS=ON',
  '-DFLOWOBS_BUILD_TOOLS=ON',
  '-DFLOWOBS_BUILD_EXAMPLES=ON',
  '-DFLOWOBS_WARNINGS_AS_ERRORS=ON'
)
if ($Bench) { $configureArgs += '-DFLOWOBS_BUILD_BENCHMARKS=ON' } else { $configureArgs += '-DFLOWOBS_BUILD_BENCHMARKS=OFF' }
if ($Asan) { $configureArgs += '-DFLOWOBS_ENABLE_ASAN=ON' }
if ($ExtraArgs.Count -gt 0) { $configureArgs += $ExtraArgs }

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

& cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }

if ($Test) {
  & ctest --test-dir $BuildDir --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw 'ctest failed' }
}

Write-Host ('build directory: ' + $BuildDir)
