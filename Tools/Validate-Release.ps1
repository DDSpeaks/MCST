$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$currentVersion = '1.20.17'
$currentBridgeBuild = 181
$minimumProductionBridgeBuild = 156
$positionCurrencyResearchBridgeBuild = 171
$currentProtocolVersion = 2

function Read-ReleaseText {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
    try {
        $text = $strictUtf8.GetString($bytes)
    }
    catch {
        $text = [System.Text.Encoding]::Default.GetString($bytes)
    }
    return $text.TrimStart([char]0xFEFF)
}

$required = @(
    'MCST.sln',
    '.gitignore',
    '.gitattributes',
    'MCST.Shared\MCST.Shared.vcxproj',
    'MCST.Shared\MCBridgeProtocol.h',
    'MCST.Shared\TrackerRecoveryPolicy.h',
    'MCST.TrackerBridge\MCST.TrackerBridge.vcxproj',
    'MCST.TrackerBridge\TrackerBridgeReader.h',
    'MCST.TrackerBridge\TrackerBridgeReader.cpp',
    'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.vcxproj',
    'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.rc',
    'MCST.TrackerBridgeHost\MCTrackerBridge.cpp',
    'MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Host.txt',
    'MCST.Watchdog\MCST.Watchdog.vcxproj',
    'MCST.Watchdog\MCST.Watchdog.rc',
    'MCST.Watchdog\resource.h',
    'MCST.Watchdog\Assets\WatchdogHeaderLogo.png',
    'MCST.Watchdog\main.cpp',
    'MCST.Watchdog\DeveloperHelpContent.h',
    'MCST.Watchdog\DeveloperHelpContent.cpp',
    'MCST.Watchdog\AutoTradingReader.cpp',
    'MCST.Watchdog\TrackerDateParser.h',
    'MCST.Watchdog\TrackerDateParser.cpp',
    'MCST.Watchdog\CompatibilityManager.cpp',
    'MCST.Watchdog\MultiChartsVersionDetector.cpp',
    'MCST.Watchdog\MultiChartsHealthMonitor.h',
    'MCST.Watchdog\MultiChartsHealthMonitor.cpp',
    'MCST.Watchdog\QueueStatusReader.h',
    'MCST.Watchdog\VisibleQueueWarning.h',
    'MCST.Watchdog\CoveredQueueProbe.h',
    'MCST.Shared\VisibleWarningPolicy.h',
    'MCST.Shared\ReportReadPolicy.h',
    'MCST.Shared\ActivityHistory.h',
    'MCST.Shared\OverallHeadlinePolicy.h',
    'MCST.Tests\MCST.Tests.vcxproj',
    'LICENSE',
    'README.md',
    'CODING_STANDARD.md',
    'RELEASE_NOTES.md',
    'BUILD_INFO.txt',
    'BUILD_VALIDATION_1.20.17.txt',
    'COVERED_QUEUE_PROBE_TRIAL.txt',
    'CHANGELOG.md',
    '.github\workflows\release.yml',
    'Examples\MCST-Watchdog.ini.example',
    'Examples\MCST-Compatibility.ini.example',
    'Docs\INSTALLATION.md',
    'Docs\ARCHITECTURE.md',
    'Docs\DEVELOPER_GUIDE.md',
    'Docs\USER_GUIDE.md',
    'Docs\COMPATIBILITY.md',
    'Docs\POSITION_CURRENCY_RESEARCH.md',
    'Docs\GITHUB_RELEASES.md',
    'Docs\FIRST_GITHUB_PUBLICATION.md',
    'Docs\PORTABLE_INSTALL.md',
    'MCST.TrackerBridgeHost\README.md',
    'Tools\UniversalApplicationMapper\README.md',
    'Tools\Build-PortableRelease.ps1',
    'Publish-GitHub-Release.ps1'
)

foreach ($item in $required) {
    $path = Join-Path $root $item
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required release item is missing: $item"
    }
}

$licenseText = Read-ReleaseText -Path (Join-Path $root 'LICENSE')
foreach ($licenseToken in @(
    'MIT License',
    'Copyright (c) 2026 Mika Tättäläinen',
    'Permission is hereby granted, free of charge',
    'THE SOFTWARE IS PROVIDED "AS IS"'
)) {
    if ($licenseText -notmatch [regex]::Escape($licenseToken)) {
        throw "MIT License contract token is missing: $licenseToken"
    }
}

$historicalChangelogs = @(Get-ChildItem -LiteralPath $root -File -Filter 'CHANGELOG_*.txt')
if ($historicalChangelogs.Count -ne 0) {
    throw 'Historical per-version CHANGELOG_*.txt files must not be included in the public production package.'
}

$activeIniFiles = @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.ini' | Where-Object {
    $_.FullName -notlike (Join-Path $root 'bin\*') -and
    $_.FullName -notlike (Join-Path $root 'obj\*') -and
    $_.FullName -notlike (Join-Path $root 'dist\*') -and
    $_.FullName -notlike (Join-Path $root '.git\*')
})
if ($activeIniFiles.Count -ne 0) {
    throw "Active INI files must not be committed to the publication source package: $($activeIniFiles.FullName -join ', ')"
}

