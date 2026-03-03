param(
    [switch]$DryRun,
    [string]$SetVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$versionHeader = Join-Path $root "include\erelang\version.hpp"
$plannedManifest = Join-Path $root "src\info\planned.obmanifest"

if (-not (Test-Path $versionHeader)) {
    throw "Version header not found: $versionHeader"
}

function Parse-VersionFromHeader([string]$content) {
    $maj = [regex]::Match($content, '#define\s+ERELANG_VERSION_MAJOR\s+(\d+)')
    $min = [regex]::Match($content, '#define\s+ERELANG_VERSION_MINOR\s+(\d+)')
    $pat = [regex]::Match($content, '#define\s+ERELANG_VERSION_PATCH\s+(\d+)')
    if (-not ($maj.Success -and $min.Success -and $pat.Success)) {
        throw "Failed to parse version macros in version.hpp"
    }
    return [pscustomobject]@{
        Major = [int]$maj.Groups[1].Value
        Minor = [int]$min.Groups[1].Value
        Patch = [int]$pat.Groups[1].Value
    }
}

function Parse-VersionString([string]$v) {
    if ($v -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw "Invalid version format '$v'. Expected x.y.z"
    }
    return [pscustomobject]@{
        Major = [int]$Matches[1]
        Minor = [int]$Matches[2]
        Patch = [int]$Matches[3]
    }
}

function Format-Version($ver) {
    return "$($ver.Major).$($ver.Minor).$($ver.Patch)"
}

function Get-NextVersion($current) {
    $next = [pscustomobject]@{
        Major = $current.Major
        Minor = $current.Minor
        Patch = $current.Patch
    }

    # Early alpha ladder:
    # 0.0.1 -> ... -> 0.0.20, then roll to 0.1.0.
    # After that, default to normal patch bumps.
    if ($current.Major -eq 0 -and $current.Minor -eq 0) {
        $next.Patch++
        if ($next.Patch -gt 20) {
            $next.Minor = 1
            $next.Patch = 0
        }
        return $next
    }

    $next.Patch++
    return $next
}

$headerContent = Get-Content $versionHeader -Raw
$current = Parse-VersionFromHeader $headerContent
$target = if ($SetVersion) { Parse-VersionString $SetVersion } else { Get-NextVersion $current }

$targetString = Format-Version $target
$currentString = Format-Version $current

$updatedHeader = $headerContent `
    -replace '(?m)^#define\s+ERELANG_VERSION_MAJOR\s+\d+\s*$', "#define ERELANG_VERSION_MAJOR $($target.Major)" `
    -replace '(?m)^#define\s+ERELANG_VERSION_MINOR\s+\d+\s*$', "#define ERELANG_VERSION_MINOR $($target.Minor)" `
    -replace '(?m)^#define\s+ERELANG_VERSION_PATCH\s+\d+\s*$', "#define ERELANG_VERSION_PATCH $($target.Patch)" `
    -replace '(?m)^#define\s+ERELANG_VERSION_STRING\s+".*"\s*$', "#define ERELANG_VERSION_STRING ""$targetString"""

if ($DryRun) {
    Write-Host "Current version: $currentString"
    Write-Host "Target version : $targetString"
    Write-Host "[DryRun] Would update:"
    Write-Host " - $versionHeader"
    if (Test-Path $plannedManifest) {
        Write-Host " - $plannedManifest"
    }
    exit 0
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($versionHeader, $updatedHeader, $utf8NoBom)

if (Test-Path $plannedManifest) {
    $manifestContent = Get-Content $plannedManifest -Raw
    $manifestUpdated = $manifestContent -replace '(?m)^version=.*$', "version=$targetString"
    [System.IO.File]::WriteAllText($plannedManifest, $manifestUpdated, $utf8NoBom)
}

Write-Host "Bumped version: $currentString -> $targetString"
