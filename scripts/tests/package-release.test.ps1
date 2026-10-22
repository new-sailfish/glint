param([string]$Case = '*')

$ErrorActionPreference = 'Stop'
$packageScript = Join-Path (Split-Path -Parent $PSScriptRoot) 'package-release.ps1'
$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('Glint-package-tests-' + [guid]::NewGuid().ToString('N'))
$gitCommand = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
$failures = [Collections.Generic.List[string]]::new()
$passed = 0

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function Write-FixtureFile([string]$Path, [string]$Text) {
  [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
  [IO.File]::WriteAllText($Path, $Text)
}

function New-Fixture([switch]$NoGit) {
  $fixture = Join-Path $testDirectory ([guid]::NewGuid().ToString('N'))
  [IO.Directory]::CreateDirectory((Join-Path $fixture 'scripts')) | Out-Null
  Copy-Item -LiteralPath $packageScript -Destination (Join-Path $fixture 'scripts/package-release.ps1')
  foreach ($entry in @{
      'build/release/Release/Glint.exe' = 'validated executable v1'
      'README.md' = 'English release instructions'
      'README.zh-CN.md' = 'Chinese release instructions'
      'docs/design.md' = 'Public design'
      'docs/protocol.md' = 'Public protocol'
      'docs/branding.md' = 'Public branding'
      'docs/assets/glint-banner.svg' = '<svg>banner</svg>'
      'docs/assets/glint-logo.svg' = '<svg>logo</svg>'
      'assets/logo.svg' = '<svg />'
      'extension/manifest.json' = '{"manifest_version":3}'
      'extension/background.js' = 'background'
      'extension/background-core.js' = 'core'
      'extension/options.html' = 'options'
      'extension/options.js' = 'options script'
      'extension/icon-16.png' = 'icon16'
      'extension/icon-32.png' = 'icon32'
      'extension/icon-48.png' = 'icon48'
      'extension/icon-128.png' = 'icon128'
    }.GetEnumerator()) {
    Write-FixtureFile (Join-Path $fixture $entry.Key) $entry.Value
  }
  if (-not $NoGit -and $gitCommand) {
    & $gitCommand.Source -C $fixture init --quiet
    if ($LASTEXITCODE) { throw 'Could not initialize fixture repository.' }
    & $gitCommand.Source -C $fixture -c user.name=GlintTests -c user.email=tests@example.invalid commit --quiet --allow-empty -m fixture
    if ($LASTEXITCODE) { throw 'Could not create fixture revision.' }
  }
  return $fixture
}

function Invoke-Package([string]$Fixture, [string]$Binary) {
  if (-not $Binary) { $Binary = Join-Path $Fixture 'build/release/Release/Glint.exe' }
  & (Join-Path $Fixture 'scripts/package-release.ps1') -Binary $Binary | Out-Null
}

function Get-ReleaseSnapshot([string]$Fixture) {
  $paths = @(Get-ChildItem -LiteralPath (Join-Path $Fixture 'dist') -File -Recurse -Force)
  $paths += Get-Item -LiteralPath (Join-Path $Fixture 'Glint-win-x64.zip')
  return (($paths | Sort-Object FullName | ForEach-Object {
    $_.FullName + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
  }) -join "`n")
}

function Assert-NoStaging([string]$Fixture) {
  $remaining = @(Get-ChildItem -LiteralPath (Join-Path $Fixture 'build') -Directory -Filter 'package-*')
  Assert-True ($remaining.Count -eq 0) 'A package staging directory was left behind.'
}

function Assert-PackageRejected([string]$Fixture, [string]$Pattern, [string]$Binary) {
  $message = ''
  try { Invoke-Package $Fixture $Binary } catch { $message = $_.Exception.Message }
  Assert-True ($message -match $Pattern) "Expected rejection matching '$Pattern', got '$message'."
}

function Test-Case([string]$Name, [scriptblock]$Test) {
  if ($Name -notlike $Case) { return }
  try {
    & $Test
    $script:passed++
    Write-Output "PASS $Name"
  } catch {
    $failures.Add("${Name}: $($_.Exception.Message)")
    Write-Output "FAIL ${Name}: $($_.Exception.Message)"
  }
}

[IO.Directory]::CreateDirectory($testDirectory) | Out-Null
try {
  Test-Case 'only public documentation is packaged and earlier internal copies are removed' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    $publicDocuments = @('docs/design.md', 'docs/protocol.md', 'docs/branding.md',
      'docs/assets/glint-banner.svg', 'docs/assets/glint-logo.svg')
    $internalDocuments = @('docs/unknown-draft.md', 'docs/superpowers/plans/private-plan.md',
      'docs/superpowers/specs/private-spec.md', 'docs/validation-2026-10-22.md', 'docs/assets/screenshot.png')
    $manifestPath = Join-Path $fixture 'dist/release-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    foreach ($relative in $internalDocuments) {
      Write-FixtureFile (Join-Path $fixture $relative) "Private source: $relative"
      Write-FixtureFile (Join-Path $fixture "dist/$relative") "Previously published: $relative"
      $manifest.Files += [pscustomobject]@{ Path = $relative; SHA256 = 'old' }
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Invoke-Package $fixture
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $fixture 'Glint-win-x64.zip'))
    try {
      $publishedDocs = @(Get-ChildItem -LiteralPath (Join-Path $fixture 'dist/docs') -File -Recurse -Force)
      $manifestDocs = @($manifest.Files | Where-Object { $_.Path.StartsWith('docs/') })
      $archiveDocs = @($archive.Entries | Where-Object { $_.FullName.Replace('\', '/').StartsWith('docs/') })
      Assert-True ($publishedDocs.Count -eq $publicDocuments.Count) 'Published docs include internal files or omit public files.'
      Assert-True ($manifestDocs.Count -eq $publicDocuments.Count) 'The manifest does not contain exactly the public docs.'
      Assert-True ($archiveDocs.Count -eq $publicDocuments.Count) 'The archive does not contain exactly the public docs.'
      foreach ($relative in $publicDocuments) {
        $sourceHash = (Get-FileHash -LiteralPath (Join-Path $fixture $relative) -Algorithm SHA256).Hash
        $publishedHash = (Get-FileHash -LiteralPath (Join-Path $fixture "dist/$relative") -Algorithm SHA256).Hash
        Assert-True ($publishedHash -eq $sourceHash) "Published public doc differs from its source: $relative"
        $record = @($manifestDocs | Where-Object Path -eq $relative)
        Assert-True ($record.Count -eq 1 -and $record[0].SHA256 -eq $sourceHash) "Manifest omitted or mismatched public doc: $relative"
        $zipped = @($archiveDocs | Where-Object { $_.FullName.Replace('\', '/') -eq $relative })
        Assert-True ($zipped.Count -eq 1) "Archive omitted or duplicated public doc: $relative"
        $stream = $zipped[0].Open()
        $hasher = [Security.Cryptography.SHA256]::Create()
        try {
          Assert-True ([Convert]::ToHexString($hasher.ComputeHash($stream)) -eq $sourceHash) "Archived public doc differs from its source: $relative"
        } finally { $hasher.Dispose(); $stream.Dispose() }
      }
      foreach ($relative in $internalDocuments) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture "dist/$relative"))) "An internal doc survived repackaging: $relative"
        Assert-True (@($manifest.Files | Where-Object Path -eq $relative).Count -eq 0) "An internal doc entered the manifest: $relative"
        Assert-True (@($archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq $relative }).Count -eq 0) "An internal doc entered the archive: $relative"
      }
    } finally { $archive.Dispose() }
    Assert-NoStaging $fixture
  }

  Test-Case 'stale managed files are removed while user root extras survive' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    foreach ($directory in @('docs', 'assets', 'extension', 'browser-extension')) {
      Write-FixtureFile (Join-Path $fixture "dist/$directory/old/stale.txt") 'old package file'
    }
    Write-FixtureFile (Join-Path $fixture 'dist/user-notes.txt') 'keep this root extra'
    Write-FixtureFile (Join-Path $fixture 'dist/user-data/notes.txt') 'keep this directory extra'
    Write-FixtureFile (Join-Path $fixture 'dist/obsolete-tool.txt') 'old manifest member'
    $manifestPath = Join-Path $fixture 'dist/release-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.Files += [pscustomobject]@{ Path = 'obsolete-tool.txt'; SHA256 = 'old' }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Invoke-Package $fixture
    Assert-True (Test-Path -LiteralPath (Join-Path $fixture 'dist/user-notes.txt')) 'An unrelated root file was deleted.'
    Assert-True (Test-Path -LiteralPath (Join-Path $fixture 'dist/user-data/notes.txt')) 'An unrelated root directory was changed.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture 'dist/obsolete-tool.txt'))) 'A stale previous manifest member survived.'
    foreach ($directory in @('docs', 'assets', 'extension', 'browser-extension')) {
      Assert-True (-not (Test-Path -LiteralPath (Join-Path $fixture "dist/$directory/old/stale.txt"))) "A stale file survived under $directory."
    }
    Assert-NoStaging $fixture
  }

  Test-Case 'manifest and archive hashes match every published source file' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    $manifestPath = Join-Path $fixture 'dist/release-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert-True ($manifest.Files.Count -gt 10) 'The manifest omitted release files.'
    if ($gitCommand) {
      Assert-True ($manifest.WorkingTreeDirty -eq $true) 'Fixture source changes were not marked dirty.'
      Assert-True ($manifest.Commit -match '^[0-9a-f]{40,64}$') 'The fixture revision was not recorded.'
    }
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $fixture 'Glint-win-x64.zip'))
    try {
      Assert-True ($archive.Entries.Count -eq $manifest.Files.Count + 1) 'The archive contains unexpected or missing entries.'
      foreach ($entry in $manifest.Files) {
        Assert-True (-not $entry.Path.Contains('\')) "Manifest path is not portable: $($entry.Path)"
        $published = Join-Path $fixture ('dist/' + $entry.Path)
        Assert-True ((Get-FileHash -LiteralPath $published -Algorithm SHA256).Hash -eq $entry.SHA256) "Manifest hash mismatch: $($entry.Path)"
        $source = if ($entry.Path -eq 'Glint.exe') {
          Join-Path $fixture 'build/release/Release/Glint.exe'
        } elseif ($entry.Path.StartsWith('browser-extension/')) {
          Join-Path $fixture ('extension/' + $entry.Path.Substring('browser-extension/'.Length))
        } else { Join-Path $fixture $entry.Path }
        Assert-True ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -eq $entry.SHA256) "Source hash mismatch: $($entry.Path)"
      }
      foreach ($file in Get-ChildItem -LiteralPath (Join-Path $fixture 'dist') -File -Recurse) {
        $relative = [IO.Path]::GetRelativePath((Join-Path $fixture 'dist'), $file.FullName).Replace('\', '/')
        $zipped = @($archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq $relative })
        Assert-True ($zipped.Count -eq 1) "Expected exactly one archive entry for $relative."
        $stream = $zipped[0].Open()
        $hasher = [Security.Cryptography.SHA256]::Create()
        try {
          $digest = [Convert]::ToHexString($hasher.ComputeHash($stream))
          Assert-True ($digest -eq (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash) "Archive hash mismatch: $relative"
        } finally { $hasher.Dispose(); $stream.Dispose() }
      }
    } finally { $archive.Dispose() }
    Assert-NoStaging $fixture
  }

  Test-Case 'nonrepository and missing git produce an unknown revision' {
    foreach ($hideGit in @($false, $true)) {
      $fixture = New-Fixture -NoGit
      $oldPath = $env:PATH
      try {
        if ($hideGit) { $env:PATH = '' }
        $global:LASTEXITCODE = 0
        Invoke-Package $fixture
        Assert-True ($LASTEXITCODE -eq 0) 'Optional git metadata left a failure exit code in the in-process caller.'
      } finally { $env:PATH = $oldPath }
      $manifest = Get-Content -LiteralPath (Join-Path $fixture 'dist/release-manifest.json') -Raw | ConvertFrom-Json
      Assert-True ($manifest.Commit -eq 'unknown') 'Packaging without repository metadata did not use an unknown revision.'
      Assert-NoStaging $fixture
    }
  }

  Test-Case 'relative binary paths follow the PowerShell working directory' {
    $fixture = New-Fixture
    Push-Location -LiteralPath $fixture
    try { Invoke-Package $fixture './build/release/Release/Glint.exe' } finally { Pop-Location }
    Assert-True ((Get-FileHash -LiteralPath (Join-Path $fixture 'dist/Glint.exe')).Hash -eq
      (Get-FileHash -LiteralPath (Join-Path $fixture 'build/release/Release/Glint.exe')).Hash) 'A relative Binary path did not select the fixture executable.'
    Assert-NoStaging $fixture
  }

  Test-Case 'a locked changed executable leaves the entire published release untouched' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    $snapshot = Get-ReleaseSnapshot $fixture
    Write-FixtureFile (Join-Path $fixture 'build/release/Release/Glint.exe') 'validated executable v2'
    Write-FixtureFile (Join-Path $fixture 'README.md') 'new docs'
    $lock = [IO.File]::Open((Join-Path $fixture 'dist/Glint.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Assert-PackageRejected $fixture 'locked|running' } finally { $lock.Dispose() }
    Assert-True ((Get-ReleaseSnapshot $fixture) -eq $snapshot) 'The published release changed before the executable lock was reported.'
    Assert-NoStaging $fixture
  }

  Test-Case 'an identical locked executable allows documentation refresh' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    Write-FixtureFile (Join-Path $fixture 'README.md') 'refreshed release docs'
    $lock = [IO.File]::Open((Join-Path $fixture 'dist/Glint.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Invoke-Package $fixture } finally { $lock.Dispose() }
    Assert-True ((Get-Content -LiteralPath (Join-Path $fixture 'dist/README.md') -Raw) -eq 'refreshed release docs') 'Documentation was not refreshed with an identical locked executable.'
    Assert-NoStaging $fixture
  }

  Test-Case 'source and destination junctions are rejected without traversal' {
    foreach ($junctionParent in @('docs', 'dist/docs')) {
      $fixture = New-Fixture
      Invoke-Package $fixture
      $snapshot = Get-ReleaseSnapshot $fixture
      $outside = Join-Path $testDirectory ([guid]::NewGuid().ToString('N'))
      Write-FixtureFile (Join-Path $outside 'keep.txt') 'outside sentinel'
      $junction = Join-Path $fixture "$junctionParent/linked"
      New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
      try { Assert-PackageRejected $fixture 'reparse|symbolic|junction' } finally { Remove-Item -LiteralPath $junction -Force }
      Assert-True ((Get-Content -LiteralPath (Join-Path $outside 'keep.txt') -Raw) -eq 'outside sentinel') 'A junction target was modified.'
      Assert-True ((Get-ReleaseSnapshot $fixture) -eq $snapshot) 'The release was modified before the junction rejection.'
      Assert-NoStaging $fixture
    }
  }

  Test-Case 'outside binary and unsafe previous manifest paths are rejected' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    $snapshot = Get-ReleaseSnapshot $fixture
    $outsideBinary = Join-Path $testDirectory 'Glint.exe'
    Write-FixtureFile $outsideBinary 'outside executable'
    Assert-PackageRejected $fixture 'outside|workspace|repository' $outsideBinary
    Assert-True ((Get-ReleaseSnapshot $fixture) -eq $snapshot) 'An outside binary changed the published release.'
    $manifestPath = Join-Path $fixture 'dist/release-manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.Files += [pscustomobject]@{ Path = '../sentinel.txt'; SHA256 = 'old' }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    Write-FixtureFile (Join-Path $fixture 'sentinel.txt') 'keep sentinel'
    $snapshot = Get-ReleaseSnapshot $fixture
    Assert-PackageRejected $fixture 'unsafe|relative|outside|traversal'
    Assert-True ((Get-ReleaseSnapshot $fixture) -eq $snapshot) 'An unsafe manifest changed the published release.'
    Assert-True ((Get-Content -LiteralPath (Join-Path $fixture 'sentinel.txt') -Raw) -eq 'keep sentinel') 'A previous manifest escaped dist.'
    Assert-NoStaging $fixture
  }

  Test-Case 'a failed package keeps the published release and removes staging' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    $snapshot = Get-ReleaseSnapshot $fixture
    Remove-Item -LiteralPath (Join-Path $fixture 'extension/icon-128.png')
    Assert-PackageRejected $fixture 'icon-128.png|source|missing'
    Assert-True ((Get-ReleaseSnapshot $fixture) -eq $snapshot) 'A failed package changed the published release.'
    Assert-NoStaging $fixture
  }

  Test-Case 'corrupted staged copies are rejected before publication and cleaned up' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    $snapshot = Get-ReleaseSnapshot $fixture
    # Simulate a bad disk copy while still copying real fixture files. The
    # command wrapper exists only in this test's local scope.
    function Copy-Item {
      [CmdletBinding()]
      param([string]$LiteralPath, [string]$Destination, [switch]$Force)
      Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
      if ($Destination -match '[/\\]build[/\\]package-[0-9a-f]{32}[/\\]' -and
          [IO.Path]::GetFileName($Destination) -eq 'README.md') {
        [IO.File]::WriteAllText($Destination, 'corrupted copy')
      }
    }
    Assert-PackageRejected $fixture 'Staged file does not match'
    Assert-True ((Get-ReleaseSnapshot $fixture) -eq $snapshot) 'A corrupted staged copy changed the published release.'
    Assert-NoStaging $fixture
  }

  Test-Case 'a corrupted archive payload is rejected before published files change' {
    $fixture = New-Fixture
    Invoke-Package $fixture
    $snapshot = Get-ReleaseSnapshot $fixture
    function Copy-Item {
      [CmdletBinding()]
      param([string]$LiteralPath, [string]$Destination, [switch]$Force)
      Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
      # Corrupt an already-checked file after the final runtime file is staged.
      # The ZIP verifier must catch this before any dist files are copied.
      if ($Destination -match '[/\\]build[/\\]package-[0-9a-f]{32}[/\\]files[/\\]browser-extension[/\\]icon-128.png$') {
        $payloadRoot = Split-Path -Parent (Split-Path -Parent $Destination)
        [IO.File]::WriteAllText((Join-Path $payloadRoot 'README.md'), 'late staged corruption')
      }
    }
    Assert-PackageRejected $fixture 'Packaged file does not match'
    Assert-True ((Get-ReleaseSnapshot $fixture) -eq $snapshot) 'An invalid archive changed the published release.'
    Assert-NoStaging $fixture
  }
} finally {
  $absoluteTestDirectory = [IO.Path]::GetFullPath($testDirectory)
  $temporaryDirectory = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if (-not $absoluteTestDirectory.StartsWith($temporaryDirectory, [StringComparison]::OrdinalIgnoreCase) -or
      [IO.Path]::GetFileName($absoluteTestDirectory) -notlike 'Glint-package-tests-*') {
    throw "Refusing to clean an unexpected test directory: $absoluteTestDirectory"
  }
  Remove-Item -LiteralPath $absoluteTestDirectory -Recurse -Force
}

Write-Output "$passed packaging tests passed; $($failures.Count) failed."
if ($failures.Count) { throw ($failures -join "`n") }
