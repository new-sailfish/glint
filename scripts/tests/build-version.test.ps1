#Requires -Version 7.0
[CmdletBinding()]
param([string]$Case = '*')

$ErrorActionPreference = 'Stop'
$repositoryDirectory = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('Glint-build-version-tests-' + [guid]::NewGuid().ToString('N'))
$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
$originalOverride = $env:GLINT_CI_VERSION
$failures = [Collections.Generic.List[string]]::new()
$passed = 0

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function Write-File([string]$Path, [string]$Text) {
  [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
  [IO.File]::WriteAllText($Path, $Text)
}

function New-Fixture {
  Assert-True (Test-Path -LiteralPath (Join-Path $repositoryDirectory 'cmake/GenerateBuildVersion.cmake')) 'The build-time version generator is missing.'
  $fixture = Join-Path $testDirectory ([guid]::NewGuid().ToString('N'))
  [IO.Directory]::CreateDirectory($fixture) | Out-Null
  Copy-Item -LiteralPath (Join-Path $repositoryDirectory 'cmake') -Destination $fixture -Recurse
  Copy-Item -LiteralPath (Join-Path $repositoryDirectory 'app.manifest.in') -Destination $fixture
  Write-File (Join-Path $fixture 'VERSION') "0.1.0`n"
  return $fixture
}

function Invoke-Generate([string]$Fixture, [string]$Output = 'build/release/generated/Release', [switch]$Failure) {
  $outputDirectory = Join-Path $Fixture $Output
  $lines = & $cmake "-DGLINT_SOURCE_DIR=$Fixture" "-DGLINT_OUTPUT_DIR=$outputDirectory" -P (Join-Path $Fixture 'cmake/GenerateBuildVersion.cmake') 2>&1
  $code = $LASTEXITCODE
  if ($Failure) {
    Assert-True ($code -ne 0) 'Invalid version input unexpectedly succeeded.'
    return ($lines -join "`n")
  }
  Assert-True ($code -eq 0) "Version generation failed: $($lines -join "`n")"
  return Get-GeneratedVersion $outputDirectory
}

function Get-GeneratedVersion([string]$OutputDirectory) {
  $header = Get-Content -LiteralPath (Join-Path $OutputDirectory 'glint_version.h') -Raw
  Assert-True ($header -match '#define GLINT_VERSION_STRING "([0-9]+\.[0-9]+\.[0-9]+)"') 'Missing product version string.'
  $version = $Matches[1]
  Assert-True ($header.Contains("#define GLINT_VERSION_WSTRING L`"$version`"")) 'Wide and narrow versions disagree.'
  [xml]$manifest = Get-Content -LiteralPath (Join-Path $OutputDirectory 'app.manifest') -Raw
  Assert-True ($manifest.assembly.assemblyIdentity.version -eq "$version.0") 'Application manifest and header disagree.'
  $resource = Get-Content -LiteralPath (Join-Path $OutputDirectory 'glint_version.rc') -Raw
  Assert-True ($resource.Contains('FILEVERSION ' + $version.Replace('.', ',') + ',0')) 'Numeric file version and header disagree.'
  Assert-True ($resource.Contains('VALUE "ProductVersion", "' + $version + '"')) 'Product version resource and header disagree.'
  return $version
}

function Test-Case([string]$Name, [scriptblock]$Body) {
  if ($Name -notlike $Case) { return }
  try {
    $env:GLINT_CI_VERSION = $null
    & $Body
    $script:passed++
    Write-Output "PASS $Name"
  } catch {
    $failures.Add("${Name}: $($_.Exception.Message)")
    Write-Output "FAIL ${Name}: $($_.Exception.Message)"
  }
}

try {
  Test-Case 'multiple CMake installations select the first executable' {
    # Hosted Windows exposes standalone and Visual Studio copies of CMake.
    # Exercise this harness with both discovery results without depending on
    # the developer machine having two installations or changing its PATH.
    $lines = @(& {
      param([string]$Harness, [string]$Executable)
      function Get-Command {
        [CmdletBinding()]
        param([string]$Name, [string]$CommandType)
        if ($Name -ne 'cmake' -or $CommandType -ne 'Application') { throw 'Unexpected command lookup in fixture.' }
        [pscustomobject]@{ Source = $Executable }
        [pscustomobject]@{ Source = "$Executable.second-installation.exe" }
      }
      & $Harness -Case 'consecutive generations increment patch and preserve source files'
    } $PSCommandPath $cmake)
    Assert-True ($lines -contains '1 build-version tests passed; 0 failed.') 'Generation did not succeed with multiple CMake candidates.'
  }

  Test-Case 'consecutive generations increment patch and preserve source files' {
    $fixture = New-Fixture
    $sources = @(Get-ChildItem -LiteralPath $fixture -File -Recurse)
    $hashes = @{}
    foreach ($source in $sources) { $hashes[$source.FullName] = (Get-FileHash -LiteralPath $source.FullName).Hash }
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.1') 'The first build did not increment the VERSION floor.'
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.2') 'An unchanged build did not increment patch.'
    foreach ($source in $sources) {
      Assert-True ((Get-FileHash -LiteralPath $source.FullName).Hash -eq $hashes[$source.FullName]) "Generation modified source $($source.Name)."
    }
  }

  Test-Case 'counter survives build cleanup and is shared across configurations' {
    $fixture = New-Fixture
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.1') 'Unexpected first version.'
    $buildDirectory = [IO.Path]::GetFullPath((Join-Path $fixture 'build'))
    Assert-True ($buildDirectory.StartsWith($testDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) 'Unsafe fixture build path.'
    Remove-Item -LiteralPath $buildDirectory -Recurse -Force
    Assert-True ((Invoke-Generate $fixture 'build/debug/generated/Debug') -eq '0.1.2') 'Build cleanup or a different configuration reset the counter.'
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.3') 'Release did not share the Debug counter.'
  }

  Test-Case 'manual floors and major minor sequences remain independent' {
    $fixture = New-Fixture
    Invoke-Generate $fixture | Out-Null
    Write-File (Join-Path $fixture 'VERSION') '0.1.20'
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.21') 'A higher manual floor was ignored.'
    Write-File (Join-Path $fixture 'VERSION') '0.2.0'
    Assert-True ((Invoke-Generate $fixture) -eq '0.2.1') 'A new minor version did not start its own sequence.'
    Write-File (Join-Path $fixture 'VERSION') '1.0.0'
    Assert-True ((Invoke-Generate $fixture) -eq '1.0.1') 'A new major version did not start its own sequence.'
    Write-File (Join-Path $fixture 'VERSION') '0.1.0'
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.22') 'Returning to an earlier sequence lost its counter.'
  }

  Test-Case 'parallel generators reserve distinct versions' {
    $fixture = New-Fixture
    $processes = @()
    try {
      foreach ($index in 1..6) {
        $start = [Diagnostics.ProcessStartInfo]::new($cmake)
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        foreach ($argument in @("-DGLINT_SOURCE_DIR=$fixture", "-DGLINT_OUTPUT_DIR=$fixture/build/parallel-$index", '-P', "$fixture/cmake/GenerateBuildVersion.cmake")) {
          $start.ArgumentList.Add($argument)
        }
        $processes += [Diagnostics.Process]::Start($start)
      }
      foreach ($process in $processes) {
        Assert-True ($process.WaitForExit(30000)) 'Concurrent generation timed out.'
        Assert-True ($process.ExitCode -eq 0) ('Concurrent generation failed: ' + $process.StandardError.ReadToEnd())
      }
      $versions = @(foreach ($index in 1..6) { Get-GeneratedVersion "$fixture/build/parallel-$index" })
      Assert-True (($versions | Sort-Object -Unique).Count -eq 6) 'Concurrent generators reused a patch.'
      Assert-True ((Invoke-Generate $fixture) -eq '0.1.7') 'Concurrent increments were lost.'
    } finally { foreach ($process in $processes) { $process.Dispose() } }
  }

  Test-Case 'corrupt exhausted and malformed versions fail without resetting state' {
    $fixture = New-Fixture
    $counter = Join-Path $fixture '.local/build-version/0.1.txt'
    foreach ($value in @('broken', '', '-1', '01', '65536', '999999999999999999999')) {
      Write-File $counter $value
      $errorText = Invoke-Generate $fixture -Failure
      Assert-True ($errorText -match 'counter|patch|range') 'Counter failure did not explain the invalid state.'
      Assert-True ([IO.File]::ReadAllText($counter) -ceq $value) 'A corrupt counter was changed.'
    }
    Write-File $counter '65535'
    Assert-True ((Invoke-Generate $fixture -Failure) -match 'exhaust|65535') 'An exhausted counter did not report its limit.'
    Assert-True ((Get-Content -LiteralPath $counter -Raw) -eq '65535') 'The counter wrapped.'
    foreach ($floor in @('0.1', '0.1.2.3', '0.1.01', '65536.0.0', '0.-1.0', "0.1.0`n1.2.3")) {
      Write-File (Join-Path $fixture 'VERSION') $floor
      Invoke-Generate $fixture -Failure | Out-Null
    }
  }

  Test-Case 'reserved CI override is validated and leaves the local sequence untouched' {
    $fixture = New-Fixture
    $env:GLINT_CI_VERSION = '0.1.123'
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.123') 'The reserved CI version was not used.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture '.local'))) 'CI created local counter state.'
    foreach ($override in @('0.1', '0.1.1.0', '0.1.65536', '0.1.-1', '0.1.01', '0.2.1', ' 0.1.2 ')) {
      $env:GLINT_CI_VERSION = $override
      Invoke-Generate $fixture -Failure | Out-Null
    }
    Write-File (Join-Path $fixture 'VERSION') '0.1.10'
    $env:GLINT_CI_VERSION = '0.1.9'
    Invoke-Generate $fixture -Failure | Out-Null
    Write-File (Join-Path $fixture 'VERSION') '0.1.0'
    $env:GLINT_CI_VERSION = $null
    Assert-True ((Invoke-Generate $fixture) -eq '0.1.1') 'CI changed the local counter.'
  }

  Test-Case 'configure and unrelated targets do not bump while native builds do' {
    $fixture = New-Fixture
    Write-File (Join-Path $fixture 'main.cpp') @'
#include "glint_version.h"
#include <iostream>
int main() { std::cout << GLINT_VERSION_STRING; }
'@
    Write-File (Join-Path $fixture 'CMakeLists.txt') @'
cmake_minimum_required(VERSION 3.21)
project(VersionFixture LANGUAGES CXX)
include(cmake/BuildVersion.cmake)
add_executable(version_fixture main.cpp)
target_link_options(version_fixture PRIVATE /MANIFEST:NO)
glint_add_build_version(version_fixture)
add_custom_target(unrelated COMMAND "${CMAKE_COMMAND}" -E true)
'@
    $build = Join-Path $fixture 'build/native'
    $lines = & $cmake -S $fixture -B $build -G 'Visual Studio 17 2022' -A x64 2>&1
    Assert-True ($LASTEXITCODE -eq 0) "Fixture configuration failed: $($lines -join "`n")"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture '.local'))) 'Configure allocated a version.'
    $lines = & $cmake --build $build --config Release --target unrelated 2>&1
    Assert-True ($LASTEXITCODE -eq 0) "Unrelated target failed: $($lines -join "`n")"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture '.local'))) 'An unrelated target allocated a version.'
    foreach ($patch in 1..2) {
      $lines = & $cmake --build $build --config Release --target version_fixture 2>&1
      Assert-True ($LASTEXITCODE -eq 0) "Native fixture build failed: $($lines -join "`n")"
      $binary = Join-Path $build 'Release/version_fixture.exe'
      $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($binary)
      Assert-True ($versionInfo.FileVersion -eq "0.1.$patch.0") 'Native file version is stale or missing.'
      Assert-True ($versionInfo.ProductVersion -eq "0.1.$patch") 'Native product version is stale or missing.'
      Assert-True ((& $binary) -eq "0.1.$patch") 'Compiled header and PE resource disagree.'
      Assert-True ((Get-GeneratedVersion (Join-Path $build 'generated/Release')) -eq "0.1.$patch") 'Generated files and native resource disagree.'
    }
  }
} finally {
  $env:GLINT_CI_VERSION = $originalOverride
  $absolute = [IO.Path]::GetFullPath($testDirectory)
  $temporaryPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if (-not $absolute.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
      [IO.Path]::GetFileName($absolute) -notlike 'Glint-build-version-tests-*') {
    throw "Refusing to clean unexpected fixture directory: $absolute"
  }
  if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}

Write-Output "$passed build-version tests passed; $($failures.Count) failed."
if ($failures.Count) { throw ($failures -join "`n") }
