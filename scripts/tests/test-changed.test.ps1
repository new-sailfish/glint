[CmdletBinding()]
param([string]$Filter = '*')

# Standalone integration tests; only PowerShell and Git are required. Each case
# uses a disposable repository, and execution cases replace CTest/Node with
# command stubs so no configured build or third-party test framework is needed.
$ErrorActionPreference = 'Stop'
$selector = (Resolve-Path (Join-Path $PSScriptRoot '../test-changed.ps1')).ProviderPath
$sourceRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$hostExecutable = Join-Path $PSHOME 'pwsh.exe'
if (-not (Test-Path $hostExecutable)) { $hostExecutable = Join-Path $PSHOME 'powershell.exe' }
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('glint-selector-tests-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $temporaryRoot
$failures = [Collections.Generic.List[string]]::new()
$passed = 0
$caseNumber = 0
$ctestNames = @(
  'glint_core', 'glint_setup_progress', 'glint_connection_overview',
  'glint_search_navigation', 'glint_hotkey', 'glint_hotkey_window',
  'glint_pipe_server', 'glint_foreground_permission', 'glint_native_host',
  'glint_registration', 'glint_bridge_installation', 'glint_extension_identity',
  'glint_extension_bundle', 'glint_browser_launch', 'glint_setup_window'
)
$extensionNames = @('background-core', 'background', 'manifest', 'options')
$wrapper = Join-Path $temporaryRoot 'invoke-selector.ps1'
@'
param([string]$Settings)
$ErrorActionPreference = 'Stop'
$settingsValue = Get-Content -Raw -LiteralPath $Settings | ConvertFrom-Json
$parameters = @{}
foreach ($property in $settingsValue.Parameters.PSObject.Properties) {
  $parameters[$property.Name] = $property.Value
}
$env:PATH = $settingsValue.Bin + ';' + $env:PATH
$env:GLINT_SELECTOR_TEST_LOG = $settingsValue.Log
$env:GLINT_SELECTOR_CTEST_EXIT = [string]$settingsValue.CTestExit
$env:GLINT_SELECTOR_NODE_EXIT = [string]$settingsValue.NodeExit
$env:TEST_BASE = $settingsValue.TestBase
Set-Location -LiteralPath $settingsValue.WorkingDirectory
$status = 0
try { & $settingsValue.Selector @parameters } catch {
  Write-Output $_.Exception.Message
  $status = 1
}
Write-Output ('__LOCATION__=' + (Get-Location).Path)
exit $status
'@ | Set-Content -LiteralPath $wrapper

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function Assert-Contains([string]$Actual, [string]$Expected) {
  Assert-True ($Actual.Contains($Expected)) "Expected '$Expected' in output:`n$Actual"
}

function Invoke-FixtureGit([string[]]$Arguments) {
  $output = @(& git -C $script:fixture @Arguments 2>&1 | ForEach-Object { [string]$_ })
  if ($LASTEXITCODE -ne 0) { throw "Fixture Git failed: $($Arguments -join ' '): $($output -join ' ')" }
  return $output
}

function Write-FixtureFile([string]$Path, [string]$Content = 'fixture') {
  $target = Join-Path $script:fixture $Path
  $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)
  Set-Content -LiteralPath $target -Value $Content
}

function Save-FixtureCommit {
  $null = Invoke-FixtureGit @('add', '.')
  $null = Invoke-FixtureGit @('commit', '-qm', 'fixture change')
}

