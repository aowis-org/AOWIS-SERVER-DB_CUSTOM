#pragma once

#include <string_view>

namespace aowis::dvmdb::detail {

// Human-readable branch labels are stored inside UUIDv7-keyed ref files, not
// as filesystem paths. Accept internal spaces and valid UTF-8 while rejecting
// control characters, ambiguous surrounding whitespace and URL path separators.
// The limit is in UTF-8 bytes (the durable branch record's name field).
inline bool validBranchLabel(std::string_view name)
{
    if (name.empty() || name.size() > 96 || name == "." || name == ".." ||
        name.front() == ' ' || name.back() == ' ')
        return false;
    for (std::size_t i = 0; i < name.size();)
    {
        const unsigned char first = static_cast<unsigned char>(name[i]);
        if (first < 0x80U)
        {
            if (first < 0x20U || first == 0x7fU || first == '/' || first == '\\'
                || first == '%' || first == '?' || first == '#') return false;
            ++i;
            continue;
        }
        unsigned count = 0;
        if (first >= 0xc2U && first <= 0xdfU) count = 2;
        else if (first >= 0xe0U && first <= 0xefU) count = 3;
        else if (first >= 0xf0U && first <= 0xf4U) count = 4;
        else return false;
        if (i + count > name.size()) return false;
        for (unsigned step = 1; step < count; ++step)
            if ((static_cast<unsigned char>(name[i + step]) & 0xc0U) != 0x80U)
                return false;
        const unsigned char second = static_cast<unsigned char>(name[i + 1]);
        if ((first == 0xe0U && second < 0xa0U) ||
            (first == 0xedU && second > 0x9fU) ||
            (first == 0xf0U && second < 0x90U) ||
            (first == 0xf4U && second > 0x8fU)) return false;
        i += count;
    }
    return true;
}

} // namespace aowis::dvmdb::detail
