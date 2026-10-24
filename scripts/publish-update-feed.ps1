#Requires -Version 7.0
<#
.SYNOPSIS
  Publish verified update files to update-feed, or stage that branch for Pages.
.DESCRIPTION
  Publish uses a temporary, isolated Git repository and a normal fast-forward
  push. Version directories are immutable. Stage preserves the product website
  and copies the feed into its updates/ directory. Absolute local repositories
  are supported for offline fixture tests; the only network remote is Glint's
  official GitHub repository. No source checkout or source branch is modified.
#>
[CmdletBinding()]
param(
  [ValidateSet('Publish', 'Stage')][string]$Mode = 'Publish',
  [string]$Repository = 'https://github.com/new-sailfish/glint.git',
  [string]$ReleaseDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'dist'),
  [string]$SiteDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) '_site'),
  [string]$Version,
  [string]$Commit
)

$ErrorActionPreference = 'Stop'
$branch = 'refs/heads/update-feed'
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryDirectory = Join-Path $temporaryParent ('glint-update-feed-' + [guid]::NewGuid().ToString('N'))
$checkout = Join-Path $temporaryDirectory 'repository'
$attributesPath = Join-Path $temporaryDirectory 'attributes'
$environmentNames = @('GIT_ASKPASS', 'GIT_TERMINAL_PROMPT', 'GIT_ATTR_NOSYSTEM', 'GLINT_UPDATE_FEED_TOKEN', 'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE')
$originalEnvironment = @{}
foreach ($name in $environmentNames) { $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }

function Assert-PlainPath([string]$Path) {
  $absolute = [IO.Path]::GetFullPath($Path)
  $ancestor = $absolute
  while ($ancestor) {
    $attributes = 0
    try { $attributes = [IO.File]::GetAttributes($ancestor) }
    catch [IO.FileNotFoundException] { }
    catch [IO.DirectoryNotFoundException] { }
    if ($attributes -band [IO.FileAttributes]::ReparsePoint) {
      throw "Refusing a symbolic link, junction, or reparse point: $ancestor"
    }
    $ancestor = [IO.Path]::GetDirectoryName($ancestor)
  }
  return $absolute
}

