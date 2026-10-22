[CmdletBinding()]
param(
  [switch]$List,
  [string]$Base,
  [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
  [object[]]$Files
)

<#
.SYNOPSIS
  Run the CTest and browser-extension tests related to the current change.

.DESCRIPTION
  The default change set is the merge-base diff against the remote default
  branch, origin/main, origin/master, main, master, or HEAD^ (in that order).
  Pass -Base or set TEST_BASE to choose another ref. Explicit file paths skip
  Git discovery. A small core/bridge/extension smoke set is always included.
  Shared files, build configuration and files that cannot be mapped safely are
  reported with a reason; configuration/resource and unknown source/test
  changes select the full relevant suite. Configure and build the Release
  preset before execution; this script does not build stale/missing binaries.

.PARAMETER List
  Print selected tests and their reasons without running them.

.PARAMETER Base
  Git ref used as the comparison base. The merge-base with HEAD is used.

.PARAMETER Files
  Explicit changed files. Paths are relative to the repository root. When any
  are supplied, Git is not queried.
#>

$ErrorActionPreference = 'Stop'
$repositoryDirectory = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath

# Keep this smoke set intentionally small, while covering the protocol/core,
# shared Windows bridge code, and extension packaged-file/identity contract.
$smokeCTest = @('glint_core', 'glint_bridge_installation')
$smokeExtension = @('test/manifest.test.js')
# These source names make -List useful without requiring a configured build.
# Full CTest execution is deliberately unfiltered so newly registered tests
# cannot be omitted by an out-of-date ownership table.
$cmakeSource = Get-Content -Raw -LiteralPath (Join-Path $repositoryDirectory 'CMakeLists.txt')
$allCTest = @([regex]::Matches($cmakeSource, '(?i)add_test\s*\(\s*NAME\s+([^\s\)]+)') |
  ForEach-Object { $_.Groups[1].Value.Trim('"') } | Sort-Object -Unique)
$extensionTestDirectory = Join-Path $repositoryDirectory 'extension/test'
$allExtension = @(Get-ChildItem -LiteralPath $extensionTestDirectory -Filter '*.test.js' -File |
  ForEach-Object { 'test/' + $_.Name } | Sort-Object -Unique)
$maxMappedTargets = 4

# Test source/header ownership follows the existing CMake targets. A source
# shared by several targets intentionally selects all of them until its fan-out
# reaches the threshold below, where core and bridge smoke checks are used.
$testByFile = @{
  'tests/test_core.cpp' = @('glint_core')
  'tests/test_setup_progress.cpp' = @('glint_setup_progress')
  'tests/test_connection_overview.cpp' = @('glint_connection_overview')
  'tests/test_search_navigation.cpp' = @('glint_search_navigation')
  'tests/test_hotkey.cpp' = @('glint_hotkey')
  'tests/test_hotkey_window.cpp' = @('glint_hotkey_window')
  'tests/test_pipe_server.cpp' = @('glint_pipe_server')
  'tests/test_foreground_permission.cpp' = @('glint_foreground_permission')
  'tests/test_native_host.cpp' = @('glint_native_host')
  'tests/test_registration.cpp' = @('glint_registration')
  'tests/test_bridge_installation.cpp' = @('glint_bridge_installation')
  'tests/test_extension_identity.cpp' = @('glint_extension_identity')
  'tests/test_extension_bundle.cpp' = @('glint_extension_bundle')
  'tests/test_browser_launch.cpp' = @('glint_browser_launch')
  'tests/test_setup_window.cpp' = @('glint_setup_window')

  'src/core.cpp' = @('glint_core', 'glint_pipe_server', 'glint_foreground_permission', 'glint_native_host', 'glint_registration', 'glint_bridge_installation', 'glint_setup_window')
  'src/search_navigation.hpp' = @('glint_search_navigation')
  'src/hotkey.cpp' = @('glint_hotkey', 'glint_hotkey_window')
  'src/hotkey.hpp' = @('glint_hotkey', 'glint_hotkey_window')
  'src/hotkey_window.cpp' = @('glint_hotkey_window')
  'src/hotkey_window.hpp' = @('glint_hotkey_window')
  'src/pipe_server.cpp' = @('glint_pipe_server')
  'src/pipe_server.hpp' = @('glint_pipe_server')
  'src/foreground_permission.cpp' = @('glint_foreground_permission', 'glint_pipe_server')
  'src/foreground_permission.hpp' = @('glint_foreground_permission', 'glint_pipe_server')
  'src/native_host.cpp' = @('glint_native_host')
  'src/native_host.hpp' = @('glint_native_host')
  'src/registration.cpp' = @('glint_registration', 'glint_setup_window')
  'src/registration.hpp' = @('glint_registration', 'glint_setup_window')
  'src/bridge_installation.cpp' = @('glint_bridge_installation', 'glint_pipe_server', 'glint_foreground_permission', 'glint_registration', 'glint_setup_window')
  'src/bridge_installation.hpp' = @('glint_bridge_installation', 'glint_pipe_server', 'glint_foreground_permission', 'glint_registration', 'glint_setup_window')
  'src/extension_identity.cpp' = @('glint_extension_identity', 'glint_registration', 'glint_setup_window')
  'src/extension_identity.hpp' = @('glint_extension_identity', 'glint_registration', 'glint_setup_window')
  'src/extension_bundle.cpp' = @('glint_extension_bundle', 'glint_setup_window')
  'src/extension_bundle.hpp' = @('glint_extension_bundle', 'glint_setup_window')
  'src/browser_launch.cpp' = @('glint_browser_launch', 'glint_setup_window')
  'src/browser_launch.hpp' = @('glint_browser_launch', 'glint_setup_window')
  'src/setup_progress.hpp' = @('glint_setup_progress', 'glint_setup_window')
  'src/connection_overview.hpp' = @('glint_connection_overview', 'glint_setup_progress', 'glint_setup_window')
  'src/setup_window.cpp' = @('glint_setup_window')
  'src/setup_window.hpp' = @('glint_setup_window')
  'src/ui_theme.hpp' = @('glint_hotkey_window', 'glint_setup_window')
  'src/setup_illustration.hpp' = @('glint_setup_window')
  'src/win_common.cpp' = @('glint_pipe_server', 'glint_foreground_permission', 'glint_native_host', 'glint_registration', 'glint_bridge_installation', 'glint_setup_window')
  'src/win_common.hpp' = @('glint_pipe_server', 'glint_foreground_permission', 'glint_native_host', 'glint_registration', 'glint_bridge_installation', 'glint_setup_window')
}

$extensionByFile = @{
  'extension/background-core.js' = @('test/background-core.test.js', 'test/background.test.js')
  'extension/background.js' = @('test/background.test.js')
  'extension/options.js' = @('test/options.test.js')
  'extension/options.html' = @('test/options.test.js', 'test/manifest.test.js')
  'extension/manifest.json' = @('test/manifest.test.js', 'test/background.test.js', 'test/options.test.js')
  'extension/host-manifest.chrome.json' = @('test/manifest.test.js')
  'extension/host-manifest.edge.json' = @('test/manifest.test.js')
  'extension/icon-16.png' = @('test/manifest.test.js')
  'extension/icon-32.png' = @('test/manifest.test.js')
  'extension/icon-48.png' = @('test/manifest.test.js')
  'extension/icon-128.png' = @('test/manifest.test.js')
  'extension/test/background-core.test.js' = @('test/background-core.test.js')
  'extension/test/background.test.js' = @('test/background.test.js')
  'extension/test/manifest.test.js' = @('test/manifest.test.js')
  'extension/test/options.test.js' = @('test/options.test.js')
}

function Normalize-Path([string]$Path) {
  $value = $Path.Trim().Replace('\', '/')
  while ($value.StartsWith('./')) { $value = $value.Substring(2) }
  return $value
}

function Invoke-Git([string[]]$Arguments) {
  $output = & git -C $repositoryDirectory @Arguments 2>$null
  if ($LASTEXITCODE -ne 0) {
    throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
  }
  return @($output | ForEach-Object { [string]$_ })
}

function Get-ChangedFiles {
  if ($Files -and $Files.Count -gt 0) {
    return @($Files | ForEach-Object { [string]$_ -split ',' } | ForEach-Object { Normalize-Path $_ } | Where-Object { $_ })
  }

  $explicitBase = if ($Base) { $Base } else { $env:TEST_BASE }
  $candidates = @()
  if ($explicitBase) {
    $candidates += $explicitBase
  } else {
    try { $candidates += @(Invoke-Git @('symbolic-ref', '--quiet', '--short', 'refs/remotes/origin/HEAD')) } catch { }
    $candidates += @('origin/main', 'origin/master', 'main', 'master', 'HEAD^')
  }
  $root = $null
  foreach ($candidate in $candidates) {
    try {
      $mergeBase = @(Invoke-Git @('merge-base', 'HEAD', $candidate))
      if ($mergeBase.Count -gt 0 -and $mergeBase[0]) { $root = $mergeBase[0]; break }
    } catch { }
  }
  if (-not $root) {
    $baseDescription = if ($explicitBase) { " '$explicitBase'" } else { '' }
    throw "Unable to find a Git comparison base$baseDescription. Pass -Base <ref>, set TEST_BASE, or supply explicit file paths."
  }

  # Treat renames as delete/add pairs so the old owner's tests remain selected.
  # Force arrays: single-line native output otherwise concatenates as strings.
  $changed = @(Invoke-Git @('diff', '--name-only', '--no-renames', $root, '--'))
  $untracked = @(Invoke-Git @('ls-files', '--others', '--exclude-standard'))
  return @($changed + $untracked | ForEach-Object { Normalize-Path $_ } | Where-Object { $_ } | Sort-Object -Unique)
}

function Add-Selection([hashtable]$Selection, [string]$Name, [string]$Reason) {
  if (-not $Selection.ContainsKey($Name)) {
    $Selection[$Name] = $Reason
  } elseif ($Selection[$Name] -eq 'smoke baseline' -and $Reason -ne 'smoke baseline') {
    # A fallback reason is more useful than the generic baseline label when a
    # shared/unmapped change is what caused this smoke test to be selected.
    $Selection[$Name] = $Reason
  }
}

$changedFiles = @(Get-ChangedFiles | Sort-Object -Unique)
$selectedCTest = @{}
$selectedExtension = @{}
foreach ($test in $smokeCTest) { Add-Selection $selectedCTest $test 'smoke baseline' }
foreach ($test in $smokeExtension) { Add-Selection $selectedExtension $test 'smoke baseline' }

$fullCTest = $false
$fullExtension = $false
$fullCTestReason = ''
$fullExtensionReason = ''
foreach ($file in $changedFiles) {
  $normalized = Normalize-Path $file
  if ($normalized -match '^(CMakeLists\.txt|CMakePresets\.json|VERSION|app\.manifest(\.in)?)$' -or
      $normalized -match '(^|/)(CMakeLists\.txt|CMakePresets\.json|[^/]+\.cmake)$' -or
      $normalized -like 'cmake/*' -or
      $normalized -like 'assets/*' -or
      $normalized -match '\.(rc|def)$') {
    $fullCTest = $true
    if (-not $fullCTestReason) { $fullCTestReason = "configuration/resource change $normalized; full CTest set" }
    continue
  }

  if ($normalized -like 'extension/package.json') {
    $fullExtension = $true
    if (-not $fullExtensionReason) { $fullExtensionReason = "extension package change $normalized; full extension set" }
    continue
  }

  # These files are embedded by extension_bundle_resources.rc as well as
  # shipped to the browser, so exercise extraction when their bytes change.
  if ($normalized -in @(
      'extension/manifest.json', 'extension/background.js', 'extension/background-core.js',
      'extension/options.html', 'extension/options.js', 'extension/icon-16.png',
      'extension/icon-32.png', 'extension/icon-48.png', 'extension/icon-128.png')) {
    Add-Selection $selectedCTest 'glint_extension_bundle' "embedded extension resource $normalized"
  }

  if ($normalized -like 'extension/test/*' -and
      -not (Test-Path -LiteralPath (Join-Path $repositoryDirectory $normalized) -PathType Leaf)) {
    $fullExtension = $true
    if (-not $fullExtensionReason) { $fullExtensionReason = "removed extension test $normalized; full extension set" }
    continue
  }

  if ($extensionByFile.ContainsKey($normalized)) {
    $mapped = @($extensionByFile[$normalized])
    if ($mapped.Count -gt $maxMappedTargets) {
      Add-Selection $selectedExtension $smokeExtension[0] "shared extension file $normalized fans out to $($mapped.Count) tests; smoke fallback"
    } else {
      foreach ($test in $mapped) { Add-Selection $selectedExtension $test "maps $normalized" }
    }
    continue
  }

  if ($normalized -like 'extension/*') {
    # Discover new test files and use every extension test for unknown public
    # files. Smoke alone could report success while skipping new behavior.
    $fullExtension = $true
    if (-not $fullExtensionReason) { $fullExtensionReason = "unmapped extension file $normalized; full extension set" }
    continue
  }

  if ($testByFile.ContainsKey($normalized)) {
    $mapped = @($testByFile[$normalized])
    if ($mapped.Count -gt $maxMappedTargets) {
      foreach ($test in $smokeCTest) { Add-Selection $selectedCTest $test "shared source $normalized fans out to $($mapped.Count) tests; smoke fallback" }
    } else {
      foreach ($test in $mapped) { Add-Selection $selectedCTest $test "maps $normalized" }
    }
    continue
  }

  if ($normalized -like 'tests/*') {
    $fullCTest = $true
    if (-not $fullCTestReason) { $fullCTestReason = "unmapped C++ test/source $normalized; full CTest set" }
    continue
  }

  if ($normalized -like 'src/*' -or $normalized -like 'include/*') {
    $fullCTest = $true
    if (-not $fullCTestReason) { $fullCTestReason = "unmapped source $normalized; full CTest set" }
    continue
  }

  # Changes outside a known test surface still get the baseline smoke checks.
  Add-Selection $selectedCTest $smokeCTest[0] "unmapped file $normalized; smoke fallback"
}

if ($fullCTest) {
  $selectedCTest = @{}
  foreach ($test in $allCTest) { Add-Selection $selectedCTest $test $fullCTestReason }
}
if ($fullExtension) {
  $selectedExtension = @{}
  foreach ($test in $allExtension) { Add-Selection $selectedExtension $test $fullExtensionReason }
}

$ctestNames = @($selectedCTest.Keys | Sort-Object)
$extensionTests = @($selectedExtension.Keys | Sort-Object)
if ($List) {
  Write-Output ('Changed files: ' + ($(if ($changedFiles.Count) { $changedFiles -join ', ' } else { '(none)' })))
  Write-Output 'CTest:'
  foreach ($test in $ctestNames) { Write-Output ("  {0}  ({1})" -f $test, $selectedCTest[$test]) }
  Write-Output 'Extension:'
  foreach ($test in $extensionTests) { Write-Output ("  {0}  ({1})" -f $test, $selectedExtension[$test]) }
  return
}

function Invoke-Checked([string]$FilePath, [string[]]$Arguments, [string]$WorkingDirectory) {
  $oldLocation = Get-Location
  try {
    if ($WorkingDirectory) { Set-Location -LiteralPath $WorkingDirectory }
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$FilePath failed with exit code $LASTEXITCODE." }
  } finally {
    Set-Location -LiteralPath $oldLocation
  }
}

if ($ctestNames.Count -gt 0) {
  Write-Output ('Running CTest: ' + ($ctestNames -join ', '))
  $ctestArguments = @('--preset', 'release', '--output-on-failure', '--no-tests=error')
  if (-not $fullCTest) {
    # CTest uses a POSIX-style expression parser; (?:...) is unsupported.
    $regex = '^(' + (($ctestNames | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
    $ctestArguments += @('--tests-regex', $regex)
  }
  Invoke-Checked 'ctest' $ctestArguments $repositoryDirectory
}
if ($extensionTests.Count -gt 0) {
  Write-Output ('Running extension tests: ' + ($extensionTests -join ', '))
  Invoke-Checked 'node' (@('--test') + $extensionTests) (Join-Path $repositoryDirectory 'extension')
}

Write-Output ('Changed tests passed: {0} CTest target(s), {1} extension test file(s).' -f $ctestNames.Count, $extensionTests.Count)
