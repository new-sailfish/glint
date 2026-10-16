param(
  [Parameter(Mandatory = $true)][string]$Binary
)

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) { throw "dumpbin.exe is required (run from a VS Developer PowerShell)." }
$imports = & $dumpbin.Source /DEPENDENTS $Binary 2>&1
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $Binary`n$imports" }
$unexpected = $imports | Select-String -Pattern 'VCRUNTIME|MSVCP|ucrtbase|libgcc|libstdc\+\+|boost|Qt[0-9]|zlib|icu' -CaseSensitive:$false
if ($unexpected) {
  Write-Error "Unexpected dynamic runtime/third-party import detected:"
  $unexpected | ForEach-Object { Write-Error $_.Line }
  exit 1
}
Write-Output "Import check passed: $Binary"
