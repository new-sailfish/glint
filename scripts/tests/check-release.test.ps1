#Requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$gateSource = Join-Path (Split-Path -Parent $PSScriptRoot) 'check-release.ps1'
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureDirectory = Join-Path $temporaryParent ('glint-release-gate-' + [guid]::NewGuid().ToString('N'))
$originalPath = $env:PATH
$originalLocation = Get-Location
$originalLog = $env:GLINT_RELEASE_TEST_LOG
$originalFailure = $env:GLINT_RELEASE_TEST_FAIL
$originalFailureMode = $env:GLINT_RELEASE_TEST_MODE
$failures = [Collections.Generic.List[string]]::new()
$testCount = 0

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function New-Fixture {
  $root = Join-Path $fixtureDirectory ([guid]::NewGuid().ToString('N'))
  $scriptDirectory = Join-Path $root 'scripts'
  $toolDirectory = Join-Path $root 'tools'
  $binaryDirectory = Join-Path $root 'build/release/Release'
  New-Item -ItemType Directory -Path $scriptDirectory, $toolDirectory, $binaryDirectory | Out-Null
  Copy-Item -LiteralPath $gateSource -Destination (Join-Path $scriptDirectory 'check-release.ps1')
  Set-Content -LiteralPath (Join-Path $binaryDirectory 'Glint.exe') -Value 'fixture executable'

  foreach ($tool in @('cmake', 'ctest', 'npm')) {
    $step = @{ cmake = 'configure'; ctest = 'ctest'; npm = 'extension' }[$tool]
    $stub = @"
@echo off
setlocal
set step=$step
if "%~1"=="--build" set step=build
echo %step% %*>> "%GLINT_RELEASE_TEST_LOG%"
if "%GLINT_RELEASE_TEST_FAIL%"=="%step%" exit /b 17
exit /b 0
"@
    Set-Content -LiteralPath (Join-Path $toolDirectory "$tool.cmd") -Value $stub
  }

  foreach ($script in @('test-desktop-lifecycle', 'check-imports', 'package-release')) {
    $step = @{ 'test-desktop-lifecycle' = 'lifecycle'; 'check-imports' = 'imports'; 'package-release' = 'package' }[$script]
    $stub = @'
param([string]$Binary, [string]$ReportPath)
Add-Content -LiteralPath $env:GLINT_RELEASE_TEST_LOG -Value '__STEP__'
if ($env:GLINT_RELEASE_TEST_FAIL -eq '__STEP__') {
  switch ($env:GLINT_RELEASE_TEST_MODE) {
    'exit' { exit 19 }
    'error' { Write-Error '__STEP__ fixture error' }
    default { throw '__STEP__ fixture failure' }
  }
}
if ('__STEP__' -eq 'package') {
  Set-Content -LiteralPath (Join-Path $PSScriptRoot '../published.txt') -Value 'published'
}
'@
    Set-Content -LiteralPath (Join-Path $scriptDirectory "$script.ps1") -Value $stub.Replace('__STEP__', $step)
  }

  return [pscustomobject]@{
    Root = $root
    Gate = Join-Path $scriptDirectory 'check-release.ps1'
    Log = Join-Path $root 'steps.log'
    Tools = $toolDirectory
    Published = Join-Path $root 'published.txt'
  }
}

function Invoke-Gate {
  param(
    [Parameter(Mandatory)]$Fixture,
    [hashtable]$Parameters = @{},
    [string]$FailStep = '',
    [string]$FailMode = 'throw',
    [int]$InitialExitCode = 0
  )
  $env:PATH = $Fixture.Tools + [IO.Path]::PathSeparator + $originalPath
  $env:GLINT_RELEASE_TEST_LOG = $Fixture.Log
  $env:GLINT_RELEASE_TEST_FAIL = $FailStep
  $env:GLINT_RELEASE_TEST_MODE = $FailMode
  $global:LASTEXITCODE = $InitialExitCode
  $output = [Collections.Generic.List[string]]::new()
  $failure = $null
  try {
    & $Fixture.Gate @Parameters | ForEach-Object { $output.Add([string]$_) }
  } catch {
    $failure = $_
  }
  $steps = if (Test-Path -LiteralPath $Fixture.Log) { @(Get-Content -LiteralPath $Fixture.Log) } else { @() }
  return [pscustomobject]@{
    Failure = $failure
    Output = $output -join "`n"
    Steps = @($steps)
    Location = (Get-Location).Path
    Published = Test-Path -LiteralPath $Fixture.Published
  }
}

