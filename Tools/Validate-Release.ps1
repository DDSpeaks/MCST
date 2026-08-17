$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$currentVersion = '1.114-R20'
$currentBridgeBuild = 172
$minimumProductionBridgeBuild = 156
$positionCurrencyResearchBridgeBuild = 171
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
    'BUILD_VALIDATION_R20.txt',
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
if ($watchdogMain -notmatch [regex]::Escape($currentVersion)) {
    throw "Watchdog main window does not identify $currentVersion."
}
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
foreach ($recoveryToken in @(
    'IsCompleteTrackerSnapshot',
    'IsRecoverableTrackerSnapshotFailure',
    'lastGoodTrackerSnapshot',
    'trackerDataStale',
    'liveSnapshot.recentLogs',
    'Bridge recovery:',
    'Snapshot attempts:'
)) {
    if ($watchdogMain -notmatch [regex]::Escape($recoveryToken)) {
        throw "Watchdog Tracker recovery token is missing: $recoveryToken"
    }
}

$appConfigSource = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\AppConfig.cpp') -Raw
if ($appConfigSource -notmatch [regex]::Escape($currentVersion) -or
    $appConfigSource -notmatch 'RefreshIniProfileCache' -or
    $appConfigSource -notmatch 'WritePrivateProfileStringW\(nullptr, nullptr, nullptr, path\.c_str\(\)\)') {
    throw 'INI profile cache refresh support is missing from AppConfig.'
}

$statusReportSource = Get-Content -LiteralPath (Join-Path $root 'MCST.Watchdog\StatusReport.cpp') -Raw
if ($statusReportSource -notmatch [regex]::Escape($currentVersion) -or
    $statusReportSource -notmatch 'AppendAlignedSectionRows' -or
    $statusReportSource -notmatch 'AppendOpenPositionsTable' -or
    $statusReportSource -notmatch 'Native Value' -or
    $statusReportSource -notmatch 'TryExtractKnownCurrency' -or
    $statusReportSource -notmatch 'openPlTotalsByCurrency' -or
    $statusReportSource -notmatch 'Total Open P/L' -or
    $statusReportSource -notmatch 'splitAlignedTableRow' -or
    $statusReportSource -notmatch 'openPositionCellHtml' -or
    $statusReportSource -notmatch 'mcst-horizontal-scroll' -or
    $statusReportSource -notmatch 'mcst-open-positions' -or
    $statusReportSource -notmatch 'width=device-width,initial-scale=1.0' -or
    $statusReportSource -notmatch 'overflow-x:auto' -or
    $statusReportSource -notmatch '-webkit-text-size-adjust:100%' -or
    $statusReportSource -notmatch 'font-size:15px !important' -or
    $statusReportSource -notmatch 'mcst-stale-tracker-warning' -or
    $statusReportSource -notmatch 'WARNING: STALE TRACKER TABLE DATA' -or
    $statusReportSource -notmatch '#15803D' -or
    $statusReportSource -notmatch '#B4232A' -or
    $statusReportSource -notmatch 'row\[7\] = source\.size\(\) > 6 \? source\[6\]' -or
    $statusReportSource -notmatch 'row\[8\] = source\.size\(\) > 7 \? source\[7\]' -or
    $statusReportSource -notmatch 'std::fabs\(quantity\) \* averagePrice' -or
    $statusReportSource -notmatch 'AppendAlignedSectionRows\(out, snapshot\.recentLogs, true\)') {
    throw 'Known-currency Tracker totals or aligned Open Positions field mapping is missing.'
}
if ($statusReportSource -match 'totalPositionValue') {
    throw 'Native Position Value must not be aggregated while Average Price currency is unknown.'
}
if ($statusReportSource -match 'NATIVE VALUE TOTAL') {
    throw 'The removed Native Value total notice must not appear in the Status Report.'
}

