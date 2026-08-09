$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$currentVersion = '1.110'

$required = @(
    'MCST.sln',
    'MCST.Shared\MCST.Shared.vcxproj',
    'MCST.TrackerBridge\MCST.TrackerBridge.vcxproj',
    'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.vcxproj',
    'MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Host.txt',
    'MCST.Watchdog\MCST.Watchdog.vcxproj',
    'MCST.Watchdog\MCST.Watchdog.rc',
    'MCST.Tests\MCST.Tests.vcxproj',
    'README.md',
    'CODING_STANDARD.md',
    'RELEASE_NOTES.md',
    'BUILD_INFO.txt',
    'CHANGELOG.md',
    'Docs\INSTALLATION.md',
    'Docs\ARCHITECTURE.md',
    'Docs\DEVELOPER_GUIDE.md',
    'Docs\USER_GUIDE.md',
    'Docs\COMPATIBILITY.md',
    'MCST.TrackerBridgeHost\README.md',
    'Tools\UniversalApplicationMapper\README.md'
)

foreach ($item in $required) {
    $path = Join-Path $root $item
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required release item is missing: $item"
    }
}

$historicalChangelogs = @(Get-ChildItem -LiteralPath $root -File -Filter 'CHANGELOG_*.txt')
if ($historicalChangelogs.Count -ne 0) {
    throw 'Historical per-version CHANGELOG_*.txt files must not be included in the public production package.'
}

$solutionText = Get-Content -LiteralPath (Join-Path $root 'MCST.sln') -Raw
if ($solutionText -match 'Debug\|') {
    throw 'Debug configuration found in the production solution.'
}

Get-ChildItem -Path $root -Recurse -Filter *.vcxproj | ForEach-Object {
    [xml]$xml = Get-Content -LiteralPath $_.FullName
    $text = Get-Content -LiteralPath $_.FullName -Raw

    if ($text -match 'Debug\|') {
        throw "Debug configuration found in $($_.FullName)"
    }
    if ($text -notmatch '<RuntimeLibrary>MultiThreaded</RuntimeLibrary>') {
        throw "Static /MT runtime is not explicitly configured in $($_.FullName)"
    }

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

$watchdogProject = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\MCST.Watchdog.vcxproj') -Raw
if ($watchdogProject -notmatch '<WholeProgramOptimization>false</WholeProgramOptimization>' -or
    $watchdogProject -notmatch '<FunctionLevelLinking>false</FunctionLevelLinking>' -or
    $watchdogProject -notmatch '<IntrinsicFunctions>false</IntrinsicFunctions>' -or
    $watchdogProject -notmatch '<EnableCOMDATFolding>false</EnableCOMDATFolding>' -or
    $watchdogProject -notmatch '<OptimizeReferences>false</OptimizeReferences>') {
    throw 'Required MCST-Watchdog production Release settings have changed.'
}

$hostText = Get-Content -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Host.txt') -Raw
if ($hostText -notmatch [regex]::Escape('C:\MCExtras\MCST-TrackerBridge.dll')) {
    throw 'PowerLanguage host does not reference the production Tracker Bridge DLL path.'
}

$watchdogRc = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\MCST.Watchdog.rc') -Raw
$expectedProductVersion = 'VALUE "ProductVersion", "' + $currentVersion + '\0"'
if (-not $watchdogRc.Contains($expectedProductVersion)) {
    throw "Watchdog Windows product version is not $currentVersion."
}

$publicDocs = @(
    'README.md',
    'RELEASE_NOTES.md',
    'BUILD_INFO.txt',
    'CHANGELOG.md',
    'Docs\INSTALLATION.md',
    'Docs\USER_GUIDE.md',
    'Docs\DEVELOPER_GUIDE.md',
    'Docs\ARCHITECTURE.md',
    'Docs\COMPATIBILITY.md'
)
foreach ($doc in $publicDocs) {
    $text = Get-Content -LiteralPath (Join-Path $root $doc) -Raw
    if ($text -match 'MCST-StartupProbe' -or $text -match 'user-verified working 1\.108' -or $text -match 'C:\\MCBridge\\MCTrackerBridge\.dll') {
        throw "Internal development-history text found in public documentation: $doc"
    }
}

Write-Host "MCST $currentVersion production release tree validation passed." -ForegroundColor Green
