#pragma once
#include <map>
#include <sstream>
#include <string>
#include <vector>
namespace mcst
{
    inline std::size_t CountAddedLogRows(const std::vector<std::vector<std::wstring>>& before,
        const std::vector<std::vector<std::wstring>>& after)
    {
        std::map<std::vector<std::wstring>, std::size_t> available;
        for (const auto& row : before) ++available[row];
        std::size_t added = 0;
        for (const auto& row : after)
        {
            auto& count = available[row];
            if (count) --count; else ++added;
        }
        return added;
    }

    inline std::wstring WithoutQueueDiagnostics(const std::wstring& report)
    {
        std::wistringstream input(report);
        std::wostringstream out;
        std::wstring line;
        bool skip = false;
        while (std::getline(input, line))
        {
            if (line.find(L"VISIBLE QUEUE WARNINGS (") == 0 ||
                line.find(L"QUEUE READ DIAGNOSTICS (") == 0)
            {
                skip = true;
                continue;
            }
            if (skip && (line.empty() || line == L"\r" || line.find(L"PID ") == 0 ||
                    line.find(L"---") == 0))
                continue;
            skip = false;
            out << line << L'\n';
        }
        return out.str();
    }
}