$publicationTextExtensions = @('.cpp', '.c', '.h', '.md', '.txt', '.ps1', '.yml', '.sln', '.vcxproj', '.rc', '.def', '.example')
foreach ($publicationFile in Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in $publicationTextExtensions }) {
    $publicationText = Get-Content -Encoding UTF8 -LiteralPath $publicationFile.FullName -Raw
    foreach ($privateFixture in @(
        ('910792' + 'INET'),
        ('977015' + 'INET'),
        ('2015' + '4437'),
        ('ddspeaks' + '@gmail.com'),
        ('mcstockalerts.mika' + '@gmail.com')
    )) {
        if ($publicationText -match [regex]::Escape($privateFixture)) {
            throw "Private production fixture found in publication source: $privateFixture in $($publicationFile.FullName)"
        }
    }
}

$gitIgnore = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root '.gitignore') -Raw
foreach ($ignoreToken in @('bin/', 'obj/', 'dist/', '*.ini', '*.pfx', '*.key', 'MCST-Watchdog-StatusReport.*', 'MCST_Position_Currency_*')) {
    if ($gitIgnore -notmatch [regex]::Escape($ignoreToken)) {
        throw "Repository protection is missing from .gitignore: $ignoreToken"
    }
}

$watchdogExample = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'Examples\MCST-Watchdog.ini.example') -Raw
$compatibilityExample = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'Examples\MCST-Compatibility.ini.example') -Raw
if ($watchdogExample -notmatch 'REFERENCE TEMPLATE ONLY' -or
    $watchdogExample -notmatch 'enabled=false' -or
    $watchdogExample -notmatch '(?m)^smtp_password=\r?$' -or
    $compatibilityExample -notmatch 'REFERENCE TEMPLATE ONLY' -or
    $compatibilityExample -notmatch 'enabled=false' -or
    $compatibilityExample -notmatch 'UNVERIFIED') {
    throw 'Safe inert INI example contract is missing.'
}

$solutionText = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.sln') -Raw
if ($solutionText -match 'Debug\|') {
    throw 'Debug configuration found in the production solution.'
}

