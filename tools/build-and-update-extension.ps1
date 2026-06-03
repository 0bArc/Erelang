# Legacy name — forwards to install-erelang-extension.ps1 (always packages; stable VSIX name).

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'install-erelang-extension.ps1') @args
exit $LASTEXITCODE
