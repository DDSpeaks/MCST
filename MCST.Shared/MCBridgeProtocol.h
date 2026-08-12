#pragma once

#include <cstdint>

namespace mcbridge
{
    constexpr std::uint32_t kMagic = 0x3142434Du; // "MCB1" in little-endian memory.
    constexpr std::uint16_t kProtocolVersion = 2;
    constexpr std::uint32_t kMaximumPayloadBytes = 4u * 1024u * 1024u;

    enum class Command : std::uint16_t
    {
        Ping = 1,
        GetStatus = 2,
        GetTrackerMap = 3,
        TakeLocalSnapshot = 4,
        ProbeAccountsExtractor = 10,
        ProbeOpenPositionsExtractor = 11,
        RunBothExtractorCalls = 12,
        GetExtractorStatus = 13,
        AnalyzeExtractorContracts = 14,
        TestUiDispatch = 15,
        AnalyzeExtractorCallers = 16,
        AnalyzeThreeTabAnchors = 17,
        AnalyzePageVirtualMethods = 18,
        AnalyzeSlot17Deep = 19,
        CaptureResearchBundle = 20,
        CaptureLiveObjectGraph = 21,
        CaptureLiveObjectGraphAccounts = 22,
        CaptureLiveObjectGraphOpenPositions = 23,
        CaptureLiveObjectGraphLogs = 24,
        CaptureTargetedProbeAccounts = 25,
        CaptureTargetedProbeOpenPositions = 26,
        CaptureTargetedProbeLogs = 27,
        CaptureTargetedProbeAll = 28,
        CapturePageObjectProbeAccounts = 29,
        CapturePageObjectProbeOpenPositions = 30,
        CapturePageObjectProbeLogs = 31,
        CapturePageObjectProbeAll = 32,
        CaptureGridInterfaceLocatorAccounts = 33,
        CaptureGridInterfaceLocatorOpenPositions = 34,
        CaptureGridInterfaceLocatorLogs = 35,
        CaptureGridInterfaceLocatorAll = 36,
        CaptureFlexGridTextReaderAccounts = 37,
        CaptureFlexGridTextReaderOpenPositions = 38,
        CaptureFlexGridTextReaderLogs = 39,
        CaptureFlexGridTextReaderAll = 40,
        CaptureV151SingleCellAccounts = 41,
        CaptureV151SingleCellOpenPositions = 42,
        CaptureV151SingleCellLogs = 43,
        CaptureV151SingleCellAll = 44,
        CaptureV152CoordinateMapAccounts = 45,
        CaptureV152CoordinateMapOpenPositions = 46,
        CaptureV152CoordinateMapLogs = 47,
        CaptureV152CoordinateMapAll = 48,
        GetStatusReportSnapshot = 49,
        CapturePositionCurrencyDirectResearch = 50
    };

    enum class Status : std::uint32_t
    {
        Ok = 0,
        InvalidHeader = 1,
        UnsupportedProtocol = 2,
        UnsupportedCommand = 3,
        PayloadTooLarge = 4,
        TrackerNotFound = 5,
        InternalError = 6,
        BridgeNotReady = 7,
        ExperimentalCallsDisabled = 8,
        ExtractorContractNotResolved = 9,
        ExtractorCallFailed = 10
    };

#pragma pack(push, 1)
    struct MessageHeader
    {
        std::uint32_t magic = kMagic;
        std::uint16_t protocolVersion = kProtocolVersion;
        std::uint16_t command = 0;
        std::uint32_t requestId = 0;
        std::uint32_t status = 0;
        std::uint32_t payloadBytes = 0;
    };
#pragma pack(pop)

    static_assert(sizeof(MessageHeader) == 20, "Bridge protocol header size changed");
}