Get-ChildItem -Path $root -Recurse -Filter *.vcxproj | ForEach-Object {
    [xml]$xml = Get-Content -Encoding UTF8 -LiteralPath $_.FullName
    $text = Get-Content -Encoding UTF8 -LiteralPath $_.FullName -Raw

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

$watchdogMain = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\main.cpp') -Raw
$autoTradingReader = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\AutoTradingReader.cpp') -Raw
$compatibilityManager = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\CompatibilityManager.cpp') -Raw
foreach ($token in @(
    'AutoTrading Dynamic Research Session 0.578',
    'CompareDynamicResearchSnapshots',
    'Charting.dll PE timestamp',
    'image size:',
    'FULL MATCH WITH REVERSE VERIFICATION',
    'NO CANDIDATE FOUND',
    'Existing verified MC16 production compatibility remains unchanged'
)) {
    if ($autoTradingReader -notmatch [regex]::Escape($token)) {
        throw "The retained dynamic AutoTrading research contract is missing: $token"
    }
}
foreach ($mc16Token in @(
    'Profile.MC16-Charting-6A5684BF',
    'charting_pe_timestamp", L"0x6A5684BF',
    'charting_image_size", L"18493440',
    'strategy_vtable_rva", L"0xA457B8',
    'autotrading_offset", L"0x142'
)) {
    if ($compatibilityManager -notmatch [regex]::Escape($mc16Token)) {
        throw "The verified MC16 compatibility profile was changed or removed: $mc16Token"
    }
}
foreach ($mc17Token in @(
    'Profile.MC17-Charting-6AB57EE6',
    'charting_pe_timestamp", L"0x6AB57EE6',
    'charting_image_size", L"18624512',
    'strategy_vtable_rva", L"0xB74CF0',
    'autotrading_offset", L"0x18',
    '4/4 exact responses with both toggle directions'
)) {
    if ($compatibilityManager -notmatch [regex]::Escape($mc17Token)) {
        throw "The verified MC17 compatibility profile is missing or changed: $mc17Token"
    }
}
foreach ($processIdentityToken in @(
    'entry.th32ProcessID != currentProcessId',
    'IsMultiChartsExecutable(entry.szExeFile)',
    'lower == L"multicharts64.exe"',
    'lower == L"multicharts.exe"'
)) {
    if ($autoTradingReader -notmatch [regex]::Escape($processIdentityToken)) {
        throw "Strict MultiCharts process identification is missing: $processIdentityToken"
    }
}
if ($autoTradingReader -match 'title\.find\(L"multicharts"\).*processIds') {
    throw 'AutoTrading process discovery must not accept a process from its window title alone.'
}
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
    $watchdogMain -notmatch 'Reload Compat' -or
    $watchdogMain -notmatch 'kDeveloperHelpTopics' -or
    $watchdogMain -notmatch 'MCST-Watchdog Developer Mode Help') {
    throw 'Required Tracker compatibility and Position Currency Developer controls are missing.'
}
if ($watchdogMain -notmatch 'MCST_Position_Currency_Reference_' -or
    $watchdogMain -notmatch 'CapturePositionCurrencyResearch') {
    throw 'Position Currency research reference/capture workflow is missing from Watchdog.'
}
if ($watchdogMain -notmatch 'kCheckDeveloperMode' -or
    $watchdogMain -notmatch 'SaveDeveloperModeEnabled' -or
    $watchdogMain -notmatch 'BS_AUTOCHECKBOX') {
    throw 'Persistent Dashboard Developer mode checkbox is missing.'
}
if ($watchdogMain -notmatch 'DrawDeveloperToolsPanel' -or
    $watchdogMain -notmatch 'DrawLatestActivityRows' -or
    $watchdogMain -notmatch 'status.activity' -or
    $watchdogMain -notmatch 'constexpr int latestRowHeight = 20' -or
    $watchdogMain -notmatch 'labelRight, y \+ rowHeight \}, item.text' -or
    $watchdogMain -notmatch 'valueLeft = rowLayout.stateLeft' -or
    $watchdogMain -notmatch 'detailLeft = rowLayout.descriptionLeft' -or
    $watchdogMain -notmatch 'detailLeft - 10, y \+ rowHeight \}, FormatLocalTime\(item.time\)' -or
    $watchdogMain -notmatch 'detailLeft, y, width - 28, y \+ rowHeight \}, item.text' -or
    $watchdogMain -notmatch 'latestValueLeft = latestLayout.stateLeft' -or
    $watchdogMain -notmatch 'latestDetailLeft = latestLayout.descriptionLeft' -or
    $watchdogMain -notmatch 'MergeActivityHistory\(g_app.status.activity, previousActivity, 10\)' -or
    $watchdogMain -notmatch 'productionButtonY - 10' -or
    $watchdogMain -notmatch 'DEVELOPER TOOLS  ·  READ-ONLY DIAGNOSTICS' -or
    $watchdogMain -notmatch 'Charts found' -or
    $watchdogMain -match 'Objects found') {
    throw 'The retained activity deduplication/alignment, Developer panel, or user-facing AutoTrading chart terminology is missing.'
}
$activityHistory = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Shared\ActivityHistory.h') -Raw
if ($activityHistory -notmatch 'IsSameActivity' -or
    $activityHistory -notmatch 'std::stable_sort' -or
    $activityHistory -notmatch 'left.time == right.time' -or
    $activityHistory -notmatch 'left.state == right.state' -or
    $activityHistory -notmatch 'left.text == right.text') {
    throw 'The retained activity-history deduplication contract is missing.'
}
$headlinePolicy = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Shared\OverallHeadlinePolicy.h') -Raw
foreach ($headline in @('SYSTEM HEALTHY', 'ATTENTION REQUIRED', 'CRITICAL CONDITION', 'CHECK INCOMPLETE', 'INITIALIZING')) {
    if ($headlinePolicy -notmatch [regex]::Escape($headline)) {
        throw "Overall Dashboard headline is missing: $headline"
    }
}
if ($watchdogMain -match 'CHECKED ITEMS OK' -or
    $watchdogMain -notmatch 'OverallHeadline\(status\.overall, firstUpdateCompleted\)') {
    throw 'Dashboard must use the shared overall-headline policy and must not restore CHECKED ITEMS OK.'
}
$developerHelp = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\DeveloperHelpContent.cpp') -Raw
foreach ($helpTopic in @('AT Start', 'AT Capture', 'AT Finish', 'Tracker Capture', 'Position CCY', 'Open Compat', 'Reload Compat')) {
    if ($developerHelp -notmatch [regex]::Escape($helpTopic)) {
        throw "Developer Help topic is missing: $helpTopic"
    }
}
foreach ($helpHeading in @('GOAL', 'WHAT THIS BUTTON DOES', 'WHEN TO USE IT', 'BEFORE YOU PRESS IT', 'PRESS THE BUTTON', 'SUCCESS', 'NEXT STEP', 'IF IT FAILS', 'SAFE OPERATION')) {
    if ($developerHelp -notmatch [regex]::Escape($helpHeading)) {
        throw "Developer Help instruction section is missing: $helpHeading"
    }
}
foreach ($captureInstruction in @('On ONE chart, choose ONE strategy', "Change only that strategy's AutoTrading state", 'ON to OFF, or OFF to ON')) {
    if ($developerHelp -notmatch [regex]::Escape($captureInstruction)) {
        throw "AT Capture beginner instruction is missing: $captureInstruction"
    }
}
foreach ($normalUseInstruction in @('only after a MultiCharts update', 'Charting.dll or ATOnPTracker.dll fingerprint changes', 'developer specifically asks', 'Do not use them during normal monitoring')) {
    if ($developerHelp -notmatch [regex]::Escape($normalUseInstruction)) {
        throw "Developer tool normal-use guidance is missing: $normalUseInstruction"
    }
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
    'trackerDataStaleCritical',
    'trackerStaleCriticalAfterMinutes',
    'lastTrackerAttempt',
    'lastCompleteTrackerSnapshot',
    'RecalculateOverallStatus',
    'bridgeRecoveryAttempts',
    'L"attempt " + std::to_wstring(attempt)',
    'MonitoringLogs(liveSnapshot)',
    'Bridge recovery:',
    'Snapshot attempts:'
)) {
    if ($watchdogMain -notmatch [regex]::Escape($recoveryToken)) {
        throw "Watchdog Tracker recovery token is missing: $recoveryToken"
    }
}

$appConfigSource = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\AppConfig.cpp') -Raw
if ($appConfigSource -notmatch [regex]::Escape($currentVersion) -or
    $appConfigSource -notmatch 'RefreshIniProfileCache' -or
    $appConfigSource -notmatch 'TrackerMonitor' -or
    $appConfigSource -notmatch 'stale_critical_after_minutes' -or
    $appConfigSource -notmatch 'date_order' -or
    $appConfigSource -notmatch 'title_only_contains' -or
    $appConfigSource -notmatch 'MultiCharts \(OpenAPI Web App\)' -or
    $appConfigSource -notmatch 'WritePrivateProfileStringW\(nullptr, nullptr, nullptr, path\.c_str\(\)\)') {
    throw 'INI profile cache refresh support is missing from AppConfig.'
}