function Assert-Version([string]$Value) {
  if ($Value -cnotmatch '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$' -or
      @($Value.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
    throw 'Update-feed version must be canonical major.minor.patch with components from 0 to 65535.'
  }
}

function Read-Manifest([string]$Path) {
  $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
  Assert-Version $manifest.Version
  if ($manifest.Product -cne 'Glint' -or $manifest.Binary -cne 'Glint.exe' -or
      $manifest.FileVersion -cne "$($manifest.Version).0" -or
      $manifest.Commit -cnotmatch '^[0-9a-fA-F]{40}$') {
    throw 'Invalid Glint update-feed manifest product, binary, file version, or commit.'
  }
  $binaryEntries = @($manifest.Files | Where-Object { $_.Path -ieq 'Glint.exe' })
  if ($binaryEntries.Count -ne 1 -or $binaryEntries[0].Path -cne 'Glint.exe' -or
      $binaryEntries[0].SHA256 -cnotmatch '^[0-9a-fA-F]{64}$') {
    throw 'The update-feed manifest must contain exactly one Glint.exe SHA-256 hash.'
  }
  return $manifest
}

function Assert-ReleasePair([string]$ManifestPath, [string]$BinaryPath, [string]$ExpectedVersion) {
  $manifest = Read-Manifest $ManifestPath
  if ($ExpectedVersion -and $manifest.Version -cne $ExpectedVersion) {
    throw 'Update-feed manifest version does not match its pinned or reserved version.'
  }
  $digest = ($manifest.Files | Where-Object { $_.Path -ceq 'Glint.exe' }).SHA256
  if ((Get-FileHash -LiteralPath $BinaryPath -Algorithm SHA256).Hash -ine $digest) {
    throw 'Update-feed Glint.exe hash does not match the verified release manifest.'
  }
  return $manifest
}

function Invoke-FeedGit([string[]]$Arguments, [switch]$AllowMissing, [switch]$AllowDifference) {
  # Preserve the verified payload bytes regardless of the runner's Git defaults.
  $output = & git -C $checkout -c credential.helper= -c core.autocrlf=false -c "core.attributesFile=$attributesPath" @Arguments 2>&1
  $code = $LASTEXITCODE
  if ($code -ne 0 -and -not ($AllowMissing -and $code -eq 2) -and -not ($AllowDifference -and $code -eq 1)) {
    # Do not propagate transport diagnostics, which can include credentials.
    throw "Git update-feed $($Arguments[0]) failed (exit $code)."
  }
  # Recognized statuses are successful script operations. Actions propagates
  # LASTEXITCODE after invoking this script, including an early return.
  $global:LASTEXITCODE = 0
  return [pscustomobject]@{ Code = $code; Output = ($output -join "`n") }
}

function Get-FeedPaths {
  $tree = (Invoke-FeedGit @('ls-tree', '-r', 'HEAD')).Output
  $paths = @(
    foreach ($line in ($tree -split "`n")) {
      if ($line -cnotmatch '^100644 blob [0-9a-f]{40,64}\t(?<path>release-manifest\.json|v[0-9]+\.[0-9]+\.[0-9]+/(?:release-manifest\.json|Glint\.exe))$') {
        throw 'The update-feed branch contains an unexpected file or symbolic link.'
      }
      $Matches.path
    }
  )
  if ($paths -cnotcontains 'release-manifest.json') { throw 'The update-feed branch has no latest manifest.' }
  return $paths
}

try {
  if ($Repository -cne 'https://github.com/new-sailfish/glint.git' -and -not [IO.Path]::IsPathFullyQualified($Repository)) {
    throw 'Update-feed remote must be the official Glint GitHub repository or an absolute local fixture path.'
  }
  if ($Mode -eq 'Publish') {
    Assert-Version $Version
    if ($Commit -cnotmatch '^[0-9a-fA-F]{40}$') { throw 'The reserved source commit is required for update-feed publication.' }
    $releasePath = Assert-PlainPath $ReleaseDirectory
    $sourceManifest = Assert-PlainPath (Join-Path $releasePath 'release-manifest.json')
    $sourceBinary = Assert-PlainPath (Join-Path $releasePath 'Glint.exe')
    $incoming = Assert-ReleasePair $sourceManifest $sourceBinary $Version
    if ($incoming.Commit -ine $Commit) { throw 'Update-feed manifest commit does not match the official build commit.' }
  }

  Assert-PlainPath $temporaryDirectory | Out-Null
  New-Item -ItemType Directory -Path $checkout | Out-Null
  [IO.File]::WriteAllText($attributesPath, '')
  foreach ($name in @('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE')) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
  $env:GIT_TERMINAL_PROMPT = '0'
  $env:GIT_ATTR_NOSYSTEM = '1'
  if ($Repository -ceq 'https://github.com/new-sailfish/glint.git') {
    $token = if ($env:GH_TOKEN) { $env:GH_TOKEN } else { $env:GITHUB_TOKEN }
    if ($Mode -eq 'Publish' -and [string]::IsNullOrWhiteSpace($token)) {
      throw 'GH_TOKEN or GITHUB_TOKEN with repository contents write permission is required.'
    }
    # Git invokes a helper containing only environment-variable references. The
    # token never appears in a process argument, URL, config file, or log.
    $env:GLINT_UPDATE_FEED_TOKEN = $token
    Set-Content -LiteralPath (Join-Path $temporaryDirectory 'credentials.ps1') -Encoding utf8 -Value @'
param([string]$Prompt)
if ($Prompt -match '(?i)username') { 'x-access-token' } else { $env:GLINT_UPDATE_FEED_TOKEN }
'@
    Set-Content -LiteralPath (Join-Path $temporaryDirectory 'credentials.cmd') -Encoding ascii -Value '@pwsh -NoProfile -File "%~dp0credentials.ps1" "%~1"'
    $env:GIT_ASKPASS = Join-Path $temporaryDirectory 'credentials.cmd'
  }

  Invoke-FeedGit @('init', '--initial-branch=update-feed') | Out-Null
  Invoke-FeedGit @('remote', 'add', 'origin', $Repository) | Out-Null
  $remote = Invoke-FeedGit @('ls-remote', '--exit-code', '--heads', 'origin', $branch) -AllowMissing
  $hasFeed = $remote.Code -eq 0
  $paths = @()
  $latest = $null
  if ($hasFeed) {
    Invoke-FeedGit @('fetch', '--depth=1', 'origin', $branch) | Out-Null
    Invoke-FeedGit @('checkout', '--detach', 'FETCH_HEAD') | Out-Null
    $paths = @(Get-FeedPaths)
    $latestPath = Join-Path $checkout 'release-manifest.json'
    $latest = Read-Manifest $latestPath
    $latestPinned = Join-Path $checkout "v$($latest.Version)"
    Assert-ReleasePair (Join-Path $latestPinned 'release-manifest.json') (Join-Path $latestPinned 'Glint.exe') $latest.Version | Out-Null
    if ((Get-FileHash -LiteralPath $latestPath).Hash -ne (Get-FileHash -LiteralPath (Join-Path $latestPinned 'release-manifest.json')).Hash) {
      throw 'The latest update-feed manifest does not match its immutable pinned manifest.'
    }
  }

  if ($Mode -eq 'Stage') {
    if (-not $hasFeed) {
      Write-Output 'No update-feed branch exists yet; publishing the product website without update files.'
      return
    }
    $sitePath = Assert-PlainPath $SiteDirectory
    $updates = Assert-PlainPath (Join-Path $sitePath 'updates')
    if (Test-Path -LiteralPath $updates) { throw 'Pages updates destination already exists; stage into a fresh site directory.' }
    $versions = @($paths | Where-Object { $_ -like 'v*/Glint.exe' } | ForEach-Object { ($_ -split '/')[0].Substring(1) })
    foreach ($pinnedVersion in $versions) {
      $pinned = Join-Path $checkout "v$pinnedVersion"
      Assert-ReleasePair (Join-Path $pinned 'release-manifest.json') (Join-Path $pinned 'Glint.exe') $pinnedVersion | Out-Null
    }
    if ($paths.Count -ne (1 + 2 * $versions.Count)) { throw 'The update-feed branch contains an incomplete pinned release.' }
    foreach ($relative in $paths) {
      $destination = Join-Path $updates $relative
      New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
      Copy-Item -LiteralPath (Join-Path $checkout $relative) -Destination $destination
      if ((Get-FileHash -LiteralPath $destination).Hash -ne (Get-FileHash -LiteralPath (Join-Path $checkout $relative)).Hash) {
        throw "Staged update-feed file does not match its source: $relative"
      }
    }
    Write-Output "Staged update feed with $($versions.Count) pinned release(s) under updates/."
    return
  }

  $pinned = Join-Path $checkout "v$Version"
  if (Test-Path -LiteralPath $pinned) {
    foreach ($name in @('release-manifest.json', 'Glint.exe')) {
      if ((Get-FileHash -LiteralPath (Join-Path $pinned $name)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $releasePath $name)).Hash) {
        throw "Pinned update-feed version v$Version is immutable and already contains different bytes."
      }
    }
  } else {
    New-Item -ItemType Directory -Path $pinned | Out-Null
    Copy-Item -LiteralPath $sourceManifest, $sourceBinary -Destination $pinned
    Assert-ReleasePair (Join-Path $pinned 'release-manifest.json') (Join-Path $pinned 'Glint.exe') $Version | Out-Null
    if ((Get-FileHash -LiteralPath (Join-Path $pinned 'release-manifest.json')).Hash -ne (Get-FileHash -LiteralPath $sourceManifest).Hash) {
      throw 'Update-feed manifest changed while staging the release.'
    }
  }
  # Builds may complete out of reservation order. Always retain every pinned
  # release, while ensuring an older build cannot move latest backwards.
  if ($null -eq $latest -or [version]$Version -ge [version]$latest.Version) {
    Copy-Item -LiteralPath (Join-Path $pinned 'release-manifest.json') -Destination (Join-Path $checkout 'release-manifest.json') -Force
  }
  Invoke-FeedGit @('add', '--', 'release-manifest.json', "v$Version") | Out-Null
  foreach ($relative in @('release-manifest.json', "v$Version/release-manifest.json", "v$Version/Glint.exe")) {
    $rawBlob = (Invoke-FeedGit @('hash-object', '--no-filters', '--', $relative)).Output
    $stagedBlob = (Invoke-FeedGit @('rev-parse', '--verify', ":$relative")).Output
    if ($rawBlob -cne $stagedBlob) { throw "Git changed the verified update-feed bytes while staging: $relative" }
  }
  $changes = Invoke-FeedGit @('diff', '--cached', '--quiet') -AllowDifference
  if ($hasFeed -and $changes.Code -eq 0) {
    Write-Output "Update-feed v$Version is already published with identical bytes."
    return
  }
  Invoke-FeedGit @('-c', 'user.name=github-actions[bot]', '-c', 'user.email=41898282+github-actions[bot]@users.noreply.github.com', 'commit', '-m', "Publish update feed v$Version") | Out-Null
  Invoke-FeedGit @('push', 'origin', "HEAD:$branch") | Out-Null
  Write-Output "Published verified update-feed v$Version."
} finally {
  foreach ($name in $environmentNames) {
    if ($null -eq $originalEnvironment[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
    else { [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name]) }
  }
  $absolute = [IO.Path]::GetFullPath($temporaryDirectory)
  $temporaryPrefix = $temporaryParent.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  if (-not $absolute.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
      [IO.Path]::GetFileName($absolute) -notmatch '^glint-update-feed-[0-9a-f]{32}$') {
    throw "Refusing to clean an unexpected temporary repository: $absolute"
  }
  if (Test-Path -LiteralPath $absolute) {
    Assert-PlainPath $absolute | Out-Null
    Remove-Item -LiteralPath $absolute -Recurse -Force
  }
}
