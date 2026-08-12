$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$currentVersion = '1.114-R2'
$currentBridgeBuild = 157
$minimumProductionBridgeBuild = 156
$currentProtocolVersion = 2

$required = @(
    'MCST.sln',
    'MCST.Shared\MCST.Shared.vcxproj',
    'MCST.Shared\MCBridgeProtocol.h',
    'MCST.TrackerBridge\MCST.TrackerBridge.vcxproj',
    'MCST.TrackerBridge\TrackerBridgeReader.h',
    'MCST.TrackerBridge\TrackerBridgeReader.cpp',
    'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.vcxproj',
    'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.rc',
    'MCST.TrackerBridgeHost\MCTrackerBridge.cpp',
    'MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Host.txt',
    'MCST.Watchdog\MCST.Watchdog.vcxproj',
    'MCST.Watchdog\MCST.Watchdog.rc',
    'MCST.Watchdog\main.cpp',
    'MCST.Watchdog\CompatibilityManager.cpp',
    'MCST.Watchdog\MultiChartsVersionDetector.cpp',
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
    'Docs\POSITION_CURRENCY_RESEARCH.md',
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

$watchdogMain = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\main.cpp') -Raw
if ($watchdogMain -match 'RECENT ACTIVITY') {
    throw 'The redundant RECENT ACTIVITY Dashboard section must not be present in the production UI.'
}
if ($watchdogMain -match [regex]::Escape('std::wstring message = diagnostic;')) {
    throw 'Tracker Capture still contains the local message variable that shadows WindowProc message.'
}
if ($watchdogMain -notmatch 'Tracker Capture' -or
    $watchdogMain -notmatch 'Position CCY' -or
    $watchdogMain -notmatch 'Open Compat' -or
    $watchdogMain -notmatch 'Reload Compat') {
    throw 'Required Tracker compatibility and Position Currency Developer controls are missing.'
}
if ($watchdogMain -notmatch 'MCST_Position_Currency_Reference_' -or
    $watchdogMain -notmatch 'CapturePositionCurrencyResearch') {
    throw 'Position Currency research reference/capture workflow is missing from Watchdog.'
}
if ($watchdogMain -notmatch 'configRevision' -or
    $watchdogMain -notmatch 'staleConfigurationResult' -or
    $watchdogMain -notmatch 'CollectStatus\(configSnapshot, configRevision, forceAutoTradingRefresh\)') {
    throw 'Configuration-generation reload protection is missing from the Watchdog refresh path.'
}

$appConfigSource = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\AppConfig.cpp') -Raw
if ($appConfigSource -notmatch 'RefreshIniProfileCache' -or
    $appConfigSource -notmatch 'WritePrivateProfileStringW\(nullptr, nullptr, nullptr, path\.c_str\(\)\)') {
    throw 'INI profile cache refresh support is missing from AppConfig.'
}

$statusReportSource = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\StatusReport.cpp') -Raw
if ($statusReportSource -notmatch 'AppendAlignedSectionRows' -or
    $statusReportSource -notmatch 'AppendOpenPositionsTable' -or
    $statusReportSource -notmatch 'Native Value' -or
    $statusReportSource -notmatch 'TOTALS NOT CALCULATED - position and P/L currency normalization is under research' -or
    $statusReportSource -notmatch 'row\[7\] = source\.size\(\) > 6 \? source\[6\]' -or
    $statusReportSource -notmatch 'row\[8\] = source\.size\(\) > 7 \? source\[7\]' -or
    $statusReportSource -notmatch 'std::fabs\(quantity\) \* averagePrice' -or
    $statusReportSource -notmatch 'AppendAlignedSectionRows\(out, snapshot\.recentLogs, true\)') {
    throw 'Research-safe aligned Tracker report formatting or Open Positions field mapping is missing.'
}
if ($statusReportSource -match 'totalPositionValue' -or $statusReportSource -match 'totalOpenPl') {
    throw 'Research build must not aggregate unnormalized multi-currency Open Positions totals.'
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
if ($hostText -notmatch "Internal bridge build: V$currentBridgeBuild") {
    throw "PowerLanguage host does not identify Tracker Bridge internal build V$currentBridgeBuild."
}

$watchdogRc = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\MCST.Watchdog.rc') -Raw
$expectedProductVersion = 'VALUE "ProductVersion", "' + $currentVersion + '\0"'
if (-not $watchdogRc.Contains($expectedProductVersion)) {
    throw "Watchdog Windows product version is not $currentVersion."
}

$bridgeSource = Get-Content -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\MCTrackerBridge.cpp') -Raw
if ($bridgeSource -match [regex]::Escape('return (std::filesystem::path(path).parent_path()')) {
    throw 'Invalid vector-to-filesystem::path construction found in Tracker Bridge Host compatibility path.'
}
if ($bridgeSource -notmatch "constexpr int kBridgeVersion = $currentBridgeBuild;") {
    throw "Tracker Bridge internal source build is not V$currentBridgeBuild."
}
if ($bridgeSource -notmatch 'WritePositionCurrencyDirectResearch' -or
    $bridgeSource -notmatch 'positions_records_10E0' -or
    $bridgeSource -notmatch 'unknown_functions_called=no') {
    throw 'Focused Position Currency direct research implementation is missing.'
}
if ($bridgeSource -match 'required CATPTTabView or ATOnPTracker module anchor was not found') {
    throw 'Ambiguous legacy Tracker read diagnostic is still present.'
}
foreach ($requiredTrackerToken in @(
    'atonptracker_pe_timestamp',
    'atonptracker_image_size',
    'tracker_tabview_vtable_rva',
    'tracker_accounts_page_offset',
    'tracker_open_positions_page_offset',
    'tracker_logs_page_offset',
    'tracker_grid_member_offset',
    'tracker_rows_offset_1',
    'tracker_rows_offset_2',
    'tracker_gettext_slot',
    'tracker_flexgrid_vtable_rva',
    'tracker_gettext_rva',
    'Candidate.ATOnPTracker-'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($requiredTrackerToken)) {
        throw "Tracker compatibility implementation token is missing: $requiredTrackerToken"
    }
}

$bridgeRc = Get-Content -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.rc') -Raw
if ($bridgeRc -notmatch "internal bridge build V$currentBridgeBuild" -or
    $bridgeRc -notmatch "1\.0\.$currentBridgeBuild\.0") {
    throw "Tracker Bridge version resource is not aligned with internal build V$currentBridgeBuild."
}

$readerHeader = Get-Content -LiteralPath (Join-Path $root 'MCST.TrackerBridge\TrackerBridgeReader.h') -Raw
if ($readerHeader -notmatch "kTrackerBridgeInternalBuildVersion = $minimumProductionBridgeBuild") {
    throw "TrackerBridgeReader production minimum is not V$minimumProductionBridgeBuild."
}
if ($readerHeader -notmatch "kPositionCurrencyResearchBridgeVersion = $currentBridgeBuild") {
    throw "Position Currency research Bridge requirement is not V$currentBridgeBuild."
}

$readerSource = Get-Content -LiteralPath (Join-Path $root 'MCST.TrackerBridge\TrackerBridgeReader.cpp') -Raw
if ($readerSource -notmatch 'snapshot\.bridgeVersion < kTrackerBridgeInternalBuildVersion') {
    throw 'Watchdog-side Tracker reader does not enforce the required Bridge internal build.'
}
if ($readerSource -notmatch 'CapturePositionCurrencyResearch' -or
    $readerSource -notmatch 'CapturePositionCurrencyDirectResearch') {
    throw 'Position Currency direct-research Bridge wrapper is missing.'
}

$protocolHeader = Get-Content -LiteralPath (Join-Path $root 'MCST.Shared\MCBridgeProtocol.h') -Raw
if ($protocolHeader -notmatch "kProtocolVersion = $currentProtocolVersion") {
    throw "Bridge protocol is not V$currentProtocolVersion."
}
if ($protocolHeader -notmatch 'CapturePositionCurrencyDirectResearch = 50') {
    throw 'Bridge Protocol V2 additive Position Currency research command 50 is missing.'
}

$versionDetector = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\MultiChartsVersionDetector.cpp') -Raw
foreach ($requiredDetectedKey in @(
    'atonptracker_pe_timestamp',
    'atonptracker_image_size',
    'autotrading_compatibility_profile',
    'tracker_compatibility_profile'
)) {
    if ($versionDetector -notmatch [regex]::Escape($requiredDetectedKey)) {
        throw "DetectedMultiCharts output is missing: $requiredDetectedKey"
    }
}

$publicCurrentDocs = @(
    'README.md',
    'RELEASE_NOTES.md',
    'BUILD_INFO.txt',
    'Docs\INSTALLATION.md',
    'Docs\USER_GUIDE.md',
    'Docs\DEVELOPER_GUIDE.md',
    'Docs\ARCHITECTURE.md',
    'Docs\COMPATIBILITY.md',
    'Docs\POSITION_CURRENCY_RESEARCH.md',
    'MCST.TrackerBridgeHost\README.md'
)
foreach ($doc in $publicCurrentDocs) {
    $text = Get-Content -LiteralPath (Join-Path $root $doc) -Raw
    if ($text -match 'MCST-StartupProbe' -or
        $text -match 'user-verified working 1\.108' -or
        $text -match 'C:\\MCBridge\\MCTrackerBridge\.dll') {
        throw "Internal development-history text found in public documentation: $doc"
    }
}

$releaseNotes = Get-Content -LiteralPath (Join-Path $root 'RELEASE_NOTES.md') -Raw
$escapedCurrentVersion = [regex]::Escape($currentVersion)
if ($releaseNotes -notmatch "MCST $escapedCurrentVersion Position Currency Direct Research" -or
    $releaseNotes -notmatch "Internal build:\s+V$currentBridgeBuild") {
    throw "Research notes do not identify MCST $currentVersion and Tracker Bridge V$currentBridgeBuild."
}

Write-Host "MCST $currentVersion Position Currency direct-research tree validation passed." -ForegroundColor Green