$statusReportSource = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\StatusReport.cpp') -Raw
if ($statusReportSource -notmatch [regex]::Escape($currentVersion) -or
    $statusReportSource -notmatch 'AppendAlignedSectionRows' -or
    $statusReportSource -notmatch 'AppendOpenPositionsTable' -or
    $statusReportSource -notmatch 'Native Value' -or
    $statusReportSource -notmatch 'TryExtractKnownCurrency' -or
    $statusReportSource -notmatch 'openPlTotalsByCurrency' -or
    $statusReportSource -notmatch 'Total Open P/L' -or
    $statusReportSource -notmatch 'splitAlignedTableRow' -or
    $statusReportSource -notmatch 'openPositionCellHtml' -or
    $statusReportSource -notmatch 'mcst-open-positions-lines' -or
    $statusReportSource -notmatch 'mcst-system-status-lines' -or
    $statusReportSource -notmatch 'Current month Realized P/L' -or
    $statusReportSource -notmatch 'BuildPositionHistoryTotalRows' -or
    $statusReportSource -notmatch 'AppendMonitorLines' -or
    $statusReportSource -notmatch 'OVERALL STATUS' -or
    $statusReportSource -notmatch 'font-size:2em' -or
    $statusReportSource -notmatch 'font-size:1.5em' -or
    $statusReportSource -notmatch 'mcst-status-dot' -or
    $statusReportSource -notmatch 'out << L"\(no rows\)\\n"' -or
    $statusReportSource -notmatch 'kMonitorNameWidth = 18' -or
    $statusReportSource -notmatch 'kMonitorStateWidth = 14' -or
    $statusReportSource -notmatch 'kMonitorValueWidth = 26' -or
    $statusReportSource -notmatch 'mcst-status-dot-cell' -or
    $statusReportSource -notmatch 'mcst-status-overall-dot' -or
    $statusReportSource -notmatch 'alignNumericColumns' -or
    $statusReportSource -notmatch 'visibleAccounts' -or
    $statusReportSource -notmatch 'totalsByAccount' -or
    $statusReportSource -notmatch 'TrimCell\(row\[2\]\)' -or
    $statusReportSource -notmatch 'Current month Realized P/L " \+ account' -or
    $statusReportSource -notmatch 'preStyle' -or
    $statusReportSource -notmatch 'width=device-width,initial-scale=1.0' -or
    $statusReportSource -notmatch 'white-space:pre;margin:0' -or
    $statusReportSource -notmatch '-webkit-text-size-adjust:100%' -or
    $statusReportSource -notmatch 'font-size:15px !important' -or
    $statusReportSource -notmatch 'mcst-stale-attention' -or
    $statusReportSource -notmatch 'mcst-stale-critical' -or
    $statusReportSource -notmatch 'ATTENTION: STALE TRACKER TABLE DATA' -or
    $statusReportSource -notmatch 'CRITICAL: STALE TRACKER TABLE DATA' -or
    $statusReportSource -notmatch 'Last Tracker attempt' -or
    $statusReportSource -notmatch 'Last complete snapshot' -or
    $statusReportSource -notmatch '#15803D' -or
    $statusReportSource -notmatch '#B4232A' -or
    $statusReportSource -notmatch 'row\[0\] = source\.size\(\) > 2 \? source\[2\]' -or
    $statusReportSource -notmatch 'row\[1\] = source\.size\(\) > 6 \? source\[6\]' -or
    $statusReportSource -notmatch 'totalRow\[0\] = L\"Total Open P/L\"' -or
    $statusReportSource -notmatch 'totalRow\[1\] = total\.first' -or
    $statusReportSource -notmatch 'row\[8\] = source\.size\(\) > 7 \? source\[7\]' -or
    $statusReportSource -notmatch 'std::fabs\(quantity\) \* averagePrice' -or
    $statusReportSource -notmatch 'AppendAlignedSectionRows\(out, snapshot\.recentLogs, true, false\)') {
    throw 'Known-currency Tracker totals or aligned Open Positions field mapping is missing.'
}
if ($statusReportSource -notmatch [regex]::Escape('&#160;</span></span>') -or
    $statusReportSource -notmatch [regex]::Escape('&#160;</span>')) {
    throw 'HTML status cells do not contain real text separators for iOS Mail data detection.'
}
if ($statusReportSource -match 'totalPositionValue') {
    throw 'Native Position Value must not be aggregated while Average Price currency is unknown.'
}
if ($statusReportSource -match 'NATIVE VALUE TOTAL') {
    throw 'The removed Native Value total notice must not appear in the Status Report.'
}
if ($statusReportSource -match 'totalRow\[2\]\s*=' -or
    $statusReportSource -match 'row\[2\]\s*=\s*L"\[no current-month row' -or
    $statusReportSource -match 'not totalled"') {
    throw 'Open Positions P/L summary comments must remain removed so they cannot widen the report.'
}
if ($statusReportSource -match 'mcst-overall-status-lines') {
    throw 'Overall status must remain in the shared System Status flow, not a separate section.'
}
if ($statusReportSource -match [regex]::Escape('width=\"1100\"') -or
    $statusReportSource -match 'mcst-horizontal-scroll' -or
    $statusReportSource -match 'width:1100px' -or
    $statusReportSource -match 'mcst-open-position-card' -or
    $statusReportSource -match [regex]::Escape('<table class=\"mcst-open-positions\"')) {
    throw 'A rejected fixed-width table, overflow wrapper, or multi-line Open Positions card is still present.'
}
if ($statusReportSource -match [regex]::Escape('<table role=\"presentation\"')) {
    throw 'Overall/System Status still uses an independently shrinkable semantic HTML table.'
}

