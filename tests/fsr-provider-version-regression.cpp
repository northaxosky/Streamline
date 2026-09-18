#include "source/plugins/sl.fsr.common/providerVersion.h"

#include <array>
#include <iostream>

int main()
{
    std::array<uint64_t, 3> ids{ 71, 143, 811 };
    std::array<const char*, 3> names{ "4.0.1", "3.1.5", "3.1.6" };
    sl::fsr::ProviderVersion selected{};

    if (!sl::fsr::chooseProviderVersion(ids, names, "3.1.5", selected) ||
        selected.id != 143 || selected.major != 3 ||
        selected.minor != 1 || selected.patch != 5 ||
        selected.name != "3.1.5")
    {
        std::cerr << "Exact public provider version was not selected.\n";
        return 1;
    }
    const sl::fsr::ProviderVersion retained = selected;
    if (sl::fsr::chooseProviderVersion(ids, names, "3.1.7", selected) ||
        sl::fsr::chooseProviderVersion(
            std::span<const uint64_t>(ids.data(), ids.size() - 1),
            names,
            "3.1.5",
            selected) ||
        sl::fsr::chooseProviderVersion(ids, names, nullptr, selected) ||
        sl::fsr::parseVersion("3.1", selected) ||
        sl::fsr::parseVersion("3.1.5-preview", selected) ||
        sl::fsr::parseVersion(nullptr, selected) ||
        selected.id != retained.id ||
        selected.major != retained.major ||
        selected.minor != retained.minor ||
        selected.patch != retained.patch ||
        selected.name != retained.name)
    {
        std::cerr << "Invalid provider data was accepted or changed the prior selection.\n";
        return 1;
    }

    std::cout << "FSR provider version regressions passed.\n";
    return 0;
}
