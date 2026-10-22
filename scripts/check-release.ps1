#Requires -Version 7.0
<#
.SYNOPSIS
  Validate the Release build and publish the verified release package.

.DESCRIPTION
  The default sequence configures and builds the Release preset, runs CTest and
  the extension tests, exercises desktop/bridge lifecycle recovery, and checks
  the final executable's imports, then runs package-release.ps1 to synchronize
  dist/ and Glint-win-x64.zip. Requires PowerShell 7 or later, including the
  runtime APIs used by packaging.

  Skipping any validation check requires -SkipPackage and reports a focused
  result. -SkipConfigure and -SkipBuild may reuse an existing Release build
  while all validation checks rerun before publication.

.PARAMETER Binary
  Defaults to build\release\Release\Glint.exe, the Release preset's artifact.
  A different executable is supported only for focused checks with -SkipBuild,
  -SkipCtest, and -SkipPackage: preset build/CTest results cannot validate it.

.PARAMETER ReportPath
  Lifecycle smoke report path. Defaults to
  build\diagnostics\desktop-lifecycle-smoke.json.

.PARAMETER SkipConfigure
  Skip `cmake --preset release`.

.PARAMETER SkipBuild
  Skip `cmake --build --preset release --parallel`.

.PARAMETER SkipCtest
  Skip `ctest --preset release --output-on-failure --no-tests=error`.
  Requires -SkipPackage.

.PARAMETER SkipExtensionTests
  Skip `npm --prefix extension test`. Requires -SkipPackage.

.PARAMETER SkipLifecycle
  Skip the desktop/bridge lifecycle smoke test. Requires -SkipPackage.

.PARAMETER SkipImports
  Skip the native executable import check. Requires -SkipPackage.

.PARAMETER SkipPackage
  Skip publication of the validated executable and package.
#>

[CmdletBinding()]
param(
  [string]$Binary,
  [string]$ReportPath,
  [switch]$SkipConfigure,
  [switch]$SkipBuild,
  [switch]$SkipCtest,
  [switch]$SkipExtensionTests,
  [switch]$SkipLifecycle,
  [switch]$SkipImports,
  [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'
$repositoryDirectory = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$defaultBinary = Join-Path $repositoryDirectory 'build\release\Release\Glint.exe'
$defaultReport = Join-Path $repositoryDirectory 'build\diagnostics\desktop-lifecycle-smoke.json'

if (-not $Binary) { $Binary = $defaultBinary }
if (-not $ReportPath) { $ReportPath = $defaultReport }

function Resolve-RepositoryPath([string]$Path) {
  if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
  return [IO.Path]::GetFullPath((Join-Path $repositoryDirectory $Path))
}

$binaryPath = Resolve-RepositoryPath $Binary
$reportPath = Resolve-RepositoryPath $ReportPath
$lifecycleScript = Join-Path $repositoryDirectory 'scripts\test-desktop-lifecycle.ps1'
$importsScript = Join-Path $repositoryDirectory 'scripts\check-imports.ps1'
$packageScript = Join-Path $repositoryDirectory 'scripts\package-release.ps1'

$skippedChecks = @(
  if ($SkipCtest) { 'CTest' }
  if ($SkipExtensionTests) { 'extension tests' }
  if ($SkipLifecycle) { 'lifecycle smoke' }
  if ($SkipImports) { 'import check' }
)
if ($skippedChecks.Count -gt 0 -and -not $SkipPackage) {
  throw "Skipping Release validation ($($skippedChecks -join ', ')) requires -SkipPackage. Focused runs cannot publish a validated release."
}
if (-not [StringComparer]::OrdinalIgnoreCase.Equals($binaryPath, $defaultBinary) -and
    (-not $SkipBuild -or -not $SkipCtest -or -not $SkipPackage)) {
  throw 'A custom -Binary requires -SkipBuild, -SkipCtest, and -SkipPackage. Release preset build/CTest results validate only build\release\Release\Glint.exe.'
}

function Invoke-NativeCommand {
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [Parameter(Mandatory = $true)][string[]]$Arguments
  )

  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($Arguments -join ' ')"
  }
}