$testsSource = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Tests\main.cpp') -Raw
$testsProject = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Tests\MCST.Tests.vcxproj') -Raw
$recoveryPolicy = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Shared\TrackerRecoveryPolicy.h') -Raw
if ($testsProject -notmatch '<TargetName>MCST-LogicTests</TargetName>' -or
    $testsProject -notmatch 'StatusReport\.cpp' -or
    $testsProject -notmatch 'TrackerDateParser\.cpp' -or
    $testsProject -notmatch 'BrokerMonitor\.cpp' -or
    $testsSource -notmatch 'EUR \+8,25' -or
    $testsSource -notmatch 'Total Open P/L' -or
    $testsSource -notmatch 'OPEN P/L ROWS NOT TOTALLED: 2' -or
    $testsSource -notmatch '\$ 3,00' -or
    $testsSource -notmatch 'USD \\u20ac 4,00' -or
    $testsSource -notmatch '#15803D' -or
    $testsSource -notmatch '#B4232A' -or
    $testsSource -notmatch 'mcst-open-positions-lines' -or
    $testsSource -notmatch 'mcst-system-status-lines' -or
    $testsSource -notmatch 'OPEN POSITIONS\\n--------------\\n\(no rows\)' -or
    $testsSource -notmatch 'display:inline-block;width:18ch' -or
    $testsSource -notmatch 'display:inline-block;width:26ch' -or
    $testsSource -notmatch 'Signed Accounts values do not end in the same column' -or
    $testsSource -notmatch 'System Status detail columns are not aligned' -or
    $testsSource -notmatch 'font-size:2em' -or
    $testsSource -notmatch 'font-size:1.5em' -or
    $testsSource -notmatch 'The fixed-width Status dot cell itself is still enlarged' -or
    $testsSource -notmatch 'Current month Realized P/L' -or
    $testsSource -notmatch 'Current month Realized P/L DEMO100001' -or
    $testsSource -notmatch 'Current month Realized P/L DEMO200002' -or
    $testsSource -notmatch 'EUR -13,18' -or
    $testsSource -notmatch 'EUR \+7,25' -or
    $testsSource -notmatch 'UNLISTED' -or
    $testsSource -notmatch '18/08/2026 18\.00\.38' -or
    $testsSource -notmatch '08/18/2026 6:00:38 PM' -or
    $testsSource -notmatch '2026-08-18T18:00:38' -or
    $testsSource -notmatch 'width=device-width,initial-scale=1\.0' -or
    $testsSource -notmatch 'white-space:pre' -or
    $testsSource -notmatch 'font-size:15px !important' -or
    $testsSource -notmatch 'mcst-stale-attention' -or
    $testsSource -notmatch 'mcst-stale-critical' -or
    $testsSource -notmatch 'ATTENTION: STALE TRACKER TABLE DATA' -or
    $testsSource -notmatch 'CRITICAL: STALE TRACKER TABLE DATA' -or
    $testsSource -notmatch [regex]::Escape('width=\"1100\"') -or
    $testsSource -notmatch 'mcst-horizontal-scroll' -or
    $testsSource -notmatch 'Last Tracker attempt' -or
    $testsSource -notmatch 'Last complete snapshot' -or
    $testsSource -notmatch 'EUR \+106,68' -or
    $testsSource -notmatch 'Connection to Saxo Group has been established' -or
    $testsSource -notmatch 'No connection to Saxo Group trading system' -or
    $testsSource -notmatch 'UIC is not valid' -or
    $testsSource -notmatch 'brokerDecision\.status\.value != L"Connected"' -or
    $testsSource -notmatch 'Broker authentication required' -or
    $testsSource -notmatch 'SelectTrackerRecoveryPolicy' -or
    $testsSource -notmatch 'targetedTimeBudgetMs != 100' -or
    $testsSource -notmatch 'processWideTimeBudgetMs != 3000' -or
    $testsSource -notmatch 'retryCooldownMs != 5ull \* 60ull \* 1000ull' -or
    $testsSource -notmatch 'logic and regression tests passed' -or
    $recoveryPolicy -notmatch 'failureStreak >= 10' -or
    $recoveryPolicy -notmatch 'failureStreak >= 3') {
    throw 'One-line Open Positions placement, size, or color regression test is missing.'
}
if ($testsSource -match [regex]::Escape('L"[10 rows]"') -or
    $testsSource -notmatch 'Current Saxo total is not aligned or still contains a removed row-count comment') {
    throw 'The current Saxo Open P/L regression test still expects the removed row-count comment.'
}

$watchdogProject = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\MCST.Watchdog.vcxproj') -Raw
if ($watchdogProject -notmatch '<WholeProgramOptimization>false</WholeProgramOptimization>' -or
    $watchdogProject -notmatch '<FunctionLevelLinking>false</FunctionLevelLinking>' -or
    $watchdogProject -notmatch '<IntrinsicFunctions>false</IntrinsicFunctions>' -or
    $watchdogProject -notmatch '<EnableCOMDATFolding>false</EnableCOMDATFolding>' -or
    $watchdogProject -notmatch '<OptimizeReferences>false</OptimizeReferences>' -or
    $watchdogProject -notmatch 'UIAutomationCore\.lib' -or
    $watchdogProject -notmatch 'OleAut32\.lib' -or
    $watchdogProject -notmatch 'MultiChartsHealthMonitor\.cpp') {
    throw 'Required MCST-Watchdog production Release settings have changed.'
}

