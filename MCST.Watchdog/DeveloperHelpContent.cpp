#include "DeveloperHelpContent.h"

namespace
{
    std::wstring Topic(
        const wchar_t* title,
        const wchar_t* goal,
        const wchar_t* purpose,
        const wchar_t* whenToUse,
        const wchar_t* before,
        const wchar_t* action,
        const wchar_t* success,
        const wchar_t* next,
        const wchar_t* failure,
        const wchar_t* safety)
    {
        return std::wstring(title) +
            L"\r\n\r\nIMPORTANT - WHEN THESE TOOLS ARE NORMALLY USED\r\n" +
            GetDeveloperToolsNormalUseRule() +
            L"\r\n\r\nGOAL\r\n" + goal +
            L"\r\n\r\nWHAT THIS BUTTON DOES\r\n" + purpose +
            L"\r\n\r\nWHEN TO USE IT\r\n" + whenToUse +
            L"\r\n\r\nBEFORE YOU PRESS IT\r\n" + before +
            L"\r\n\r\nPRESS THE BUTTON\r\n" + action +
            L"\r\n\r\nSUCCESS\r\n" + success +
            L"\r\n\r\nNEXT STEP\r\n" + next +
            L"\r\n\r\nIF IT FAILS\r\n" + failure +
            L"\r\n\r\nSAFE OPERATION\r\n" + safety;
    }
}

const wchar_t* GetDeveloperToolsNormalUseRule()
{
    return
        L"Developer tools are normally needed only after a MultiCharts update, "
        L"after the Charting.dll or ATOnPTracker.dll fingerprint changes, or when "
        L"a developer specifically asks you to collect compatibility evidence. "
        L"Do not use them during normal monitoring.";
}