function New-Fixture {
  $script:caseNumber++
  $script:fixture = Join-Path $temporaryRoot "repo-$script:caseNumber"
  $null = New-Item -ItemType Directory -Path (Join-Path $script:fixture 'scripts')
  Copy-Item -LiteralPath $selector -Destination (Join-Path $script:fixture 'scripts/test-changed.ps1')
  Copy-Item -LiteralPath (Join-Path $sourceRoot 'CMakeLists.txt') -Destination $script:fixture
  foreach ($name in $extensionNames) { Write-FixtureFile "extension/test/$name.test.js" }
  Write-FixtureFile 'src/hotkey.cpp' 'original hotkey source'
  Write-FixtureFile 'src/setup_window.cpp' 'original setup source'
  Write-FixtureFile 'docs/notes.md' 'original notes'
  Write-FixtureFile 'README.md' 'fixture readme'
  $null = Invoke-FixtureGit @('init', '-q', '--initial-branch=master')
  $null = Invoke-FixtureGit @('config', 'user.name', 'Selector Tests')
  $null = Invoke-FixtureGit @('config', 'user.email', 'selector-tests@example.invalid')
  $null = Invoke-FixtureGit @('config', 'commit.gpgsign', 'false')
  $null = Invoke-FixtureGit @('config', 'core.autocrlf', 'false')
  Save-FixtureCommit
  $null = Invoke-FixtureGit @('tag', 'fixture-base')
  $script:bin = Join-Path $temporaryRoot "bin-$script:caseNumber"
  $null = New-Item -ItemType Directory -Path $script:bin
  foreach ($command in @('ctest', 'node')) {
    $exitVariable = if ($command -eq 'ctest') { 'GLINT_SELECTOR_CTEST_EXIT' } else { 'GLINT_SELECTOR_NODE_EXIT' }
    @(
      "Add-Content -LiteralPath `$env:GLINT_SELECTOR_TEST_LOG -Value ('$command;' + (Get-Location).Path + ';' + (`$args -join ' '))"
      "exit [int]`$env:$exitVariable"
    ) | Set-Content -LiteralPath (Join-Path $script:bin "$command.ps1")
  }
}

function Invoke-Selector(
  [hashtable]$Parameters = @{ List = $true; Files = @('README.md') },
  [string]$WorkingDirectory = $script:fixture,
  [string]$TestBase = '',
  [int]$CTestExit = 0,
  [int]$NodeExit = 0
) {
  $settings = Join-Path $temporaryRoot 'settings.json'
  $log = Join-Path $temporaryRoot ('commands-' + [guid]::NewGuid().ToString('N') + '.log')
  @{
    Selector = Join-Path $script:fixture 'scripts/test-changed.ps1'
    Parameters = $Parameters; WorkingDirectory = $WorkingDirectory
    TestBase = $TestBase; Bin = $script:bin; Log = $log
    CTestExit = $CTestExit; NodeExit = $NodeExit
  } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $settings
  $output = @(& $hostExecutable -NoProfile -File $wrapper -Settings $settings 2>&1 | ForEach-Object { [string]$_ })
  $status = $LASTEXITCODE
  $commands = if (Test-Path -LiteralPath $log) { Get-Content -Raw -LiteralPath $log } else { '' }
  return @{ Exit = $status; Output = $output -join "`n"; Commands = $commands }
}

function Assert-Success($Result) {
  Assert-True ($Result.Exit -eq 0) "Selector failed ($($Result.Exit)):`n$($Result.Output)"
}

function Get-ListedTests($Result, [string]$Section) {
  $inSection = $false
  foreach ($line in ($Result.Output -split "`n")) {
    if ($line -eq "${Section}:") { $inSection = $true; continue }
    if ($line -match '^\S') { $inSection = $false }
    if ($inSection -and $line -match '^  (\S+)  \(') { $Matches[1] }
  }
}

function Assert-Tests($Result, [string]$Section, [string[]]$Expected) {
  Assert-Success $Result
  $actual = @(Get-ListedTests $Result $Section) -join ','
  $expectedValue = @($Expected | Sort-Object -Unique) -join ','
  Assert-True ($actual -eq $expectedValue) "Expected $Section tests '$expectedValue', got '$actual'.`n$($Result.Output)"
}

function Test-Case([string]$Name, [scriptblock]$Body) {
  if ($Name -notlike $Filter) { return }
  try {
    New-Fixture
    & $Body
    $script:passed++
    Write-Output "PASS $Name"
  } catch {
    $failures.Add("${Name}: $($_.Exception.Message)")
    Write-Output "FAIL ${Name}: $($_.Exception.Message)"
  }
}