$hostText = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\PowerLanguage\MCST_Tracker_Bridge_Host.txt') -Raw
if ($hostText -notmatch [regex]::Escape('C:\MCExtras\MCST-TrackerBridge.dll')) {
    throw 'PowerLanguage host does not reference the production Tracker Bridge DLL path.'
}
if ($hostText -notmatch "Internal bridge build: V$currentBridgeBuild") {
    throw "PowerLanguage host does not identify Tracker Bridge internal build V$currentBridgeBuild."
}

$watchdogRc = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\MCST.Watchdog.rc') -Raw
$expectedProductVersion = 'VALUE "ProductVersion", "' + $currentVersion + '\0"'
if (-not $watchdogRc.Contains($expectedProductVersion)) {
    throw "Watchdog Windows product version is not $currentVersion."
}
if ($watchdogRc -notmatch 'FILEVERSION 1,20,17,0' -or
    $watchdogRc -notmatch 'PRODUCTVERSION 1,20,17,0') {
    throw 'Watchdog numeric Windows version resource is not aligned with 1.20.17.'
}
if ($watchdogRc -notmatch 'IDR_WATCHDOG_HEADER_LOGO' -or
    $watchdogRc -notmatch 'Assets\\\\WatchdogHeaderLogo\.png') {
    throw 'Watchdog header logo is not embedded in the Windows resource.'
}

$bridgeSource = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\MCTrackerBridge.cpp') -Raw
$bridgeRc = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.rc') -Raw
if ($bridgeSource -match [regex]::Escape('return (std::filesystem::path(path).parent_path()')) {
    throw 'Invalid vector-to-filesystem::path construction found in Tracker Bridge Host compatibility path.'
}
if ($bridgeSource -notmatch "constexpr int kBridgeVersion = $currentBridgeBuild;") {
    throw "Tracker Bridge internal source build is not V$currentBridgeBuild."
}
foreach ($trackerResearchToken in @(
    'WriteDynamicTrackerLocatorResearch',
    'MCST_Tracker_Dynamic_Locator',
    'dynamic_tracker_locator',
    'FindRttiVtableObjectsProcessWide',
    'Passive research only: no clicks, no input, no function calls and no writes to MultiCharts memory.'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($trackerResearchToken)) {
        throw "Dynamic Tracker research token is missing: $trackerResearchToken"
    }
}

foreach ($mc17TrackerToken in @(
    'kMc17VerifiedAtonpTimestamp = 0x6AB58F4Cu',
    'kMc17VerifiedTabViewPrimaryVtableRva = 0x1D78D8u',
    'VerifiedTabViewVtableRva',
    'Embedded validated MC17 Tracker profile'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($mc17TrackerToken)) {
        throw "Verified MC17 Tracker profile token is missing: $mc17TrackerToken"
    }
}

$watchdogMain = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\main.cpp') -Raw
foreach ($helpButtonToken in @('BS_OWNERDRAW', 'DrawDeveloperHelpButton', 'L"?  Help"')) {
    if ($watchdogMain -notmatch [regex]::Escape($helpButtonToken)) {
        throw "Highlighted Developer Help button token is missing: $helpButtonToken"
    }
}
if ($bridgeRc -notmatch 'FILEVERSION 1,0,181,0' -or
    $bridgeRc -notmatch [regex]::Escape('VALUE "FileVersion", "1.0.181.0\0"')) {
    throw 'Tracker Bridge Windows file version is not aligned with internal V181.'
}
foreach ($recoveryToken in @(
    'InvalidateTabViewCaches',
    'forceFreshScan',
    'BuildV179StatusReportSnapshotWithRecovery',
    'TabViewRecoveryModeScope',
    '~TabViewRecoveryModeScope',
    'g_tabViewRecoveryModeActive.store(false)',
    'g_candidateCacheValid',
    'g_candidateCacheTick',
    'kTabViewCandidateCacheTtlMs = 30000',
    'recovery_attempted',
    'recovery_result',
    'g_tabViewRecoveryModeActive',
    'g_tabViewPersistentFailureActive',
    'g_tabViewRecoveryFailureStreak',
    'g_recentTabViewHints',
    'RememberTabViewHint',
    'tabview_recovery_start',
    'tabview_recovery_validated_hint',
    'tabview_recovery_targeted',
    'tabview_recovery_process_wide',
    'tabview_recovery_complete',
    'tabview_recovery_incomplete',
    'exact_vtable_hits',
    'structurally_rejected',
    'accepted_candidates',
    'ValidateRecentProfileTabViewHints',
    'SelectStrongProfileTabViewCandidate',
    'processWideTimeBudgetMs',
    'processWideByteBudget',
    'SelectTrackerRecoveryPolicy',
    'process-wide discovery deferred during persistent recovery',
    'pagesRead < 3'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($recoveryToken)) {
        throw "Tracker Bridge recovery token is missing: $recoveryToken"
    }
}
foreach ($targetedRecoveryToken in @(
    'kKnownTabViewPrimaryVtableRva = 0x1D78C8u',
    'ValidateAndScoreTabViewCandidate(snapshot, targetVtables, candidate)',
    'secondaryTabViewVtableAt48',
    'candidate.trackerLayoutSignature',
    'pageObjectPointers >= 5'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($targetedRecoveryToken)) {
        throw "Tracker Bridge targeted structural recovery token is missing: $targetedRecoveryToken"
    }
}