const std::vector<DeveloperHelpTopic>& GetDeveloperHelpTopics()
{
    static const std::vector<DeveloperHelpTopic> topics = {
        {
            L"Overview",
            L"DEVELOPER MODE HELP\r\n\r\n"
            L"IMPORTANT - WHEN THESE TOOLS ARE NORMALLY USED\r\n" +
            std::wstring(GetDeveloperToolsNormalUseRule()) +
            L"\r\n\r\nThese buttons collect compatibility evidence for developers. They are not normal monitoring controls and they do not switch live trading on or off.\r\n\r\n"
            L"Choose a button on the left. The right side explains its purpose, the exact preparation, what success looks like, and what to do with the result.\r\n\r\n"
            L"AutoTrading workflow:\r\n"
            L"1. AT Start\r\n"
            L"2. Change only the AutoTrading ON/OFF state of one strategy on one chart\r\n"
            L"3. AT Capture\r\n"
            L"4. Repeat one controlled change and one capture as needed\r\n"
            L"5. AT Finish\r\n\r\n"
            L"If you did not deliberately start a compatibility investigation, leave these controls alone and use the normal Refresh and report controls."
        },
        {
            L"AT Start",
            Topic(
                L"AT START",
                L"Create a trustworthy baseline for an AutoTrading compatibility investigation.",
                L"Starts a new research session, dynamically maps the current Charting.dll candidate objects, and records snapshot 1. Starting again replaces any unfinished session.",
                L"Use when a MultiCharts update has made the AutoTrading count unknown or when a developer has asked for a controlled capture.",
                L"1. Start every MultiCharts instance that will remain open during the test.\r\n2. Open the chart whose strategy will be used.\r\n3. Confirm the strategy and its current AutoTrading ON/OFF state.\r\n4. Do not change anything yet.",
                L"Click AT Start once. Wait for the result message before touching MultiCharts.",
                L"The message says that the research session started and baseline snapshot 1 was captured. The file C:\\Temp\\MCST-Watchdog\\AutoTradingResearch.txt opens.",
                L"On one chart, change only one strategy's AutoTrading state from ON to OFF or OFF to ON. Wait for it to settle, then press AT Capture.",
                L"Read the message. Make sure MultiCharts is running and stable. If necessary, press AT Start again to create a clean baseline.",
                L"Read-only research. It records state but does not click MultiCharts, change AutoTrading, place orders, or write to MultiCharts memory.")
        },
        {
            L"AT Capture",
            Topic(
                L"AT CAPTURE",
                L"Record the AutoTrading state after one controlled change so it can be compared with the previous capture.",
                L"Adds one snapshot to the active AutoTrading research session and records which dynamically discovered RVA/offset candidates reacted to the controlled ON/OFF change.",
                L"Use only after AT Start and after one deliberate AutoTrading state change.",
                L"1. Press AT Start first.\r\n2. On ONE chart, choose ONE strategy.\r\n3. Change only that strategy's AutoTrading state: ON to OFF, or OFF to ON.\r\n4. Do not change strategy settings, charts, workspaces, or the number of running MultiCharts instances.\r\n5. Wait until the new AutoTrading state is stable.",
                L"Click AT Capture once. Do not double-click and do not make another change while capture is running.",
                L"A message says that a numbered snapshot was captured. The comparison is appended to C:\\Temp\\MCST-Watchdog\\AutoTradingResearch.txt.",
                L"For another comparison, change only the same chart's AutoTrading ON/OFF state again, wait, and press AT Capture. Capture both directions (OFF and ON), preferably at least three transitions, then press AT Finish.",
                L"If there is no active session, press AT Start and repeat the steps. If the MultiCharts process set changed, return all intended instances to a stable state and restart the research session with AT Start.",
                L"Read-only research. The button does not click MultiCharts, change a strategy, place orders, or write to MultiCharts memory. The user performs the single ON/OFF change.")
        },
        {
            L"AT Finish",
            Topic(
                L"AT FINISH",
                L"Turn the captured changes into a ranked compatibility report.",
                L"Ends the active AutoTrading research session, analyzes the baseline and captures, and ranks candidates that followed the controlled changes.",
                L"Use after AT Start and successful AT Captures in both directions. At least three clean transitions are recommended.",
                L"Stop making changes. Confirm that the latest AT Capture succeeded and that the MultiCharts instances used throughout the session are still running.",
                L"Click AT Finish once and wait for analysis to complete.",
                L"A summary message appears and C:\\Temp\\MCST-Watchdog\\AutoTradingResearch.txt opens with the automatic ranking.",
                L"Send the complete report to the developer. Do not copy a candidate into the compatibility database unless it has been independently verified.",
                L"If no session is active, begin again with AT Start. If the report has no useful transitions, repeat the investigation using exactly one AutoTrading ON/OFF change before every capture.",
                L"Analysis is read-only and does not control strategies or trading.")
        },
        {
            L"Tracker Capture",
            Topic(
                L"TRACKER CAPTURE",
                L"Collect evidence needed to diagnose an unreadable Order and Position Tracker after a MultiCharts update.",
                L"Requests a passive research bundle from the Bridge for Accounts, Open Positions, Position History, Recent Logs, object candidates, and compatibility metadata.",
                L"Use when Tracker Snapshot or Recent Logs remains Warning/Critical, or when a new ATOnPTracker.dll fingerprint has no verified profile.",
                L"1. Start MultiCharts and Watchdog.\r\n2. Open Order and Position Tracker.\r\n3. Make Accounts, Open Positions, Positions History, and Logs tabs available.\r\n4. Leave the Tracker window open and stable.\r\n5. If possible, keep representative accounts and positions visible.",
                L"Click Tracker Capture once. Keep MultiCharts and Tracker open until the completion message appears.",
                L"A completion message summarizes the capture and Windows opens C:\\Temp, where the timestamped Tracker research files are stored.",
                L"Send the entire capture bundle and the displayed ATOnPTracker fingerprint to the developer. Do not enable a generated Candidate profile yourself.",
                L"Confirm that the Bridge is green and the correct Bridge DLL is loaded. Reopen Order and Position Tracker, wait briefly, and try once more. Preserve the exact error message if it still fails.",
                L"Passive and read-only. It does not click Tracker, alter orders, or write to MultiCharts memory.")
        },
        {
            L"Position CCY",
            Topic(
                L"POSITION CCY",
                L"Collect optional evidence linking each visible open position to its currency fields.",
                L"Captures a fresh Open Positions reference and correlates it with a bounded read-only position-object investigation.",
                L"Use only when a developer requests currency research. Normal Open P/L reporting does not require it.",
                L"1. Open Order and Position Tracker on Open Positions.\r\n2. Keep at least two representative positions open, preferably using different currencies.\r\n3. Confirm the rows are current and readable.\r\n4. Keep MultiCharts and Tracker stable.",
                L"Click Position CCY once and do not change positions while it is running.",
                L"The result message identifies the reference and research files under C:\\Temp and opens that folder.",
                L"Send MCST_Position_Currency_Reference_<pid>.txt and MCST_Position_Currency_Dynamic_<pid>.txt together to the developer.",
                L"If there are no open positions or Open Positions is unreadable, correct that first. If the Bridge is too old, install the supplied Bridge and restart MultiCharts before retrying.",
                L"Read-only research. It never calls an unknown MultiCharts function and does not modify positions or orders.")
        },
        {
            L"Open Compat",
            Topic(
                L"OPEN COMPAT",
                L"Inspect the compatibility profiles that authorize build-dependent internal reads.",
                L"Creates the database if missing and opens C:\\MCExtras\\MCST-Compatibility.ini in the associated text editor.",
                L"Use when reviewing an exact module fingerprint or entering values that a developer has already verified.",
                L"Make a backup. Obtain the complete verified profile and exact Charting.dll or ATOnPTracker.dll fingerprint from the developer. Never guess offsets or copy values from another build.",
                L"Click Open Compat. Reading the file is safe. Edit only when you have verified instructions, then save it normally.",
                L"The compatibility file opens. No running configuration changes merely because the file was opened.",
                L"After saving verified changes, return to Watchdog and press Reload Compat. If a Bridge DLL was replaced, restart MultiCharts instead.",
                L"If the file cannot be opened, verify that C:\\MCExtras is writable and that a text editor is associated with .ini files. Do not create an improvised profile elsewhere.",
                L"Opening is non-mutating except that a missing database may be created with the built-in verified profile. Unverified Candidate sections must remain disabled.")
        },
        {
            L"Reload Compat",
            Topic(
                L"RELOAD COMPAT",
                L"Apply saved, verified compatibility-profile changes without restarting Watchdog.",
                L"Forces a fresh Watchdog AutoTrading read and a new Tracker snapshot so both compatibility consumers re-evaluate the saved database.",
                L"Use only after saving a verified change in C:\\MCExtras\\MCST-Compatibility.ini. It is not a general repair button.",
                L"1. Save and close the compatibility file.\r\n2. Confirm the profile fingerprint exactly matches the loaded module.\r\n3. Confirm the profile is explicitly verified and enabled.\r\n4. Leave MultiCharts and Tracker running.",
                L"Click Reload Compat once and wait for the forced refresh to finish.",
                L"The message says profiles will be re-evaluated. The Dashboard then reports the selected profile and fresh subsystem state.",
                L"Confirm AutoTrading and Tracker results separately. A profile may authorize one subsystem but not the other.",
                L"Reopen the file and check spelling, fingerprint, required fields, verified, and enabled. Replacing MCST-TrackerBridge.dll cannot be completed by this button; restart MultiCharts to load the new DLL.",
                L"Reloading does not modify the compatibility file or MultiCharts memory. It only asks the readers to re-evaluate saved verified data.")
        }
    };
    return topics;
}
