$ErrorActionPreference = "Stop"

$target = Join-Path $PSScriptRoot "build-win"

if (Test-Path -LiteralPath $target) {
    Remove-Item -LiteralPath $target -Recurse -Force
    Write-Host "Deleted: $target"
} else {
    Write-Host "Not found: $target"
}