$trackerReaderHeader = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridge\TrackerBridgeReader.h') -Raw
$trackerReaderSource = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridge\TrackerBridgeReader.cpp') -Raw
if ($trackerReaderHeader -notmatch 'tabViewDiagnostic' -or
    $trackerReaderSource -notmatch 'key == "tabview_diagnostic"') {
    throw 'CATPTTabView recovery diagnostic is not exposed through TrackerBridgeReader.'
}

$brokerAuthDetector = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\BrokerAuthDetector.cpp') -Raw
if ($brokerAuthDetector -notmatch 'UIAutomation\.h' -or
    $brokerAuthDetector -notmatch 'ole2\.h' -or
    $brokerAuthDetector -notmatch 'GetCurrentPatternAs' -or
    $brokerAuthDetector -notmatch 'UIA_ValuePatternId' -or
    $brokerAuthDetector -notmatch 'titleOnlyContains' -or
    $brokerAuthDetector -notmatch 'Never copy the browser''s full URL') {
    throw 'Bounded browser authentication detection or its COM header order is missing.'
}
if ($brokerAuthDetector -match '#define WIN32_LEAN_AND_MEAN') {
    throw 'BrokerAuthDetector hides the COM declarations required by UIAutomation.h.'
}
foreach ($historyToken in @(
    'V153GridSectionResult monitoringLogs',
    'logs.rows.resize(10)',
    'monitoringLogs.name = "monitoring_logs"',
    'L"; display_rows=" + std::to_wstring(logs.rows.size())',
    'L"; monitoring_rows=" + std::to_wstring(monitoringLogs.rows.size())',
    'AppendV153Section(out, monitoringLogs, 6)',
    '200, 200, 3'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($historyToken)) {
        throw "Tracker Bridge extended monitoring-history token is missing: $historyToken"
    }
}
foreach ($positionHistoryToken in @(
    'positionHistoryPageOffset',
    'kPositionHistoryAtonpTimestamp = 0x6A5694FBu',
    'profile.positionHistoryPageOffset',
    'V153GridSectionResult positionHistory',
    '"position_history"',
    'AppendV153Section(out, positionHistory, 8)',
    '5000, 5000, 3'
)) {
    if ($bridgeSource -notmatch [regex]::Escape($positionHistoryToken)) {
        throw "Tracker Bridge Position History token is missing: $positionHistoryToken"
    }
}
if ($bridgeSource -match 'logs\.diagnostic \+= "; display_rows="' -or
    $bridgeSource -match 'std::to_string\(monitoringLogs\.rows\.size\(\)\)') {
    throw 'Tracker Bridge monitoring diagnostics mix narrow strings with std::wstring.'
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
    'tracker_position_history_page_offset',
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

$bridgeRc = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridgeHost\MCST.TrackerBridgeHost.rc') -Raw
if ($bridgeRc -notmatch "internal bridge build V$currentBridgeBuild" -or
    $bridgeRc -notmatch "1\.0\.$currentBridgeBuild\.0") {
    throw "Tracker Bridge version resource is not aligned with internal build V$currentBridgeBuild."
}

$readerHeader = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridge\TrackerBridgeReader.h') -Raw
if ($readerHeader -notmatch "kTrackerBridgeInternalBuildVersion = $minimumProductionBridgeBuild") {
    throw "TrackerBridgeReader production minimum is not V$minimumProductionBridgeBuild."
}
if ($readerHeader -notmatch "kPositionCurrencyResearchBridgeVersion = $positionCurrencyResearchBridgeBuild") {
    throw "Position Currency research Bridge requirement is not V$positionCurrencyResearchBridgeBuild."
}

$readerSource = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.TrackerBridge\TrackerBridgeReader.cpp') -Raw
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
if ($readerSource -notmatch 'name == "monitoring_logs"' -or
    $readerSource -notmatch 'snapshot\.monitoringLogs') {
    throw 'Optional monitoring_logs parsing or validation is missing.'
}
if ($readerSource -notmatch 'name == "position_history"' -or
    $readerSource -notmatch 'snapshot\.positionHistory') {
    throw 'Optional position_history parsing or validation is missing.'
}

$protocolHeader = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Shared\MCBridgeProtocol.h') -Raw
if ($protocolHeader -notmatch "kProtocolVersion = $currentProtocolVersion") {
    throw "Bridge protocol is not V$currentProtocolVersion."
}
if ($protocolHeader -notmatch 'CapturePositionCurrencyDirectResearch = 50') {
    throw 'Bridge Protocol V2 additive Position Currency research command 50 is missing.'
}

$versionDetector = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\MultiChartsVersionDetector.cpp') -Raw
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

$multiChartsHealthMonitor = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\MultiChartsHealthMonitor.cpp') -Raw
foreach ($healthToken in @(
    'SendMessageTimeoutW',
    'GetProcessMemoryInfo',
    'GetProcessHandleCount',
    'GetGuiResources',
    'ReadQueueIndicator',
    'queueAgeSeconds >= 10',
    'queueGrowthSamples >= 3',
    'cpuCorePercent >= 90.0',
    'highCpuSamples >= 3'
)) {
    if ($multiChartsHealthMonitor -notmatch [regex]::Escape($healthToken)) {
        throw "MultiCharts Health monitor contract token is missing: $healthToken"
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
    'Docs\GITHUB_RELEASES.md',
    'Docs\FIRST_GITHUB_PUBLICATION.md',
    'Docs\PORTABLE_INSTALL.md',
    'MCST.TrackerBridgeHost\README.md'
)
foreach ($doc in $publicCurrentDocs) {
    $text = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root $doc) -Raw
    if ($text -match 'MCST-StartupProbe' -or
        $text -match 'user-verified working 1\.108' -or
        $text -match 'C:\\MCBridge\\MCTrackerBridge\.dll') {
        throw "Internal development-history text found in public documentation: $doc"
    }
}

$releaseNotes = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'RELEASE_NOTES.md') -Raw
$escapedCurrentVersion = [regex]::Escape($currentVersion)
if ($releaseNotes -notmatch "MCST $escapedCurrentVersion GitHub Publication-Ready Package") {
    throw "Release notes heading must contain: MCST $currentVersion GitHub Publication-Ready Package"
}
if ($releaseNotes -notmatch "internal build:\s+V$currentBridgeBuild" -or
    $releaseNotes -notmatch 'GitHub Release' -or
    $releaseNotes -notmatch 'SHA-256' -or
    $releaseNotes -notmatch 'MIT License' -or
    $releaseNotes -notmatch 'Mika Tättäläinen' -or
    $releaseNotes -notmatch 'no\s+active INI' -or
    $releaseNotes -notmatch '\.gitignore' -or
    $releaseNotes -notmatch '\.ini\.example' -or
    $releaseNotes -notmatch 'Overall' -or
    $releaseNotes -notmatch 'Position History' -or
    $releaseNotes -notmatch 'visible in Accounts' -or
    $releaseNotes -notmatch 'UI Automation' -or
    $releaseNotes -notmatch 'ole2.h' -or
    $releaseNotes -notmatch '0x1D78C8' -or
    $releaseNotes -notmatch 'date_order' -or
    $releaseNotes -notmatch '15-pixel monospaced' -or
    $releaseNotes -notmatch 'Protocol V2') {
    throw "Release notes do not identify MCST $currentVersion, Tracker Bridge V$currentBridgeBuild, account totals, browser authentication detection, and self-recovery."
}

