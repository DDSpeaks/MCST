$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$required = @(
    'MCST.sln',
    'MCST.Shared\MCST.Shared.vcxproj',
    'MCST.TrackerBridge\MCST.TrackerBridge.vcxproj',
    'MCST.Watchdog\MCST.Watchdog.vcxproj',
    'MCST.Tests\MCST.Tests.vcxproj',
    'README.md',
    'CODING_STANDARD.md',
    'RELEASE_NOTES.md',
    'BUILD_INFO.txt',
    'Docs\INSTALLATION.md',
    'Docs\ARCHITECTURE.md',
    'Docs\DEVELOPER_GUIDE.md',
    'Docs\USER_GUIDE.md',
    'Docs\COMPATIBILITY.md'
)

foreach ($item in $required) {
    $path = Join-Path $root $item
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required release item is missing: $item"
    }
}

Get-ChildItem -Path $root -Recurse -Filter *.vcxproj | ForEach-Object {
    [xml]$xml = Get-Content -LiteralPath $_.FullName
    $items = @{}
    $xml.Project.ItemGroup.ChildNodes | ForEach-Object {
        if ($_.Include) {
            $key = "$($_.Name)|$($_.Include)".ToLowerInvariant()
            if ($items.ContainsKey($key)) {
                throw "Duplicate project item in $($_.OwnerDocument.BaseURI): $key"
            }
            $items[$key] = $true
        }
    }
}

Write-Host 'MCST release tree validation passed.' -ForegroundColor Green
