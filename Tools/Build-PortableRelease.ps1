[CmdletBinding()]
param(
    [string]$OutputDirectory = 'dist',
    [string]$ExpectedTag = ''
)

$ErrorActionPreference = 'Stop'

$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$version = '1.20.15'
$releaseTag = "v$version"
$packageName = "MCST-Watchdog-$version-Windows-x64"

if ($ExpectedTag -and $ExpectedTag -ne $releaseTag) {
    throw "Tag '$ExpectedTag' does not match package version '$releaseTag'."
}

$outputPath = if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    [IO.Path]::GetFullPath($OutputDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
}

if ($outputPath -eq $root) {
    throw 'OutputDirectory must not be the source root.'
}

$stagingParent = Join-Path $root 'obj\PortableRelease'
$stagingRoot = Join-Path $stagingParent $packageName
$runtimeRoot = Join-Path $stagingRoot 'MCExtras'
$powerLanguageRoot = Join-Path $stagingRoot 'PowerLanguage'
$zipPath = Join-Path $outputPath "$packageName.zip"
$checksumPath = Join-Path $outputPath "MCST-Watchdog-$version-SHA256SUMS.txt"

$files = [ordered]@{
    'bin\Release\MCST-Watchdog.exe' = 'MCExtras\MCST-Watchdog.exe'
    'bin\Release\MCST-TrackerBridge.dll' = 'MCExtras\MCST-TrackerBridge.dll'
    'MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Host.txt' = 'PowerLanguage\MCST_Tracker_Bridge_Host.txt'
    'MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Stop.txt' = 'PowerLanguage\MCST_Tracker_Bridge_Stop.txt'
    'Examples\MCST-Watchdog.ini.example' = 'Examples\MCST-Watchdog.ini.example'
    'Examples\MCST-Compatibility.ini.example' = 'Examples\MCST-Compatibility.ini.example'
    'Docs\PORTABLE_INSTALL.md' = 'INSTALL.md'
    'RELEASE_NOTES.md' = 'RELEASE_NOTES.md'
    'COVERED_QUEUE_PROBE_TRIAL.txt' = 'COVERED_QUEUE_PROBE_TRIAL.txt'
    'LICENSE' = 'LICENSE'
}

foreach ($sourceRelative in $files.Keys) {
    $source = Join-Path $root $sourceRelative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required portable-release file is missing: $sourceRelative"
    }
}

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $runtimeRoot, $powerLanguageRoot | Out-Null

try {
    foreach ($entry in $files.GetEnumerator()) {
        $source = Join-Path $root $entry.Key
        $destination = Join-Path $stagingRoot $entry.Value
        $destinationDirectory = Split-Path -Parent $destination
        New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }

    $forbidden = @(Get-ChildItem -LiteralPath $stagingRoot -Recurse -File | Where-Object {
        $_.Extension -in @('.ini', '.pdb', '.lib', '.obj', '.cpp', '.h') -or
        $_.Name -eq 'MCST-LogicTests.exe'
    })
    if ($forbidden.Count -ne 0) {
        throw "Forbidden development or user-configuration file entered the package: $($forbidden.FullName -join ', ')"
    }

    $manifest = @(
        "MCST portable package $version"
        'Built from Release x64 with static /MT runtime linkage.'
        'Licensed under the MIT License. Copyright (c) 2026 Mika Tättäläinen.'
        'The package contains only inert .ini.example templates; it contains no active INI files, credentials, PDB files, libraries, test executable, or source code.'
        ''
        'Files:'
    )
    Get-ChildItem -LiteralPath $stagingRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($stagingRoot.Length + 1).Replace('\', '/')
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
        $manifest += "$hash  $relative"
    }
    Set-Content -LiteralPath (Join-Path $stagingRoot 'PACKAGE_MANIFEST.txt') -Value $manifest -Encoding utf8

    Remove-Item -LiteralPath $zipPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path $stagingRoot -DestinationPath $zipPath -CompressionLevel Optimal

    $zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash.ToLowerInvariant()
    Set-Content -LiteralPath $checksumPath -Value "$zipHash  $([IO.Path]::GetFileName($zipPath))" -Encoding ascii

    Write-Host "Created $zipPath" -ForegroundColor Green
    Write-Host "Created $checksumPath" -ForegroundColor Green
} finally {
    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}
