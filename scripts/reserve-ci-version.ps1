#Requires -Version 7.0
<#
.SYNOPSIS
  Atomically reserve the next official build version using a GitHub tag.
.DESCRIPTION
  Reads the repository VERSION floor and all build-version/* tags. Creates a
  lightweight tag at GITHUB_SHA, then appends version=<major.minor.patch> to
  GITHUB_OUTPUT. Each invocation, including a rerun of the same commit, consumes
  a new patch. Failed builds retain reservations; local builds use another counter.
  Requires GH_TOKEN or GITHUB_TOKEN with repository contents write permission.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function ConvertTo-BuildVersion([string]$Text, [string]$Source) {
  if ($Text -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw "$Source must contain a canonical major.minor.patch version."
  }
  $parts = foreach ($part in $Text.Split('.')) {
    $number = 0
    if (-not [int]::TryParse($part, [ref]$number) -or $number -gt 65535) {
      throw "$Source version components must be in the range 0..65535."
    }
    $number
  }
  return ,$parts
}

function Get-HttpStatus($Failure) {
  if ($null -ne $Failure.Exception.Response.StatusCode) { return [int]$Failure.Exception.Response.StatusCode }
  return 0
}

function Get-GitHubResource([string]$Uri) {
  try {
    Invoke-RestMethod -Method Get -Uri $Uri -Headers $headers -TimeoutSec 30 -ErrorAction Stop
  } catch {
    $status = Get-HttpStatus $_
    # Never propagate transport exceptions or request headers containing tokens.
    throw "GitHub version query failed (HTTP $status)."
  }
}

$repositoryDirectory = Split-Path -Parent $PSScriptRoot
$floor = ConvertTo-BuildVersion (Get-Content -LiteralPath (Join-Path $repositoryDirectory 'VERSION') -Raw).Trim() 'VERSION'
if ($env:GITHUB_EVENT_NAME -notin @('push', 'workflow_dispatch')) {
  throw 'Official version reservation requires a push or workflow_dispatch event; pull requests cannot reserve versions.'
}
if ($env:GITHUB_REPOSITORY -cnotmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
  throw 'A valid GITHUB_REPOSITORY owner/name is required.'
}
if ($env:GITHUB_SHA -cnotmatch '^[0-9a-fA-F]{40}$') { throw 'A valid 40-character GITHUB_SHA is required.' }
if ([string]::IsNullOrWhiteSpace($env:GITHUB_OUTPUT)) { throw 'GITHUB_OUTPUT is required.' }
$outputPath = [IO.Path]::GetFullPath($env:GITHUB_OUTPUT)
if (-not [IO.Directory]::Exists([IO.Path]::GetDirectoryName($outputPath))) {
  throw 'The GITHUB_OUTPUT parent directory must exist.'
}
$token = if (-not [string]::IsNullOrWhiteSpace($env:GH_TOKEN)) { $env:GH_TOKEN } else { $env:GITHUB_TOKEN }
if ([string]::IsNullOrWhiteSpace($token)) { throw 'GH_TOKEN or GITHUB_TOKEN is required.' }
$headers = @{
  Authorization = "Bearer $token"
  Accept = 'application/vnd.github+json'
  'X-GitHub-Api-Version' = '2022-11-28'
  'User-Agent' = 'Glint-CI-Version'
}
$apiRoot = "https://api.github.com/repos/$env:GITHUB_REPOSITORY"
$minimumPatch = $floor[2]

for ($attempt = 1; $attempt -le 20; $attempt++) {
  $maximumPatch = $minimumPatch
  # List tags rather than matching-refs: this endpoint documents pagination.
  for ($page = 1; ; $page++) {
    # REST emits a JSON array as one pipeline object. Capture it directly:
    # @(Get-GitHubResource ...) would add an outer array around each page.
    $tags = Get-GitHubResource "${apiRoot}/tags?per_page=100&page=$page"
    if ($tags -isnot [array]) { throw 'GitHub returned an invalid tag listing response.' }
    foreach ($tag in $tags) {
      if ($null -eq $tag -or $tag.name -isnot [string]) { throw 'GitHub returned an invalid tag listing response.' }
      if (-not $tag.name.StartsWith('build-version/', [StringComparison]::Ordinal)) { continue }
      $remote = ConvertTo-BuildVersion $tag.name.Substring('build-version/'.Length) 'Remote build-version counter'
      if ($remote[0] -eq $floor[0] -and $remote[1] -eq $floor[1]) {
        $maximumPatch = [Math]::Max($maximumPatch, $remote[2])
      }
    }
    if ($tags.Count -lt 100) { break }
  }
  if ($maximumPatch -ge 65535) { throw 'The official patch counter is exhausted at 65535; increase VERSION major or minor.' }
  $patch = $maximumPatch + 1
  $version = "$($floor[0]).$($floor[1]).$patch"
  $ref = "refs/tags/build-version/$version"
  $body = @{ ref = $ref; sha = $env:GITHUB_SHA } | ConvertTo-Json -Compress
  try {
    $created = Invoke-RestMethod -Method Post -Uri "${apiRoot}/git/refs" -Headers $headers `
      -ContentType 'application/json' -Body $body -TimeoutSec 30 -ErrorAction Stop
  } catch {
    $status = Get-HttpStatus $_
    $collision = $false
    if ($status -eq 422) {
      try {
        $details = $_.ErrorDetails.Message | ConvertFrom-Json -ErrorAction Stop
        $collision = $details.message -eq 'Reference already exists' -or
          @($details.errors | Where-Object { $_.resource -eq 'Reference' -and $_.code -eq 'already_exists' }).Count -gt 0
      } catch { $collision = $false }
    }
    if (-not $collision) { throw "GitHub version reservation failed (HTTP $status)." }
    $existing = Get-GitHubResource "${apiRoot}/git/ref/tags/build-version/$version"
    if ($existing.ref -cne $ref -or $existing.object.sha -cnotmatch '^[0-9a-fA-F]{40}$') {
      throw 'GitHub did not confirm the version reservation collision.'
    }
    # A freshly created tag may take time to appear in paginated listings.
    $minimumPatch = $patch
    continue
  }
  if ($created.ref -cne $ref -or $created.object.sha -ine $env:GITHUB_SHA) {
    throw 'GitHub returned an unexpected version reservation response.'
  }
  [IO.File]::AppendAllText($outputPath, "version=$version`n", [Text.UTF8Encoding]::new($false))
  Write-Output "Reserved official build version $version."
  return
}
throw 'Could not reserve an official build version after 20 concurrent collisions.'