function Test-Case([string]$Name, [scriptblock]$Action) {
  $script:testCount++
  try {
    & $Action
    Write-Output "PASS $Name"
  } catch {
    $script:failures.Add("${Name}: $($_.Exception.Message)")
    Write-Output "FAIL ${Name}: $($_.Exception.Message)"
  }
}

try {
  New-Item -ItemType Directory -Path $fixtureDirectory | Out-Null
  # Invoke from a different directory so both success and failure test restoration.
  Set-Location -LiteralPath $temporaryParent

  Test-Case 'default validates every stage before publication' {
    $fixture = New-Fixture
    $result = Invoke-Gate $fixture
    Assert-True ($null -eq $result.Failure) "Default gate failed: $($result.Failure)"
    Assert-True ($result.Steps.Count -eq 7) "Expected seven stages, got $($result.Steps -join ', ')"
    Assert-True (($result.Steps | ForEach-Object { ($_ -split ' ')[0] }) -join ',' -eq 'configure,build,ctest,extension,lifecycle,imports,package') 'Incorrect validation order.'
    Assert-True $result.Published 'Default gate did not publish.'
    Assert-True ($result.Location -eq $temporaryParent.TrimEnd('\', '/')) 'Success changed the caller location.'
  }

  Test-Case 'CTest treats an empty suite as a failure' {
    $result = Invoke-Gate (New-Fixture)
    Assert-True ([bool]($result.Steps | Where-Object { $_ -match '^ctest .*--no-tests=error' })) 'CTest omitted --no-tests=error.'
  }

  foreach ($stage in @('configure', 'build', 'ctest', 'extension')) {
    Test-Case "native $stage failure stops before publication" {
      $fixture = New-Fixture
      $result = Invoke-Gate $fixture -FailStep $stage
      Assert-True ($null -ne $result.Failure) "$stage failure was accepted."
      Assert-True ($result.Failure.ToString() -match '17') 'Native exit code was lost.'
      Assert-True (-not $result.Published) 'Gate published after failure.'
      Assert-True (($result.Steps[-1] -split ' ')[0] -eq $stage) 'Gate continued after failure.'
      Assert-True ($result.Location -eq $temporaryParent.TrimEnd('\', '/')) 'Failure changed the caller location.'
    }
  }

  foreach ($stage in @('lifecycle', 'imports', 'package')) {
    foreach ($mode in @('throw', 'error', 'exit')) {
      Test-Case "PowerShell $stage $mode failure propagates" {
        $fixture = New-Fixture
        $result = Invoke-Gate $fixture -FailStep $stage -FailMode $mode
        Assert-True ($null -ne $result.Failure) "$stage $mode failure was accepted."
        Assert-True (-not $result.Published) 'Gate published after failure.'
        Assert-True ($result.Steps[-1] -eq $stage) 'Gate continued after failure.'
        Assert-True ($result.Location -eq $temporaryParent.TrimEnd('\', '/')) 'PowerShell failure changed the caller location.'
      }
    }
  }

  foreach ($skip in @('SkipCtest', 'SkipExtensionTests', 'SkipLifecycle', 'SkipImports')) {
    Test-Case "$skip requires SkipPackage before any stage runs" {
      $fixture = New-Fixture
      $result = Invoke-Gate $fixture -Parameters @{ $skip = $true }
      Assert-True ($null -ne $result.Failure) "$skip allowed unvalidated publication."
      Assert-True ($result.Failure.ToString() -match 'SkipPackage') 'Guard did not explain -SkipPackage.'
      Assert-True ($result.Steps.Count -eq 0) 'Guard ran stages before rejecting flags.'
      Assert-True (-not $result.Published) 'Guard permitted publication.'
    }

    Test-Case "$skip with SkipPackage reports only focused success" {
      $result = Invoke-Gate (New-Fixture) -Parameters @{ $skip = $true; SkipPackage = $true }
      Assert-True ($null -eq $result.Failure) "Focused run failed: $($result.Failure)"
      Assert-True ($result.Output -match 'Focused Release checks passed') 'Focused success was not identified.'
      Assert-True ($result.Output -notmatch '(?m)^Release (checks|validation).*passed') 'Focused run claimed full Release success.'
      Assert-True (-not $result.Published) 'Focused run published.'
    }
  }

  Test-Case 'configure and build skips still allow complete validation and publication' {
    $result = Invoke-Gate (New-Fixture) -Parameters @{ SkipConfigure = $true; SkipBuild = $true }
    Assert-True ($null -eq $result.Failure) "Existing-build validation failed: $($result.Failure)"
    Assert-True ($result.Steps.Count -eq 5) 'Existing-build validation omitted a check.'
    Assert-True $result.Published 'Existing-build validation did not publish.'
  }

  Test-Case 'validation without packaging identifies the omitted publication' {
    $result = Invoke-Gate (New-Fixture) -Parameters @{ SkipPackage = $true }
    Assert-True ($null -eq $result.Failure) "Validation failed: $($result.Failure)"
    Assert-True ($result.Output -match 'packaging skipped') 'Output did not identify omitted packaging.'
    Assert-True (-not $result.Published) 'SkipPackage published.'
  }

  Test-Case 'all stages skipped is rejected' {
    $result = Invoke-Gate (New-Fixture) -Parameters @{
      SkipConfigure = $true; SkipBuild = $true; SkipCtest = $true
      SkipExtensionTests = $true; SkipLifecycle = $true; SkipImports = $true; SkipPackage = $true
    }
    Assert-True ($null -ne $result.Failure) 'Empty selection succeeded.'
    Assert-True ($result.Failure.ToString() -match 'No Release checks selected') 'Empty selection error was unclear.'
  }

  Test-Case 'PowerShell-only focused checks ignore a stale native exit code' {
    $result = Invoke-Gate (New-Fixture) -InitialExitCode 41 -Parameters @{
      SkipConfigure = $true; SkipBuild = $true; SkipCtest = $true
      SkipExtensionTests = $true; SkipPackage = $true
    }
    Assert-True ($null -eq $result.Failure) "Stale native status failed a successful PowerShell step: $($result.Failure)"
    Assert-True (($result.Steps -join ',') -eq 'lifecycle,imports') 'PowerShell-only selection ran the wrong stages.'
  }

  Test-Case 'custom executable cannot inherit default build and CTest validation' {
    $fixture = New-Fixture
    $customBinary = Join-Path $fixture.Root 'other-Glint.exe'
    Set-Content -LiteralPath $customBinary -Value 'unrelated executable'
    $result = Invoke-Gate $fixture -Parameters @{ Binary = $customBinary }
    Assert-True ($null -ne $result.Failure) 'Custom executable inherited preset validation.'
    Assert-True ($result.Steps.Count -eq 0) 'Custom executable guard ran stages before rejecting.'
    Assert-True (-not $result.Published) 'Custom executable was published without its own CTest validation.'
  }

  Test-Case 'custom executable permits explicit focused executable checks' {
    $fixture = New-Fixture
    $customBinary = Join-Path $fixture.Root 'other-Glint.exe'
    Set-Content -LiteralPath $customBinary -Value 'focused executable'
    $result = Invoke-Gate $fixture -Parameters @{
      Binary = $customBinary; SkipConfigure = $true; SkipBuild = $true
      SkipCtest = $true; SkipExtensionTests = $true; SkipPackage = $true
    }
    Assert-True ($null -eq $result.Failure) "Focused executable check failed: $($result.Failure)"
    Assert-True ($result.Output -match 'Focused Release checks passed') 'Custom executable claimed full validation.'
  }

  Test-Case 'missing executable stops validation before packaging' {
    $fixture = New-Fixture
    Remove-Item -LiteralPath (Join-Path $fixture.Root 'build/release/Release/Glint.exe')
    $result = Invoke-Gate $fixture
    Assert-True ($null -ne $result.Failure) 'Missing executable passed validation.'
    Assert-True (-not $result.Published) 'Missing executable was published.'
    Assert-True ($result.Steps.Count -eq 4) 'Missing executable did not stop at lifecycle validation.'
  }
} finally {
  Set-Location -LiteralPath $originalLocation
  $env:PATH = $originalPath
  $env:GLINT_RELEASE_TEST_LOG = $originalLog
  $env:GLINT_RELEASE_TEST_FAIL = $originalFailure
  $env:GLINT_RELEASE_TEST_MODE = $originalFailureMode
  # Verify the recursive cleanup stays inside the temporary directory we created.
  $cleanupPath = [IO.Path]::GetFullPath($fixtureDirectory)
  if ((Split-Path -Parent $cleanupPath).TrimEnd('\', '/') -ne $temporaryParent.TrimEnd('\', '/') -or
      [IO.Path]::GetFileName($cleanupPath) -notmatch '^glint-release-gate-[a-f0-9]{32}$') {
    throw "Refusing unexpected fixture cleanup path: $cleanupPath"
  }
  if (Test-Path -LiteralPath $cleanupPath) { Remove-Item -LiteralPath $cleanupPath -Recurse -Force }
}

if ($failures.Count -gt 0) {
  throw "$($failures.Count) of $testCount release gate tests failed:`n$($failures -join "`n")"
}
Write-Output "All $testCount release gate tests passed."
