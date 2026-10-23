param(
  [string]$Binary
)

$ErrorActionPreference = 'Stop'
$repositoryDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$repositoryPrefix = $repositoryDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
$releaseDirectory = Join-Path $repositoryDirectory 'dist'
$archivePath = Join-Path $repositoryDirectory 'Glint-win-x64.zip'
$manifestName = 'release-manifest.json'
$managedDirectories = @('docs', 'assets', 'extension', 'browser-extension')

function Assert-WorkspacePath([string]$Path, [switch]$AllowRoot) {
  $absolute = [IO.Path]::GetFullPath($Path)
  $isRoot = $absolute.Equals($repositoryDirectory, [StringComparison]::OrdinalIgnoreCase)
  if ((-not $AllowRoot -or -not $isRoot) -and
      -not $absolute.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Path is outside the repository workspace: $absolute"
  }
  # Inspect every ancestor, including ancestors of the workspace. Resolve-Path
  # alone does not reject Windows junctions and other reparse points.
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

function Get-SafeTree([string]$Directory) {
  Assert-WorkspacePath $Directory | Out-Null
  foreach ($item in Get-ChildItem -LiteralPath $Directory -Force) {
    Assert-WorkspacePath $item.FullName | Out-Null
    $item
    if ($item.PSIsContainer) { Get-SafeTree $item.FullName }
  }
}

function Get-ReleasePath([string]$RelativePath) {
  if ([string]::IsNullOrWhiteSpace($RelativePath) -or [IO.Path]::IsPathRooted($RelativePath)) {
    throw "Unsafe relative release path: $RelativePath"
  }
  foreach ($part in ($RelativePath -split '[/\\]')) {
    if (-not $part -or $part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or
        $part.EndsWith(' ') -or $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
      throw "Unsafe relative release path: $RelativePath"
    }
  }
  return Assert-WorkspacePath (Join-Path $releaseDirectory $RelativePath)
}

function Get-Digest([string]$Path) {
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-BinaryVersion([string]$Path) {
  $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
  $version = $metadata.ProductVersion
  if ($version -cnotmatch '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$' -or
      @($version.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
    throw "Binary ProductVersion must be major.minor.patch with components from 0 to 65535: $Path"
  }
  $fileVersion = "$version.0"
  $nativeFileVersion = '{0}.{1}.{2}.{3}' -f $metadata.FileMajorPart, $metadata.FileMinorPart,
    $metadata.FileBuildPart, $metadata.FilePrivatePart
  if ($metadata.FileVersion -cne $fileVersion -or $nativeFileVersion -cne $fileVersion) {
    throw "Binary FileVersion string and native version must both match ProductVersion with a zero revision ($fileVersion): $Path"
  }
  return [pscustomobject]@{ Version = $version; FileVersion = $fileVersion }
}

function Assert-BinaryCanPublish([string]$SourceHash) {
  $publishedBinary = Get-ReleasePath 'Glint.exe'
  if (-not (Test-Path -LiteralPath $publishedBinary)) { return $false }
  if (-not [IO.File]::Exists($publishedBinary)) { throw "Published executable is not a file: $publishedBinary" }
  try {
    # An identical running EXE needs no write access. This also permits docs
    # updates while a browser native host or the desktop app is using it.
    if ((Get-Digest $publishedBinary) -eq $SourceHash) { return $true }
    $probe = [IO.File]::Open($publishedBinary, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $probe.Dispose()
  } catch {
    throw "Published Glint.exe is locked or not writable. Close the running Glint before replacing it. $($_.Exception.Message)"
  }
  return $false
}

function Assert-Archive([string]$Path, $Hashes) {
  $archive = [IO.Compression.ZipFile]::OpenRead($Path)
  try {
    if ($archive.Entries.Count -ne $Hashes.Count) { throw 'Release archive has unexpected or missing entries.' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $archive.Entries) {
      $relative = $entry.FullName.Replace('\', '/')
      if (-not $Hashes.Contains($relative) -or -not $seen.Add($relative)) {
        throw "Unexpected or duplicate archive entry: $relative"
      }
      $stream = $entry.Open()
      $hasher = [Security.Cryptography.SHA256]::Create()
      try {
        if ([Convert]::ToHexString($hasher.ComputeHash($stream)) -ne $Hashes[$relative]) {
          throw "Packaged file does not match its staged bytes: $relative"
        }
      } finally { $hasher.Dispose(); $stream.Dispose() }
    }
  } finally { $archive.Dispose() }
}

Assert-WorkspacePath $repositoryDirectory -AllowRoot | Out-Null
Assert-WorkspacePath $releaseDirectory | Out-Null
Assert-WorkspacePath $archivePath | Out-Null
if ((Test-Path -LiteralPath $releaseDirectory) -and -not [IO.Directory]::Exists($releaseDirectory)) {
  throw "Release directory is not a directory: $releaseDirectory"
}
if ((Test-Path -LiteralPath $archivePath) -and -not [IO.File]::Exists($archivePath)) {
  throw "Release archive is not a file: $archivePath"
}
if (-not $Binary) { $Binary = Join-Path $repositoryDirectory 'build/release/Release/Glint.exe' }
$binaryPath = Assert-WorkspacePath ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Binary))
if (-not [IO.File]::Exists($binaryPath) -or [IO.Path]::GetFileName($binaryPath) -ine 'Glint.exe') {
  throw 'Binary must name the validated Release Glint.exe.'
}

$releaseFiles = [ordered]@{'Glint.exe' = $binaryPath}
foreach ($name in @('README.md', 'README.zh-CN.md')) {
  $releaseFiles[$name] = Join-Path $repositoryDirectory $name
}
foreach ($name in @('extension', 'assets')) {
  $sourceDirectory = Join-Path $repositoryDirectory $name
  foreach ($file in Get-SafeTree $sourceDirectory | Where-Object { -not $_.PSIsContainer }) {
    $relative = [IO.Path]::GetRelativePath($sourceDirectory, $file.FullName).Replace('\', '/')
    $releaseFiles["$name/$relative"] = $file.FullName
  }
}
# Validate the whole docs tree for unsafe paths, but publish only reviewed public
# documents. Local plans, validation notes, and new drafts must not ship by default.
Get-SafeTree (Join-Path $repositoryDirectory 'docs') | Out-Null
foreach ($name in @('docs/design.md', 'docs/protocol.md', 'docs/branding.md',
    'docs/user-guide.md', 'docs/development.md', 'docs/assets/README.md',
    'docs/assets/glint-banner.svg', 'docs/assets/glint-logo.svg',
    'docs/assets/tab-overload.svg', 'docs/assets/tab-overload-en.svg', 'docs/assets/search.png',
    'docs/assets/setup-browser.png', 'docs/assets/setup-load.png',
    'docs/assets/setup-connected.png', 'docs/assets/hotkey.png',
    'docs/assets/search-en.png', 'docs/assets/setup-browser-en.png',
    'docs/assets/setup-load-en.png', 'docs/assets/setup-connected-en.png', 'docs/assets/hotkey-en.png',
    'docs/assets/architecture.svg', 'docs/assets/architecture-en.svg')) {
  $releaseFiles[$name] = Join-Path $repositoryDirectory $name
}
# Browser profiles may already load this stable unpacked-extension path.
foreach ($name in @('manifest.json', 'background.js', 'background-core.js', 'options.html', 'options.js',
    'icon-16.png', 'icon-32.png', 'icon-48.png', 'icon-128.png',
    '_locales/en/messages.json', '_locales/zh_CN/messages.json', '_locales/zh_TW/messages.json')) {
  $releaseFiles["browser-extension/$name"] = Join-Path (Join-Path $repositoryDirectory 'extension') $name
}
$expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $releaseFiles.GetEnumerator()) {
  Assert-WorkspacePath $entry.Value | Out-Null
  if (-not [IO.File]::Exists($entry.Value)) { throw "Missing release source file: $($entry.Value)" }
  $destination = Get-ReleasePath $entry.Key
  if ([IO.Directory]::Exists($destination)) { throw "Release file destination is a directory: $destination" }
  $expected.Add($entry.Key) | Out-Null
}
$expected.Add($manifestName) | Out-Null
$publishedManifest = Get-ReleasePath $manifestName
if ([IO.Directory]::Exists($publishedManifest)) { throw "Release manifest destination is a directory: $publishedManifest" }

# Discover cleanup candidates before writing anything in dist. Only managed
# trees and individual files recorded by the previous manifest belong to us.
$staleFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$managedTree = @(
  foreach ($name in $managedDirectories) {
    $directory = Get-ReleasePath $name
    if (Test-Path -LiteralPath $directory) {
      if (-not [IO.Directory]::Exists($directory)) { throw "Managed release directory is not a directory: $directory" }
      Get-SafeTree $directory
      Get-Item -LiteralPath $directory -Force
    }
  }
)
foreach ($file in $managedTree | Where-Object { -not $_.PSIsContainer }) {
  $relative = [IO.Path]::GetRelativePath($releaseDirectory, $file.FullName).Replace('\', '/')
  if (-not $expected.Contains($relative)) { $staleFiles.Add($file.FullName) | Out-Null }
}
if ([IO.File]::Exists($publishedManifest)) {
  $previousManifest = Get-Content -LiteralPath $publishedManifest -Raw | ConvertFrom-Json
  if ($null -eq $previousManifest.Files) { throw 'Previous release manifest has no Files list.' }
  foreach ($entry in $previousManifest.Files) {
    $previousPath = Get-ReleasePath $entry.Path
    $relative = $entry.Path.Replace('\', '/')
    if (-not $expected.Contains($relative) -and [IO.File]::Exists($previousPath)) {
      $staleFiles.Add($previousPath) | Out-Null
    }
  }
}

# Detect a running, different published executable before any release changes.
Assert-BinaryCanPublish (Get-Digest $binaryPath) | Out-Null
$stagingDirectory = Assert-WorkspacePath (Join-Path $repositoryDirectory ('build/package-' + [guid]::NewGuid().ToString('N')))
$payloadDirectory = Join-Path $stagingDirectory 'files'
$stagedArchive = Join-Path $stagingDirectory 'Glint-win-x64.zip'
try {
  New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
  $packageFiles = [ordered]@{}
  $hashes = [ordered]@{}
  foreach ($entry in $releaseFiles.GetEnumerator()) {
    $destination = Assert-WorkspacePath (Join-Path $payloadDirectory $entry.Key)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    $sourceHash = Get-Digest $entry.Value
    Copy-Item -LiteralPath $entry.Value -Destination $destination
    $stagedHash = Get-Digest $destination
    if ($sourceHash -ne $stagedHash -or (Get-Digest $entry.Value) -ne $stagedHash) {
      throw "Staged file does not match its source: $($entry.Key)"
    }
    $packageFiles[$entry.Key] = $destination
    $hashes[$entry.Key] = $stagedHash
  }

  # Describe the executable bytes being packaged, including when current source
  # version files have advanced since this validated binary was built.
  $binaryVersion = Get-BinaryVersion $packageFiles['Glint.exe']
  $commit = 'unknown'
  $dirty = $null
  $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($git) {
    try {
      $revision = & $git.Source -C $repositoryDirectory rev-parse HEAD 2>$null
      if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace(($revision -join ''))) {
        $commit = ($revision -join '').Trim()
        $status = & $git.Source -C $repositoryDirectory status --porcelain 2>$null
        if ($LASTEXITCODE -eq 0) { $dirty = -not [string]::IsNullOrWhiteSpace(($status -join "`n")) }
      }
    } catch { $commit = 'unknown'; $dirty = $null }
  }
  $manifest = [ordered]@{
    Product = 'Glint'
    Version = $binaryVersion.Version
    FileVersion = $binaryVersion.FileVersion
    Commit = $commit
    WorkingTreeDirty = $dirty
    Binary = 'Glint.exe'
    Files = @(
      foreach ($entry in $releaseFiles.GetEnumerator()) {
        [ordered]@{ Path = $entry.Key; SHA256 = $hashes[$entry.Key] }
      }
    )
  }
  $manifestPath = Join-Path $payloadDirectory $manifestName
  $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8
  $packageFiles[$manifestName] = $manifestPath
  $hashes[$manifestName] = Get-Digest $manifestPath

  # The ZIP is complete and verified before either published output changes.
  [IO.Compression.ZipFile]::CreateFromDirectory($payloadDirectory, $stagedArchive)
  Assert-Archive $stagedArchive $hashes
  $archiveHash = Get-Digest $stagedArchive
  foreach ($entry in $releaseFiles.GetEnumerator()) {
    if ((Get-Digest $entry.Value) -ne $hashes[$entry.Key]) { throw "Release source changed during packaging: $($entry.Key)" }
  }
  $skipBinary = Assert-BinaryCanPublish $hashes['Glint.exe']

  foreach ($entry in $packageFiles.GetEnumerator()) {
    $destination = Get-ReleasePath $entry.Key
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    if (-not ($entry.Key -eq 'Glint.exe' -and $skipBinary)) {
      Copy-Item -LiteralPath $entry.Value -Destination $destination -Force
    }
    if ((Get-Digest $destination) -ne $hashes[$entry.Key]) {
      throw "Published file does not match its staged bytes: $($entry.Key)"
    }
  }
  foreach ($path in $staleFiles) {
    Assert-WorkspacePath $path | Out-Null
    Remove-Item -LiteralPath $path -Force
  }
  foreach ($directory in $managedTree | Where-Object PSIsContainer | Sort-Object { $_.FullName.Length } -Descending) {
    Assert-WorkspacePath $directory.FullName | Out-Null
    if (-not (Get-ChildItem -LiteralPath $directory.FullName -Force | Select-Object -First 1)) {
      Remove-Item -LiteralPath $directory.FullName -Force
    }
  }
  Assert-WorkspacePath $archivePath | Out-Null
  [IO.File]::Move($stagedArchive, $archivePath, $true)
  if ((Get-Digest $archivePath) -ne $archiveHash) { throw 'Published release archive differs from the verified archive.' }
} finally {
  if (Test-Path -LiteralPath $stagingDirectory) {
    Assert-WorkspacePath $stagingDirectory | Out-Null
    # Validate descendants too before using recursive removal on our unique tree.
    Get-SafeTree $stagingDirectory | Out-Null
    Remove-Item -LiteralPath $stagingDirectory -Recurse -Force
  }
}
$global:LASTEXITCODE = 0
Write-Output "Published and verified $($packageFiles.Count) files: $releaseDirectory"
Write-Output "Verified release archive: $archivePath"