$publisher = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'Publish-GitHub-Release.ps1') -Raw
foreach ($publisherToken in @(
    'C:\Users\Administrator\AppData\Local\GitHubDesktop\app-3.6.4\resources\app\git\mingw64\bin\git.exe',
    'Tools\Validate-Release.ps1',
    'v1.20.17',
    'git.exe',
    'push',
    'tag'
)) {
    if ($publisher -notmatch [regex]::Escape($publisherToken)) {
        throw "GitHub publication helper token is missing: $publisherToken"
    }
}

$workflow = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root '.github\workflows\release.yml') -Raw
foreach ($workflowToken in @(
    'windows-2022',
    'actions/checkout@v5',
    'microsoft/setup-msbuild@v3',
    'actions/upload-artifact@v4',
    'MCST-Watchdog-1.20.17-Windows-x64',
    'contents: write',
    'Validate-Release.ps1',
    'MCST-LogicTests.exe',
    'Build-PortableRelease.ps1',
    'v*.*.*',
    'gh release create',
    '--verify-tag'
)) {
    if ($workflow -notmatch [regex]::Escape($workflowToken)) {
        throw "GitHub Release workflow token is missing: $workflowToken"
    }
}

$portableBuilder = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'Tools\Build-PortableRelease.ps1') -Raw
foreach ($packageToken in @(
    '1.20.17',
    'MCST-Watchdog.exe',
    'MCST-TrackerBridge.dll',
    'MCST_Tracker_Bridge_Host.txt',
    'MCST_Tracker_Bridge_Stop.txt',
    'MCST-Watchdog.ini.example',
    'MCST-Compatibility.ini.example',
    "'LICENSE' = 'LICENSE'",
    'PORTABLE_INSTALL.md',
    'PACKAGE_MANIFEST.txt',
    'SHA256SUMS.txt',
    'MCST-LogicTests.exe',
    "'.ini'",
    "'.pdb'",
    'Get-FileHash',
    'Compress-Archive'
)) {
    if ($portableBuilder -notmatch [regex]::Escape($packageToken)) {
        throw "Portable Release package contract token is missing: $packageToken"
    }
}

$coveredProbe = Get-Content -Encoding UTF8 -LiteralPath (Join-Path $root 'MCST.Watchdog\CoveredQueueProbe.h') -Raw
foreach ($probeToken in @(
    'JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE', 'CREATE_SUSPENDED | CREATE_NO_WINDOW',
    'WaitForSingleObject(process.hProcess, 300)', 'std::chrono::seconds(60)',
    'Budget() = 2', 'PrintWindow(bar, memory, PW_CLIENTONLY)',
    'IsConfirmedRenderedWarning', '--mcst-covered-queue-probe'
)) {
    if (-not $coveredProbe.Contains($probeToken)) {
        throw "Covered queue probe safety token is missing: $probeToken"
    }
}
if ($watchdogMain -notmatch 'TryWorkerMode') {
    throw 'Watchdog must intercept covered-queue worker mode before normal initialization.'
}

Write-Host "MCST $currentVersion source validation passed; Windows live trial still required." -ForegroundColor Green