try {
  Test-Case 'baseline includes core and bridge smoke checks' {
    $result = Invoke-Selector
    Assert-Tests $result 'CTest' @('glint_core', 'glint_bridge_installation')
    Assert-Tests $result 'Extension' @('test/manifest.test.js')
    Assert-Contains $result.Output 'smoke baseline'
  }
  Test-Case 'explicit mapping is normalized, sorted, and deterministic' {
    $a = Invoke-Selector @{ List = $true; Files = @('.\src\hotkey.cpp', 'tests/test_registration.cpp', 'src/hotkey.cpp') }
    $b = Invoke-Selector @{ List = $true; Files = @('tests/test_registration.cpp,src/hotkey.cpp') }
    Assert-Tests $a 'CTest' @('glint_core', 'glint_bridge_installation', 'glint_hotkey', 'glint_hotkey_window', 'glint_registration')
    Assert-True ($a.Output -eq $b.Output) 'File ordering changed the selected output.'
    Assert-Contains $a.Output 'maps src/hotkey.cpp'
  }
  Test-Case 'UI theme and illustration select the setup window tests' {
    $result = Invoke-Selector @{ List = $true; Files = @('src/ui_theme.hpp', 'src/setup_illustration.hpp') }
    Assert-Tests $result 'CTest' @('glint_core', 'glint_bridge_installation', 'glint_hotkey_window', 'glint_setup_window')
  }
  Test-Case 'setup progress and overview headers select their compiled consumers' {
    $result = Invoke-Selector @{ List = $true; Files = @('src/connection_overview.hpp') }
    Assert-Tests $result 'CTest' @('glint_core', 'glint_bridge_installation', 'glint_connection_overview', 'glint_setup_progress', 'glint_setup_window')
    $result = Invoke-Selector @{ List = $true; Files = @('src/setup_progress.hpp') }
    Assert-Tests $result 'CTest' @('glint_core', 'glint_bridge_installation', 'glint_setup_progress', 'glint_setup_window')
  }
  Test-Case 'shared Windows and bridge files retain bridge smoke coverage' {
    foreach ($file in @('src/win_common.hpp', 'src/bridge_installation.cpp')) {
      $result = Invoke-Selector @{ List = $true; Files = @($file) }
      Assert-Tests $result 'CTest' @('glint_core', 'glint_bridge_installation')
      Assert-Contains $result.Output 'smoke fallback'
    }
  }
  Test-Case 'unknown native source selects every CTest test' {
    $result = Invoke-Selector @{ List = $true; Files = @('src/new_feature.cpp') }
    Assert-Tests $result 'CTest' $ctestNames
    Assert-Contains $result.Output 'full CTest set'
  }
  Test-Case 'unknown C++ test selects every CTest test' {
    $result = Invoke-Selector @{ List = $true; Files = @('tests/test_new_feature.cpp') }
    Assert-Tests $result 'CTest' $ctestNames
  }
  Test-Case 'new extension tests are discovered and run' {
    Write-FixtureFile 'extension/test/new-feature.test.js'
    $result = Invoke-Selector @{ List = $true; Files = @('extension/test/new-feature.test.js') }
    Assert-Success $result
    Assert-True (@(Get-ListedTests $result 'Extension') -contains 'test/new-feature.test.js') 'New extension test was not selected.'
  }
  Test-Case 'unknown extension implementation selects all extension tests' {
    $result = Invoke-Selector @{ List = $true; Files = @('extension/new-helper.js') }
    Assert-Tests $result 'Extension' @($extensionNames | ForEach-Object { "test/$_.test.js" })
  }
  Test-Case 'embedded extension runtime and icons select native bundle tests' {
    foreach ($file in @('extension/options.js', 'extension/icon-128.png')) {
      $result = Invoke-Selector @{ List = $true; Files = @($file) }
      Assert-Tests $result 'CTest' @('glint_core', 'glint_bridge_installation', 'glint_extension_bundle')
      Assert-Contains $result.Output "embedded extension resource $file"
    }
  }
  Test-Case 'removed extension tests select the remaining extension suite' {
    Remove-Item -LiteralPath (Join-Path $fixture 'extension/test/options.test.js')
    $result = Invoke-Selector @{ List = $true; Files = @('extension/test/options.test.js') }
    Assert-Tests $result 'Extension' @('test/background-core.test.js', 'test/background.test.js', 'test/manifest.test.js')
    Assert-Contains $result.Output 'full extension set'
  }
  Test-Case 'renamed extension tests run the new path and remaining suite' {
    $null = Invoke-FixtureGit @('mv', 'extension/test/options.test.js', 'extension/test/renamed.test.js')
    $result = Invoke-Selector @{ Base = 'fixture-base' }
    Assert-Success $result
    Assert-Contains $result.Commands 'test/renamed.test.js'
    Assert-True (-not $result.Commands.Contains('test/options.test.js')) 'Deleted extension test was still executed.'
  }
  Test-Case 'configuration changes include newly registered CTest names' {
    Add-Content -LiteralPath (Join-Path $fixture 'CMakeLists.txt') -Value 'add_test(NAME glint_new COMMAND glint_new_tests)'
    $result = Invoke-Selector @{ List = $true; Files = @('CMakeLists.txt') }
    Assert-Tests $result 'CTest' ($ctestNames + 'glint_new')
  }
  Test-Case 'native resource assets select the full CTest suite' {
    $result = Invoke-Selector @{ List = $true; Files = @('assets/glint.ico') }
    Assert-Tests $result 'CTest' $ctestNames
    Assert-Contains $result.Output 'configuration/resource change assets/glint.ico; full CTest set'
  }
  Test-Case 'version floors and generated resource templates select the full CTest suite' {
    foreach ($file in @('VERSION', 'app.manifest.in', 'cmake/glint_version.rc.in', 'cmake/GenerateBuildVersion.cmake')) {
      $result = Invoke-Selector @{ List = $true; Files = @($file) }
      Assert-Tests $result 'CTest' $ctestNames
      Assert-Contains $result.Output "configuration/resource change $file; full CTest set"
    }
  }
  Test-Case 'CTest execution requires tests and exact mapped names' {
    $result = Invoke-Selector @{ Files = @('src/hotkey.cpp') }
    Assert-Success $result
    Assert-Contains $result.Commands '--preset release'
    Assert-Contains $result.Commands '--no-tests=error'
    Assert-True (-not $result.Commands.Contains('(?:')) 'CTest regular expressions cannot use noncapturing groups.'
    Assert-Contains $result.Commands '--tests-regex ^(glint_bridge_installation|glint_core|glint_hotkey|glint_hotkey_window)$'
  }
  Test-Case 'full CTest execution has no restrictive test regex' {
    $result = Invoke-Selector @{ Files = @('tests/test_new_feature.cpp') }
    Assert-Success $result
    Assert-Contains $result.Commands '--no-tests=error'
    Assert-True (-not $result.Commands.Contains('--tests-regex')) 'Full fallback still filters out unknown tests.'
  }
  Test-Case 'CTest failure propagates and stops extension execution' {
    $result = Invoke-Selector @{ Files = @('README.md') } -CTestExit 7
    Assert-True ($result.Exit -ne 0) 'CTest failure was reported as success.'
    Assert-Contains $result.Output 'exit code 7'
    Assert-True (-not $result.Commands.Contains('node;')) 'Node ran after CTest failed.'
    Assert-True (-not $result.Output.Contains('Changed tests passed:')) 'Failure printed a passing summary.'
  }
  Test-Case 'extension execution failure propagates' {
    $result = Invoke-Selector @{ Files = @('README.md') } -NodeExit 9
    Assert-True ($result.Exit -ne 0) 'Node failure was reported as success.'
    Assert-Contains $result.Output 'exit code 9'
  }
  Test-Case 'successful and failed execution restore caller directory' {
    foreach ($status in @(0, 7)) {
      $result = Invoke-Selector @{ Files = @('README.md') } -WorkingDirectory $temporaryRoot -CTestExit $status
      Assert-Contains $result.Output "__LOCATION__=$temporaryRoot"
    }
  }
  Test-Case 'automatic base detects local master beyond HEAD parent' {
    $null = Invoke-FixtureGit @('checkout', '-qb', 'feature')
    Write-FixtureFile 'src/hotkey.cpp' 'changed hotkey'
    Save-FixtureCommit
    Write-FixtureFile 'docs/notes.md' 'later docs'
    Save-FixtureCommit
    $result = Invoke-Selector @{ List = $true }
    Assert-Success $result
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
  }
  Test-Case 'automatic base detects origin/master' {
    $null = Invoke-FixtureGit @('update-ref', 'refs/remotes/origin/master', 'fixture-base')
    $null = Invoke-FixtureGit @('checkout', '-qb', 'feature')
    $null = Invoke-FixtureGit @('branch', '-D', 'master')
    Write-FixtureFile 'src/hotkey.cpp' 'changed hotkey'
    Save-FixtureCommit
    Write-FixtureFile 'docs/notes.md' 'later docs'
    Save-FixtureCommit
    $result = Invoke-Selector @{ List = $true }
    Assert-Success $result
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
  }
  Test-Case 'TEST_BASE chooses an explicit comparison ref' {
    $null = Invoke-FixtureGit @('checkout', '-qb', 'feature')
    Write-FixtureFile 'src/setup_window.cpp' 'changed setup'
    Save-FixtureCommit
    $null = Invoke-FixtureGit @('tag', 'custom-base')
    Write-FixtureFile 'src/hotkey.cpp' 'changed hotkey'
    Save-FixtureCommit
    Write-FixtureFile 'docs/notes.md' 'later docs'
    Save-FixtureCommit
    $result = Invoke-Selector @{ List = $true } -TestBase 'custom-base'
    Assert-Success $result
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
    Assert-True (-not $result.Output.Contains('maps src/setup_window.cpp')) 'TEST_BASE was ignored.'
  }
  Test-Case '-Base takes precedence over TEST_BASE' {
    Write-FixtureFile 'src/hotkey.cpp' 'changed hotkey'
    $result = Invoke-Selector @{ List = $true; Base = 'fixture-base' } -TestBase 'invalid-ref'
    Assert-Success $result
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
  }
  Test-Case 'invalid explicit base fails instead of using another ref' {
    $result = Invoke-Selector @{ List = $true; Base = 'invalid-ref' }
    Assert-True ($result.Exit -ne 0) 'Invalid base was silently accepted.'
    Assert-Contains $result.Output 'comparison base'
  }
  Test-Case 'deleted native files still select their mapped tests' {
    Remove-Item -LiteralPath (Join-Path $fixture 'src/hotkey.cpp')
    $result = Invoke-Selector @{ List = $true; Base = 'fixture-base' }
    Assert-Success $result
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
  }
  Test-Case 'renames retain the old path for test selection' {
    $null = Invoke-FixtureGit @('mv', 'src/hotkey.cpp', 'docs/moved.cpp')
    $result = Invoke-Selector @{ List = $true; Base = 'fixture-base' }
    Assert-Success $result
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
    Assert-Contains $result.Output 'docs/moved.cpp'
  }
  Test-Case 'single changed and untracked paths remain separate' {
    Write-FixtureFile 'src/hotkey.cpp' 'changed hotkey'
    Write-FixtureFile 'docs/new.md' 'new docs'
    $result = Invoke-Selector @{ List = $true; Base = 'fixture-base' }
    Assert-Success $result
    Assert-Contains $result.Output 'Changed files: docs/new.md, src/hotkey.cpp'
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
  }
  Test-Case 'a clean comparison runs the baseline without scalar errors' {
    $result = Invoke-Selector @{ List = $true; Base = 'fixture-base' }
    Assert-Tests $result 'CTest' @('glint_core', 'glint_bridge_installation')
    Assert-Contains $result.Output 'Changed files: (none)'
  }
  Test-Case 'Git selection works from outside the repository' {
    Write-FixtureFile 'src/hotkey.cpp' 'changed hotkey'
    $result = Invoke-Selector @{ List = $true; Base = 'fixture-base' } -WorkingDirectory $temporaryRoot
    Assert-Success $result
    Assert-Contains $result.Output 'maps src/hotkey.cpp'
    Assert-Contains $result.Output "__LOCATION__=$temporaryRoot"
  }
} finally {
  # Only remove the unique directory created by this run, never a computed
  # repository/workspace path.
  $resolvedTemporaryRoot = (Resolve-Path -LiteralPath $temporaryRoot).ProviderPath
  $expectedParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
  Assert-True ((Split-Path -Parent $resolvedTemporaryRoot).TrimEnd('\', '/') -eq $expectedParent) 'Unsafe temporary cleanup path.'
  Assert-True ((Split-Path -Leaf $resolvedTemporaryRoot) -match '^glint-selector-tests-[0-9a-f]{32}$') 'Unsafe temporary cleanup name.'
  Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force
}

Write-Output "Selector regression tests: $passed passed, $($failures.Count) failed."
if ($failures.Count -gt 0) { exit 1 }
