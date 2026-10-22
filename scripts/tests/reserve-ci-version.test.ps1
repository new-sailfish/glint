#Requires -Version 7.0
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'reserve-ci-version.ps1'
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixtureDirectory = Join-Path $temporaryParent ('glint-ci-version-' + [guid]::NewGuid().ToString('N'))
$environmentNames = @('GH_TOKEN', 'GITHUB_TOKEN', 'GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_EVENT_NAME', 'GITHUB_OUTPUT')
$originalEnvironment = @{}
foreach ($name in $environmentNames) { $originalEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
$failures = [Collections.Generic.List[string]]::new()
$testCount = 0

function Assert-True([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw $Message }
}

function New-Fixture([string]$Floor = '0.1.0', [string[]]$Tags = @()) {
  $root = Join-Path $fixtureDirectory ([guid]::NewGuid().ToString('N'))
  $scripts = Join-Path $root 'scripts'
  New-Item -ItemType Directory -Path $scripts | Out-Null
  Assert-True (Test-Path -LiteralPath $source) 'CI version reservation implementation is missing.'
  Copy-Item -LiteralPath $source -Destination (Join-Path $scripts 'reserve-ci-version.ps1')
  Set-Content -LiteralPath (Join-Path $root 'VERSION') -Value $Floor
  $records = [Collections.Generic.List[object]]::new()
  foreach ($tag in $Tags) { $records.Add([pscustomobject]@{ name = $tag; commit = @{ sha = 'a' * 40 } }) }
  return [pscustomobject]@{
    Script = Join-Path $scripts 'reserve-ci-version.ps1'
    OutputFile = Join-Path $root 'github-output'
    Tags = $records
    Requests = [Collections.Generic.List[object]]::new()
    FailureStatus = 0
    FailureMethod = 'Post'
    FailureMessage = 'fixture request denied'
    CollisionCount = 0
    MissingCollisionRef = $false
    StaleListing = $false
    InvalidCreatedRef = $false
    TransportFailure = $false
    InvalidListing = $false
  }
}

function Throw-ApiError([int]$Status, [string]$Message) {
  $exception = [Exception]::new("Fixture transport failed with $env:GH_TOKEN")
  $exception | Add-Member -NotePropertyName Response -NotePropertyValue ([pscustomobject]@{ StatusCode = $Status })
  $record = [Management.Automation.ErrorRecord]::new($exception, 'FixtureApiFailure', 'InvalidOperation', $null)
  $record.ErrorDetails = [Management.Automation.ErrorDetails]::new((@{ message = $Message } | ConvertTo-Json -Compress))
  throw $record
}

# Shadow only the transport command in the script's parent scope. Unexpected
# requests fail here; this suite never contacts GitHub or uses real credentials.
function Invoke-RestMethod {
  [CmdletBinding()]
  param([string]$Uri, [string]$Method = 'Get', [hashtable]$Headers, [string]$Body,
    [string]$ContentType, [int]$TimeoutSec)
  Assert-True ($Uri.StartsWith('https://api.github.com/repos/fixture/glint/')) 'Unexpected API destination.'
  Assert-True ($Headers.Authorization -eq 'Bearer fixture-secret-token') 'Authorization header was omitted.'
  $global:GlintCiVersionTestApi.Requests.Add([pscustomobject]@{ Method = $Method; Uri = $Uri; Body = $Body })
  if ($global:GlintCiVersionTestApi.TransportFailure) { throw "Fixture timeout with $env:GH_TOKEN" }
  if ($global:GlintCiVersionTestApi.FailureStatus -and $Method -eq $global:GlintCiVersionTestApi.FailureMethod) {
    Throw-ApiError $global:GlintCiVersionTestApi.FailureStatus $global:GlintCiVersionTestApi.FailureMessage
  }
  if ($Method -eq 'Get' -and $Uri -match '/tags\?per_page=100&page=(\d+)$') {
    if ($global:GlintCiVersionTestApi.InvalidListing) { return [pscustomobject]@{ unexpected = 'value' } }
    $page = [int]$Matches[1]
    $visible = @($global:GlintCiVersionTestApi.Tags | Where-Object {
        -not $global:GlintCiVersionTestApi.StaleListing -or $_.commit.sha -eq ('a' * 40)
      })
    # Invoke-RestMethod writes a JSON array as one pipeline object, even when
    # empty. Preserve that shape so the fixture exercises actual REST behavior.
    Write-Output -NoEnumerate @($visible | Select-Object -Skip (($page - 1) * 100) -First 100)
    return
  }
  if ($Method -eq 'Post' -and $Uri.EndsWith('/git/refs')) {
    $request = $Body | ConvertFrom-Json
    Assert-True ($request.sha -eq ('b' * 40)) 'Reservation was not made at GITHUB_SHA.'
    Assert-True ($request.ref -match '^refs/tags/build-version/\d+\.\d+\.\d+$') 'Unexpected reservation ref.'
    $tagName = $request.ref.Substring('refs/tags/'.Length)
    if ($global:GlintCiVersionTestApi.CollisionCount -gt 0) {
      $global:GlintCiVersionTestApi.CollisionCount--
      if (-not $global:GlintCiVersionTestApi.MissingCollisionRef) {
        $global:GlintCiVersionTestApi.Tags.Add([pscustomobject]@{ name = $tagName; commit = @{ sha = 'c' * 40 } })
      }
      Throw-ApiError 422 'Reference already exists'
    }
    if (@($global:GlintCiVersionTestApi.Tags | Where-Object name -eq $tagName).Count) { Throw-ApiError 422 'Reference already exists' }
    $global:GlintCiVersionTestApi.Tags.Add([pscustomobject]@{ name = $tagName; commit = @{ sha = $request.sha } })
    $createdRef = if ($global:GlintCiVersionTestApi.InvalidCreatedRef) { 'refs/tags/wrong' } else { $request.ref }
    return [pscustomobject]@{ ref = $createdRef; object = @{ sha = $request.sha; type = 'commit' } }
  }
  if ($Method -eq 'Get' -and $Uri -match '/git/ref/tags/build-version/(\d+\.\d+\.\d+)$') {
    $tag = @($global:GlintCiVersionTestApi.Tags | Where-Object name -eq "build-version/$($Matches[1])")
    if ($tag.Count -eq 0) { Throw-ApiError 404 'Not Found' }
    return [pscustomobject]@{ ref = "refs/tags/$($tag[0].name)"; object = @{ sha = $tag[0].commit.sha; type = 'commit' } }
  }
  throw 'Unexpected fixture API request.'
}

function Invoke-Reservation($Fixture, [hashtable]$Environment = @{}) {
  $global:GlintCiVersionTestApi = $Fixture
  $env:GH_TOKEN = 'fixture-secret-token'
  $env:GITHUB_TOKEN = ''
  $env:GITHUB_REPOSITORY = 'fixture/glint'
  $env:GITHUB_SHA = 'b' * 40
  $env:GITHUB_EVENT_NAME = 'push'
  $env:GITHUB_OUTPUT = $Fixture.OutputFile
  foreach ($name in $Environment.Keys) { [Environment]::SetEnvironmentVariable($name, $Environment[$name]) }
  $failure = $null
  $output = [Collections.Generic.List[string]]::new()
  try { & $Fixture.Script *>&1 | ForEach-Object { $output.Add([string]$_) } } catch { $failure = $_ }
  $saved = if (Test-Path -LiteralPath $Fixture.OutputFile) { Get-Content -LiteralPath $Fixture.OutputFile -Raw } else { '' }
  return [pscustomobject]@{ Failure = $failure; Output = $output -join "`n"; Saved = $saved }
}

function Assert-Failure($Result, [string]$Pattern) {
  Assert-True ($null -ne $Result.Failure) 'Expected reservation to fail.'
  Assert-True ($Result.Failure.ToString() -match $Pattern) "Unexpected failure: $($Result.Failure)"
  Assert-True ($Result.Saved -eq '') 'Failed reservation published a version.'
  Assert-True (($Result.Output + $Result.Failure.ToString()) -notmatch 'fixture-secret-token') 'Failure exposed the token.'
}

function Test-Case([string]$Name, [scriptblock]$Action) {
  $script:testCount++
  try { & $Action; Write-Output "PASS $Name" } catch {
    $script:failures.Add("${Name}: $($_.Exception.Message)")
    Write-Output "FAIL ${Name}: $($_.Exception.Message)"
  }
}

try {
  New-Item -ItemType Directory -Path $fixtureDirectory | Out-Null
  Test-Case 'first official build advances the floor and preserves existing outputs' {
    $fixture = New-Fixture
    Set-Content -LiteralPath $fixture.OutputFile -Value 'existing=value'
    $result = Invoke-Reservation $fixture
    Assert-True ($null -eq $result.Failure) "Reservation failed: $($result.Failure)"
    Assert-True ($result.Saved -match '(?m)^version=0\.1\.1\r?$') 'First official version was not 0.1.1.'
    Assert-True ($result.Saved -match 'existing=value') 'Existing step outputs were overwritten.'
    Assert-True (($result.Output + $result.Saved) -notmatch 'fixture-secret-token') 'Token leaked.'
  }
  Test-Case 'remote patch maximum wins independently of tag order and other series' {
    $fixture = New-Fixture '0.1.4' @('build-version/0.1.9', 'v9.0.0', 'build-version/2.0.99', 'build-version/0.1.3')
    $result = Invoke-Reservation $fixture
    Assert-True ($result.Saved.Trim() -eq 'version=0.1.10') "Incorrect maximum: $($result.Failure)"
  }
  Test-Case 'new major/minor series starts from its own floor' {
    $result = Invoke-Reservation (New-Fixture '0.2.7' @('build-version/0.1.65535'))
    Assert-True ($result.Saved.Trim() -eq 'version=0.2.8') 'Another series affected the new counter.'
  }
  Test-Case 'a raised VERSION floor wins over existing official versions' {
    $result = Invoke-Reservation (New-Fixture '0.1.50' @('build-version/0.1.40'))
    Assert-True ($result.Saved.Trim() -eq 'version=0.1.51') 'VERSION floor was ignored.'
  }
  Test-Case 'rerunning the same SHA consumes a fresh version' {
    $fixture = New-Fixture
    $first = Invoke-Reservation $fixture
    $second = Invoke-Reservation $fixture
    Assert-True ($null -eq $first.Failure -and $null -eq $second.Failure) 'Sequential reservations failed.'
    Assert-True ($second.Saved -match 'version=0.1.1\s+version=0.1.2') 'Same-SHA rerun reused its version.'
  }
  Test-Case 'pagination includes a maximum beyond the first hundred tags' {
    $tags = @(1..100 | ForEach-Object { "release-note/$_" }) + @('build-version/0.1.199')
    $fixture = New-Fixture '0.1.0' $tags
    $result = Invoke-Reservation $fixture
    Assert-True ($result.Saved.Trim() -eq 'version=0.1.200') 'Paginated maximum was missed.'
    Assert-True ([bool]($fixture.Requests | Where-Object Uri -like '*page=2')) 'Second page was not read.'
  }
  foreach ($tagCount in @(0, 1, 99, 100, 101, 200, 201)) {
    Test-Case "REST array with $tagCount tags normalizes and paginates correctly" {
      $tags = @(for ($patch = 1; $patch -le $tagCount; $patch++) { "build-version/0.1.$patch" })
      $fixture = New-Fixture '0.1.0' $tags
      $result = Invoke-Reservation $fixture
      Assert-True ($null -eq $result.Failure) "REST-shaped reservation failed: $($result.Failure)"
      Assert-True ($result.Saved.Trim() -eq "version=0.1.$($tagCount + 1)") 'REST array maximum was not applied.'
      $pages = @($fixture.Requests | Where-Object Uri -match '/tags\?')
      $expectedPages = [Math]::Floor($tagCount / 100) + 1
      Assert-True ($pages.Count -eq $expectedPages) "Expected $expectedPages list pages; got $($pages.Count)."
      Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 1) 'Expected one reservation.'
    }
  }
  Test-Case 'a genuine collision retries above the consumed patch even with stale listing' {
    $fixture = New-Fixture
    $fixture.CollisionCount = 1
    $fixture.StaleListing = $true
    $result = Invoke-Reservation $fixture
    Assert-True ($result.Saved.Trim() -eq 'version=0.1.2') "Collision retry failed: $($result.Failure)"
    Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 2) 'Collision was not retried once.'
    Assert-True ([bool]($fixture.Requests | Where-Object Uri -like '*/git/ref/tags/build-version/0.1.1')) 'Collision was not verified.'
  }
  Test-Case 'a claimed collision without an existing ref fails closed' {
    $fixture = New-Fixture
    $fixture.CollisionCount = 1
    $fixture.MissingCollisionRef = $true
    $result = Invoke-Reservation $fixture
    Assert-Failure $result 'collision|HTTP 404'
    Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 1) 'Unverified collision was retried.'
  }
  Test-Case 'continuous contention fails after a bounded number of retries' {
    $fixture = New-Fixture
    $fixture.CollisionCount = 30
    Assert-Failure (Invoke-Reservation $fixture) '20 concurrent collisions'
    Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 20) 'Collision retries were not bounded.'
  }
  foreach ($status in @(401, 403, 422, 500)) {
    Test-Case "HTTP $status reservation failure is not retried or logged with credentials" {
      $fixture = New-Fixture
      $fixture.FailureStatus = $status
      $result = Invoke-Reservation $fixture
      Assert-Failure $result "HTTP $status"
      Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 1) 'Noncollision failure was retried.'
    }
  }
  Test-Case 'read failure stops before reservation' {
    $fixture = New-Fixture
    $fixture.FailureStatus = 403
    $fixture.FailureMethod = 'Get'
    $result = Invoke-Reservation $fixture
    Assert-Failure $result 'HTTP 403'
    Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 0) 'Read failure was ignored.'
  }
  Test-Case 'transport errors never expose request credentials' {
    $fixture = New-Fixture
    $fixture.TransportFailure = $true
    Assert-Failure (Invoke-Reservation $fixture) 'HTTP 0'
    Assert-True ($fixture.Requests.Count -eq 1) 'Transport failure was retried.'
  }
  Test-Case 'malformed API listing fails before reservation' {
    $fixture = New-Fixture
    $fixture.InvalidListing = $true
    Assert-Failure (Invoke-Reservation $fixture) 'invalid tag listing response'
    Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 0) 'Malformed listing was ignored.'
  }
  foreach ($invalid in @('0.1', 'v0.1.0', '0.01.0', '-1.0.0', '0.1.65536', '999999999999999999999.1.0')) {
    Test-Case "invalid VERSION $invalid fails before contacting GitHub" {
      $fixture = New-Fixture $invalid
      $result = Invoke-Reservation $fixture
      Assert-Failure $result 'VERSION|version'
      Assert-True ($fixture.Requests.Count -eq 0) 'Invalid floor contacted GitHub.'
    }
  }
  foreach ($invalid in @('build-version/0.1.bad', 'build-version/0.1.65536', 'build-version/0.01.2')) {
    Test-Case "invalid remote counter $invalid fails closed" {
      $fixture = New-Fixture '0.1.0' @($invalid)
      Assert-Failure (Invoke-Reservation $fixture) 'Remote build-version counter'
      Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 0) 'Malformed remote counter was ignored.'
    }
  }
  Test-Case 'patch exhaustion fails without creating a ref' {
    $fixture = New-Fixture '0.1.0' @('build-version/0.1.65535')
    Assert-Failure (Invoke-Reservation $fixture) 'exhaust|65535'
    Assert-True (@($fixture.Requests | Where-Object Method -eq 'Post').Count -eq 0) 'Patch overflow created a ref.'
  }
  Test-Case 'maximum Windows version components remain valid' {
    $result = Invoke-Reservation (New-Fixture '65535.65535.65534')
    Assert-True ($result.Saved.Trim() -eq 'version=65535.65535.65535') 'Valid maximum version failed.'
  }
  Test-Case 'GITHUB_TOKEN fallback supports the Actions token' {
    $result = Invoke-Reservation (New-Fixture) @{ GH_TOKEN = ''; GITHUB_TOKEN = 'fixture-secret-token' }
    Assert-True ($result.Saved.Trim() -eq 'version=0.1.1') 'GITHUB_TOKEN fallback failed.'
  }
  Test-Case 'manual workflow dispatch reserves an official version' {
    $result = Invoke-Reservation (New-Fixture) @{ GITHUB_EVENT_NAME = 'workflow_dispatch' }
    Assert-True ($result.Saved.Trim() -eq 'version=0.1.1') 'Manual dispatch failed.'
  }
  foreach ($eventName in @('pull_request', 'pull_request_target')) {
    Test-Case "$eventName cannot reserve an official version" {
      $fixture = New-Fixture
      Assert-Failure (Invoke-Reservation $fixture @{ GITHUB_EVENT_NAME = $eventName }) 'event|pull.request'
      Assert-True ($fixture.Requests.Count -eq 0) 'PR event contacted GitHub.'
    }
  }
  foreach ($missing in @('GH_TOKEN', 'GITHUB_REPOSITORY', 'GITHUB_SHA', 'GITHUB_OUTPUT')) {
    Test-Case "missing $missing fails before networking" {
      $fixture = New-Fixture
      Assert-Failure (Invoke-Reservation $fixture @{ $missing = '' }) 'required|GITHUB|token'
      Assert-True ($fixture.Requests.Count -eq 0) 'Missing context contacted GitHub.'
    }
  }
  Test-Case 'unexpected successful response is not published' {
    $fixture = New-Fixture
    $fixture.InvalidCreatedRef = $true
    Assert-Failure (Invoke-Reservation $fixture) 'response|ref'
  }
  foreach ($invalidContext in @(@{ GITHUB_REPOSITORY = 'fixture/glint/extra' }, @{ GITHUB_SHA = 'not-a-commit' })) {
    Test-Case "invalid context $($invalidContext.Keys) fails before networking" {
      $fixture = New-Fixture
      Assert-Failure (Invoke-Reservation $fixture $invalidContext) 'valid.*required'
      Assert-True ($fixture.Requests.Count -eq 0) 'Invalid context contacted GitHub.'
    }
  }
} finally {
  Remove-Variable -Name GlintCiVersionTestApi -Scope Global -ErrorAction SilentlyContinue
  foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $originalEnvironment[$name]) }
  $resolvedFixture = [IO.Path]::GetFullPath($fixtureDirectory)
  if (-not $resolvedFixture.StartsWith($temporaryParent.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar,
      [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test fixture cleanup path.' }
  if (Test-Path -LiteralPath $resolvedFixture) { Remove-Item -LiteralPath $resolvedFixture -Recurse -Force }
}
if ($failures.Count) { throw "$($failures.Count) of $testCount CI version reservation tests failed.`n$($failures -join "`n")" }
Write-Output "All $testCount CI version reservation tests passed."
