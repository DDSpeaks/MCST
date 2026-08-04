#include <iostream>
#include <type_traits>
#include "../MCST.Shared/MCBridgeProtocol.h"
#include "../MCST.TrackerBridge/TrackerBridgeReader.h"

int wmain()
{
    static_assert(sizeof(mcbridge::MessageHeader) == 20);
    static_assert(std::is_default_constructible_v<TrackerStatusSnapshot>);
    std::wcout << L"MCST foundation smoke tests passed.\n";
    return 0;
}
