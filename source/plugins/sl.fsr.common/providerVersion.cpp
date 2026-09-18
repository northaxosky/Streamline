/*
* Copyright (c) 2026
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include "providerVersion.h"

#include <charconv>
#include <string_view>
#include <utility>

namespace sl::fsr
{

bool parseVersion(const char* value, ProviderVersion& version)
{
    if (!value) return false;
    std::string_view text(value);
    const auto first = text.find('.');
    const auto second = first == std::string_view::npos ? first : text.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) return false;

    ProviderVersion parsed{};
    const auto parse = [](std::string_view part, uint32_t& output)
    {
        const auto result = std::from_chars(part.data(), part.data() + part.size(), output);
        return result.ec == std::errc{} && result.ptr == part.data() + part.size();
    };
    if (!parse(text.substr(0, first), parsed.major) ||
        !parse(text.substr(first + 1, second - first - 1), parsed.minor) ||
        !parse(text.substr(second + 1), parsed.patch))
    {
        return false;
    }
    parsed.name = value;
    version = std::move(parsed);
    return true;
}

bool chooseProviderVersion(
    std::span<const uint64_t> ids,
    std::span<const char* const> names,
    const char* expectedVersion,
    ProviderVersion& provider)
{
    if (!expectedVersion || ids.size() != names.size()) return false;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (names[i] && std::string_view(names[i]) == expectedVersion)
        {
            ProviderVersion selected{};
            if (!parseVersion(names[i], selected)) return false;
            selected.id = ids[i];
            provider = std::move(selected);
            return true;
        }
    }
    return false;
}

}