function Invoke-PowerShellScript {
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [hashtable]$Arguments = @{}
  )

  # Successful PowerShell scripts need not set LASTEXITCODE. Start a new status
  # for each invocation, while still detecting scripts that explicitly exit.
  $LASTEXITCODE = 0
  & $FilePath @Arguments
  if (-not $? -or $LASTEXITCODE -ne 0) {
    throw "PowerShell script failed with exit code ${LASTEXITCODE}: $FilePath"
  }
}

function Invoke-ReleaseStep {
  param(
    [Parameter(Mandatory = $true)][int]$Index,
    [Parameter(Mandatory = $true)][int]$Total,
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][scriptblock]$Action
  )

  Write-Output ("[{0}/{1}] {2}" -f $Index, $Total, $Name)
  $clock = [Diagnostics.Stopwatch]::StartNew()
  try {
    & $Action
    Write-Output ("[{0}/{1}] PASS {2} ({3:N1}s)" -f $Index, $Total, $Name, $clock.Elapsed.TotalSeconds)
  } catch {
    throw ("[{0}/{1}] FAIL {2} ({3:N1}s): {4}" -f $Index, $Total, $Name, $clock.Elapsed.TotalSeconds, $_.Exception.Message)
  } finally {
    $clock.Stop()
  }
}

function Assert-BinaryAvailable {
  if (-not [IO.File]::Exists($binaryPath)) {
    throw "Release executable was not found: $binaryPath. Build it first or pass -Binary to an existing Glint.exe."
  }
}

$steps = [System.Collections.Generic.List[object]]::new()
if (-not $SkipConfigure) {
  $steps.Add([pscustomobject]@{
      Name = 'Configure Release x64'
      Action = { Invoke-NativeCommand -FilePath 'cmake' -Arguments @('--preset', 'release') }
    })
}
if (-not $SkipBuild) {
  $steps.Add([pscustomobject]@{
      Name = 'Build Release x64'
      Action = { Invoke-NativeCommand -FilePath 'cmake' -Arguments @('--build', '--preset', 'release', '--parallel') }
    })
}
if (-not $SkipCtest) {
  $steps.Add([pscustomobject]@{
      Name = 'Run CTest Release suite'
      Action = { Invoke-NativeCommand -FilePath 'ctest' -Arguments @('--preset', 'release', '--output-on-failure', '--no-tests=error') }
    })
}
if (-not $SkipExtensionTests) {
  $steps.Add([pscustomobject]@{
      Name = 'Run browser extension tests'
      Action = { Invoke-NativeCommand -FilePath 'npm' -Arguments @('--prefix', 'extension', 'test') }
    })
}
if (-not $SkipLifecycle) {
  $steps.Add([pscustomobject]@{
      Name = 'Verify desktop and bridge lifecycle'
      Action = {
        Assert-BinaryAvailable
        Invoke-PowerShellScript -FilePath $lifecycleScript -Arguments @{ Binary = $binaryPath; ReportPath = $reportPath }
      }
    })
}
if (-not $SkipImports) {
  $steps.Add([pscustomobject]@{
      Name = 'Check executable imports'
      Action = {
        Assert-BinaryAvailable
        Invoke-PowerShellScript -FilePath $importsScript -Arguments @{ Binary = $binaryPath }
      }
    })
}
if (-not $SkipPackage) {
  $steps.Add([pscustomobject]@{
      Name = 'Package and verify Release artifacts'
      Action = {
        Assert-BinaryAvailable
        Invoke-PowerShellScript -FilePath $packageScript -Arguments @{ Binary = $binaryPath }
      }
    })
}

if ($steps.Count -eq 0) {
  throw 'No Release checks selected. Remove at least one skip switch.'
}

$location = Get-Location
try {
  Set-Location -LiteralPath $repositoryDirectory
  for ($index = 0; $index -lt $steps.Count; $index++) {
    $step = $steps[$index]
    Invoke-ReleaseStep -Index ($index + 1) -Total $steps.Count -Name $step.Name -Action $step.Action
  }
  if ($skippedChecks.Count -gt 0) {
    Write-Output ("Focused Release checks passed: {0} step(s). Skipped validation: {1}; packaging skipped." -f $steps.Count, ($skippedChecks -join ', '))
  } elseif ($SkipPackage) {
    Write-Output ("Release validation passed: {0} step(s); packaging skipped." -f $steps.Count)
  } else {
    Write-Output ("Release validation and packaging passed: {0} step(s)." -f $steps.Count)
  }
} finally {
  Set-Location -LiteralPath $location
}