$testsSource = Get-Content -LiteralPath (Join-Path $root 'MCST.Tests\main.cpp') -Raw
$testsProject = Get-Content -LiteralPath (Join-Path $root 'MCST.Tests\MCST.Tests.vcxproj') -Raw
if ($testsProject -notmatch 'StatusReport\.cpp' -or
    $testsSource -notmatch 'EUR \+8,25' -or
    $testsSource -notmatch 'Total Open P/L' -or
    $testsSource -notmatch 'OPEN P/L ROWS NOT TOTALLED: 2' -or
    $testsSource -notmatch '\$ 3,00' -or
    $testsSource -notmatch 'USD \\u20ac 4,00' -or
    $testsSource -notmatch '#15803D' -or
    $testsSource -notmatch '#B4232A' -or
    $testsSource -notmatch 'mcst-horizontal-scroll' -or
    $testsSource -notmatch 'mcst-open-positions' -or
    $testsSource -notmatch 'width=device-width,initial-scale=1\.0' -or
    $testsSource -notmatch 'overflow-x:auto' -or
    $testsSource -notmatch 'font-size:15px !important' -or
    $testsSource -notmatch 'mcst-stale-tracker-warning' -or
    $testsSource -notmatch 'WARNING: STALE TRACKER TABLE DATA' -or
    $testsSource -notmatch 'EUR \+106,68') {
    throw 'Responsive Open Positions placement, size, or color regression test is missing.'
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
if ($watchdogRc -notmatch 'FILEVERSION 1,114,20,0' -or
    $watchdogRc -notmatch 'PRODUCTVERSION 1,114,20,0') {
    throw 'Watchdog numeric Windows version resource is not aligned with 1.114-R20.'
}

$bridgeSource = Get-Content -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\MCTrackerBridge.cpp') -Raw
if ($bridgeSource -match [regex]::Escape('return (std::filesystem::path(path).parent_path()')) {
    throw 'Invalid vector-to-filesystem::path construction found in Tracker Bridge Host compatibility path.'
}
if ($bridgeSource -notmatch "constexpr int kBridgeVersion = $currentBridgeBuild;") {
    throw "Tracker Bridge internal source build is not V$currentBridgeBuild."
}
foreach ($recoveryToken in @(
    'InvalidateTabViewCaches',
    'forceFreshScan',
    'BuildV172StatusReportSnapshotWithRecovery',
    'recovery_attempted',
    'recovery_result',
    'kTabViewRecoveryCooldownMs = 30000',
    'g_tabViewRecoveryModeActive',
    'pagesRead < 3'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($recoveryToken)) {
        throw "Tracker Bridge recovery token is missing: $recoveryToken"
    }
}
if ($bridgeSource -notmatch 'bool WritePositionCurrencyDirectResearch\(' -or
    $bridgeSource -notmatch 'R16VerifyPositionInterfaceFingerprint' -or
    $bridgeSource -notmatch 'R16ReadMsvcWstring' -or
    $bridgeSource -notmatch 'R16ScanDirectVtableValues' -or
    $bridgeSource -notmatch 'R16ScanPointerReferences' -or
    $bridgeSource -notmatch 'R16MatchRowsToObjects' -or
    $bridgeSource -notmatch 'kExpectedAtCenterProxySize = 7303168u' -or
    $bridgeSource -notmatch 'kVtableRva = 0x44D518' -or
    $bridgeSource -notmatch 'object \+ 0x1A8' -or
    $bridgeSource -notmatch 'object \+ 0x1B0' -or
    $bridgeSource -notmatch 'object \+ 0x1B8' -or
    $bridgeSource -notmatch 'object \+ 0x1C8' -or
    $bridgeSource -notmatch 'R16ReadMsvcWstring\(object, 0x308\)' -or
    $bridgeSource -notmatch 'R16ReadMsvcWstring\(object, 0x328\)' -or
    $bridgeSource -notmatch 'MCST_Position_Currency_R16_Checkpoint' -or
    $bridgeSource -notmatch 'unknown_functions_called=no') {
    throw 'Position Currency R16 targeted position-interface verification is missing.'
}
if ($bridgeSource -match 'positions_records_10E0=' -or
    $bridgeSource -match 'recordBase \+ 0x68') {
    throw 'R6 must not retain the rejected fixed +0x10E0/fixed-record correlation implementation.'
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
if ($readerHeader -notmatch "kPositionCurrencyResearchBridgeVersion = $positionCurrencyResearchBridgeBuild") {
    throw "Position Currency research Bridge requirement is not V$positionCurrencyResearchBridgeBuild."
}

$readerSource = Get-Content -LiteralPath (Join-Path $root 'MCST.TrackerBridge\TrackerBridgeReader.cpp') -Raw
if ($readerSource -notmatch 'snapshot\.bridgeVersion < kTrackerBridgeInternalBuildVersion') {
    throw 'Watchdog-side Tracker reader does not enforce the required Bridge internal build.'
}
if ($readerSource -notmatch 'CapturePositionCurrencyResearch' -or
    $readerSource -notmatch 'CapturePositionCurrencyDirectResearch') {
    throw 'Position Currency direct-research Bridge wrapper is missing.'
}
if ($readerSource -notmatch 'recovery_attempted' -or
    $readerSource -notmatch 'recovery_result') {
    throw 'Tracker Bridge recovery metadata parsing is missing.'
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
if ($releaseNotes -notmatch "MCST $escapedCurrentVersion Tracker Snapshot Self-Recovery" -or
    $releaseNotes -notmatch "internal build:\s+V$currentBridgeBuild" -or
    $releaseNotes -notmatch 'mobile Status Report correction') {
    throw "Release notes do not identify MCST $currentVersion, Tracker Bridge V$currentBridgeBuild, and the retained mobile report correction."
}

Write-Host "MCST $currentVersion Tracker self-recovery release validation passed." -ForegroundColor Green
