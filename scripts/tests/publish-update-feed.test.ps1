#Requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'publish-update-feed.ps1'
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testDirectory = Join-Path $temporaryParent ('glint-update-feed-tests-' + [guid]::NewGuid().ToString('N'))
$actionsWrapper = Join-Path $testDirectory 'actions-step.ps1'
$failures = [Collections.Generic.List[string]]::new()
$testCount = 0

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function Invoke-Git([string]$Directory, [string[]]$Arguments) {
  $output = & git -C $Directory @Arguments 2>&1
  if ($LASTEXITCODE) { throw "Fixture Git failed: $($output -join "`n")" }
  return ($output -join "`n")
}

function New-Fixture {
  $root = Join-Path $testDirectory ([guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $root | Out-Null
  Assert-True ([IO.File]::Exists($source)) 'Update-feed publisher implementation is missing.'
  $remote = Join-Path $root 'remote.git'
  Invoke-Git $root @('init', '--bare', '--initial-branch=master', $remote) | Out-Null
  $seed = Join-Path $root 'source'
  Invoke-Git $root @('init', '--initial-branch=master', $seed) | Out-Null
  Invoke-Git $seed @('config', 'user.name', 'Feed fixture') | Out-Null
  Invoke-Git $seed @('config', 'user.email', 'fixture@example.invalid') | Out-Null
  Set-Content -LiteralPath (Join-Path $seed 'sentinel.txt') 'preserve source branch'
  Invoke-Git $seed @('add', '.') | Out-Null
  Invoke-Git $seed @('commit', '-m', 'source fixture') | Out-Null
  Invoke-Git $seed @('push', $remote, 'HEAD:refs/heads/master') | Out-Null
  return [pscustomobject]@{
    Root = $root
    Remote = $remote
    Release = Join-Path $root 'dist'
    Site = Join-Path $root '_site'
    SourceCommit = Invoke-Git $remote @('rev-parse', 'refs/heads/master')
  }
}

function Set-Release($Fixture, [string]$Version = '0.1.2', [string]$Content = 'verified executable bytes') {
  New-Item -ItemType Directory -Path $Fixture.Release -Force | Out-Null
  $binary = Join-Path $Fixture.Release 'Glint.exe'
  [IO.File]::WriteAllText($binary, $Content)
  $manifest = [ordered]@{
    Product = 'Glint'
    Version = $Version
    FileVersion = "$Version.0"
    Commit = 'a' * 40
    WorkingTreeDirty = $false
    Binary = 'Glint.exe'
    Files = @(@{ Path = 'Glint.exe'; SHA256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash },
      @{ Path = 'README.md'; SHA256 = 'b' * 64 })
  }
  $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Fixture.Release 'release-manifest.json') -Encoding utf8
}

function Invoke-Publish($Fixture, [string]$Version = '0.1.2') {
  & $source -Repository $Fixture.Remote -ReleaseDirectory $Fixture.Release -Version $Version -Commit ('a' * 40) | Out-Null
}

function Invoke-ActionsWrapper($Fixture, [string]$Mode = 'Publish', [string]$Version = '0.1.2', [string]$Repository = $Fixture.Remote) {
  $output = & pwsh -NoProfile -File $actionsWrapper -Publisher $source -Mode $Mode -Repository $Repository -ReleaseDirectory $Fixture.Release -SiteDirectory $Fixture.Site -Version $Version -Commit ('a' * 40) 2>&1
  return [pscustomobject]@{ Code = $LASTEXITCODE; Output = ($output -join "`n") }
}

function Read-RemoteFile($Fixture, [string]$Path) {
  return Invoke-Git $Fixture.Remote @('show', "refs/heads/update-feed:$Path")
}

function Assert-Rejected([scriptblock]$Action, [string]$Pattern) {
  $failure = $null
  try { & $Action } catch { $failure = $_ }
  Assert-True ($null -ne $failure) 'Expected update-feed operation to fail.'
  Assert-True ($failure.ToString() -match $Pattern) "Unexpected failure: $failure"
}

function Test-Case([string]$Name, [scriptblock]$Action) {
  $script:testCount++
  try { & $Action; Write-Output "PASS $Name" } catch {
    $script:failures.Add("${Name}: $($_.Exception.Message)")
    Write-Output "FAIL ${Name}: $($_.Exception.Message)"
  }
}

try {
  New-Item -ItemType Directory -Path $testDirectory | Out-Null
  # GitHub Actions invokes the script in-process and propagates LASTEXITCODE;
  # running the publisher directly with pwsh -File does not reproduce that.
  Set-Content -LiteralPath $actionsWrapper -Encoding utf8 -Value @'
param([string]$Publisher, [string]$Mode, [string]$Repository, [string]$ReleaseDirectory, [string]$SiteDirectory, [string]$Version, [string]$Commit)
$ErrorActionPreference = 'Stop'
$global:LASTEXITCODE = 77
& $Publisher -Mode $Mode -Repository $Repository -ReleaseDirectory $ReleaseDirectory -SiteDirectory $SiteDirectory -Version $Version -Commit $Commit
if (Test-Path -LiteralPath variable:\LASTEXITCODE) { exit $LASTEXITCODE }
'@
  Test-Case 'first publication creates an isolated feed with latest and pinned files' {
    $fixture = New-Fixture
    Set-Release $fixture
    Invoke-Publish $fixture
    $manifest = Read-RemoteFile $fixture 'release-manifest.json' | ConvertFrom-Json
    Assert-True ($manifest.Version -ceq '0.1.2') 'Latest manifest was not published.'
    Assert-True ((Read-RemoteFile $fixture 'v0.1.2/release-manifest.json') -ceq (Read-RemoteFile $fixture 'release-manifest.json')) 'Pinned manifest differs from latest.'
    Assert-True ((Read-RemoteFile $fixture 'v0.1.2/Glint.exe') -ceq 'verified executable bytes') 'Pinned binary bytes differ.'
    $files = (Invoke-Git $fixture.Remote @('ls-tree', '-r', '--name-only', 'refs/heads/update-feed')) -split "`n"
    Assert-True ($files.Count -eq 3) 'Source files leaked into the feed branch.'
    Assert-True ((Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/master')) -eq $fixture.SourceCommit) 'The source branch changed.'
  }
  Test-Case 'new publication preserves pinned history and moves latest forward' {
    $fixture = New-Fixture
    Set-Release $fixture
    Invoke-Publish $fixture
    $oldManifest = Read-RemoteFile $fixture 'v0.1.2/release-manifest.json'
    Set-Release $fixture '0.1.3' 'new executable'
    Invoke-Publish $fixture '0.1.3'
    Assert-True ((Read-RemoteFile $fixture 'v0.1.2/release-manifest.json') -ceq $oldManifest) 'Older pinned manifest changed.'
    Assert-True ((Read-RemoteFile $fixture 'v0.1.2/Glint.exe') -ceq 'verified executable bytes') 'Older pinned binary changed.'
    Assert-True ((Read-RemoteFile $fixture 'release-manifest.json' | ConvertFrom-Json).Version -eq '0.1.3') 'Latest did not advance.'
  }
  Test-Case 'Git line-ending settings cannot change the published release bytes' {
    $fixture = New-Fixture
    Set-Release $fixture '0.1.2' "first line`r`nsecond line`r`n"
    $names = @('GIT_CONFIG_COUNT', 'GIT_CONFIG_KEY_0', 'GIT_CONFIG_VALUE_0')
    $saved = @{}
    foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
    try {
      $env:GIT_CONFIG_COUNT = '1'
      $env:GIT_CONFIG_KEY_0 = 'core.autocrlf'
      $env:GIT_CONFIG_VALUE_0 = 'true'
      Invoke-Publish $fixture
      foreach ($relative in @('Glint.exe', 'release-manifest.json')) {
        $sourceBlob = Invoke-Git $fixture.Remote @('hash-object', '--no-filters', (Join-Path $fixture.Release $relative))
        $publishedBlob = Invoke-Git $fixture.Remote @('rev-parse', "refs/heads/update-feed:v0.1.2/$relative")
        Assert-True ($sourceBlob -eq $publishedBlob) "Git changed the raw published bytes of $relative."
      }
    } finally {
      foreach ($name in $names) {
        if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
      }
    }
  }
  Test-Case 'rerunning an identical release creates no extra commit' {
    $fixture = New-Fixture
    Set-Release $fixture
    $result = Invoke-ActionsWrapper $fixture
    Assert-True ($result.Code -eq 0) "Actions rejected first publication: $($result.Output)"
    $before = Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/update-feed')
    $result = Invoke-ActionsWrapper $fixture
    Assert-True ($result.Code -eq 0) "Actions rejected an identical release: $($result.Output)"
    Assert-True ((Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/update-feed')) -eq $before) 'Identical release created another feed commit.'
  }
  Test-Case 'global Git attributes cannot change the published manifest bytes' {
    $fixture = New-Fixture
    Set-Release $fixture
    $manifestPath = Join-Path $fixture.Release 'release-manifest.json'
    [IO.File]::WriteAllText($manifestPath, ([IO.File]::ReadAllText($manifestPath) -replace '\r?\n', "`r`n"))
    $attributes = Join-Path $fixture.Root 'global-attributes'
    Set-Content -LiteralPath $attributes '*.json text eol=lf'
    $names = @('GIT_CONFIG_COUNT', 'GIT_CONFIG_KEY_0', 'GIT_CONFIG_VALUE_0')
    $saved = @{}
    foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
    try {
      $env:GIT_CONFIG_COUNT = '1'
      $env:GIT_CONFIG_KEY_0 = 'core.attributesFile'
      $env:GIT_CONFIG_VALUE_0 = $attributes
      Invoke-Publish $fixture
      $sourceBlob = Invoke-Git $fixture.Remote @('hash-object', '--no-filters', $manifestPath)
      $publishedBlob = Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/update-feed:v0.1.2/release-manifest.json')
      Assert-True ($sourceBlob -eq $publishedBlob) 'Global Git attributes changed the raw published manifest bytes.'
    } finally {
      foreach ($name in $names) {
        if ($null -eq $saved[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
      }
    }
  }
  Test-Case 'an older build retains latest while publishing its pinned version' {
    $fixture = New-Fixture
    Set-Release $fixture '0.1.10' 'newest'
    Invoke-Publish $fixture '0.1.10'
    Set-Release $fixture '0.1.9' 'older'
    Invoke-Publish $fixture '0.1.9'
    Assert-True ((Read-RemoteFile $fixture 'release-manifest.json' | ConvertFrom-Json).Version -eq '0.1.10') 'Out-of-order build regressed latest.'
    Assert-True ((Read-RemoteFile $fixture 'v0.1.9/Glint.exe') -ceq 'older') 'Older release was not pinned.'
  }
  Test-Case 'a pinned version cannot be replaced with different bytes' {
    $fixture = New-Fixture
    Set-Release $fixture
    Invoke-Publish $fixture
    $before = Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/update-feed')
    Set-Release $fixture '0.1.2' 'replacement bytes'
    Assert-Rejected { Invoke-Publish $fixture } 'pinned|immutable|different'
    Assert-True ((Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/update-feed')) -eq $before) 'Rejected replacement changed the feed.'
  }
  Test-Case 'corrupt binaries and wrong reserved versions or commits fail before publication' {
    foreach ($badField in @('hash', 'version', 'commit', 'duplicate', 'path')) {
      $fixture = New-Fixture
      Set-Release $fixture
      $manifestPath = Join-Path $fixture.Release 'release-manifest.json'
      $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
      switch ($badField) {
        hash { $manifest.Files[0].SHA256 = '0' * 64 }
        version { $manifest.Version = '../master' }
        commit { $manifest.Commit = 'c' * 40 }
        duplicate { $manifest.Files += $manifest.Files[0] }
        path { $manifest.Binary = '../Glint.exe' }
      }
      $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
      Assert-Rejected { Invoke-Publish $fixture } 'hash|version|commit|binary|Glint.exe'
      Assert-True ((Invoke-Git $fixture.Remote @('for-each-ref', '--format=%(refname)', 'refs/heads/update-feed')) -eq '') 'Invalid release created a feed branch.'
    }
  }
  Test-Case 'a rejected push fails visibly without modifying the published feed' {
    $fixture = New-Fixture
    Set-Release $fixture
    Invoke-Publish $fixture
    Invoke-Git $fixture.Remote @('config', 'receive.denyNonFastForwards', 'true') | Out-Null
    # Git's built-in denyCurrentBranch=refuse also applies to a bare repository
    # configured as non-bare with update-feed checked out; no executable hook needed.
    Invoke-Git $fixture.Remote @('config', 'core.bare', 'false') | Out-Null
    Invoke-Git $fixture.Remote @('symbolic-ref', 'HEAD', 'refs/heads/update-feed') | Out-Null
    $before = Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/update-feed')
    Set-Release $fixture '0.1.3'
    Assert-Rejected { Invoke-Publish $fixture '0.1.3' } 'Git|push|publish'
    Assert-True ((Invoke-Git $fixture.Remote @('rev-parse', 'refs/heads/update-feed')) -eq $before) 'Failed push changed the remote.'
  }
  Test-Case 'Pages staging preserves the website and includes every pinned version' {
    $fixture = New-Fixture
    Set-Release $fixture
    Invoke-Publish $fixture
    Set-Release $fixture '0.1.3' 'new executable'
    Invoke-Publish $fixture '0.1.3'
    New-Item -ItemType Directory -Path $fixture.Site | Out-Null
    Set-Content -LiteralPath (Join-Path $fixture.Site 'index.html') 'product website'
    $result = Invoke-ActionsWrapper $fixture -Mode Stage
    Assert-True ($result.Code -eq 0) "Actions rejected staging the published feed: $($result.Output)"
    Assert-True ((Get-Content -LiteralPath (Join-Path $fixture.Site 'index.html') -Raw).Trim() -eq 'product website') 'Staging overwrote the website.'
    Assert-True ([IO.File]::Exists((Join-Path $fixture.Site 'updates/v0.1.2/Glint.exe'))) 'Staging omitted older pinned executable.'
    Assert-True ([IO.File]::Exists((Join-Path $fixture.Site 'updates/v0.1.3/Glint.exe'))) 'Staging omitted latest pinned executable.'
    Assert-True ((Get-Content -LiteralPath (Join-Path $fixture.Site 'updates/release-manifest.json') -Raw | ConvertFrom-Json).Version -eq '0.1.3') 'Staging omitted latest manifest.'
    Assert-True (-not [IO.Directory]::Exists((Join-Path $fixture.Site 'updates/.git'))) 'Staging exposed Git metadata.'
  }
  Test-Case 'Pages builds without a feed branch before the first release' {
    $fixture = New-Fixture
    New-Item -ItemType Directory -Path $fixture.Site | Out-Null
    Set-Content -LiteralPath (Join-Path $fixture.Site 'index.html') 'product website'
    $result = Invoke-ActionsWrapper $fixture -Mode Stage
    Assert-True ($result.Code -eq 0) "Actions rejected staging before the first release (exit $($result.Code)): $($result.Output)"
    Assert-True (-not [IO.Directory]::Exists((Join-Path $fixture.Site 'updates'))) 'Absent feed created a misleading updates directory.'
    Assert-True ([IO.File]::Exists((Join-Path $fixture.Site 'index.html'))) 'Absent feed removed the website.'
  }
  Test-Case 'transport failure is distinguished from an absent feed branch' {
    $fixture = New-Fixture
    Assert-Rejected { & $source -Mode Stage -Repository (Join-Path $fixture.Root 'missing.git') -SiteDirectory $fixture.Site } 'Git|remote|query|feed'
    $result = Invoke-ActionsWrapper $fixture -Mode Stage -Repository (Join-Path $fixture.Root 'missing.git')
    Assert-True ($result.Code -ne 0) 'Actions accepted a real transport failure.'
    Assert-True ($result.Output -match 'Git update-feed ls-remote failed') 'Actions hid the real transport failure.'
  }
  Test-Case 'Actions rejects invalid release bytes before the first Git command' {
    $fixture = New-Fixture
    Set-Release $fixture
    Set-Content -LiteralPath (Join-Path $fixture.Release 'Glint.exe') 'corrupt bytes'
    $result = Invoke-ActionsWrapper $fixture
    Assert-True ($result.Code -ne 0) 'Actions accepted a corrupt release.'
    Assert-True ($result.Output -match 'hash does not match') 'Actions hid the release validation failure.'
  }
} finally {
  $absolute = [IO.Path]::GetFullPath($testDirectory)
  $temporaryPrefix = $temporaryParent.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if (-not $absolute.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
      [IO.Path]::GetFileName($absolute) -notlike 'glint-update-feed-tests-*') {
    throw "Refusing to clean an unexpected test directory: $absolute"
  }
  if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}

Write-Output "$testCount update-feed tests; $($failures.Count) failed."
if ($failures.Count) { throw ($failures -join "`n") }
# Expected failures above are assertions, not failures of this test script.
$global:LASTEXITCODE = 0
