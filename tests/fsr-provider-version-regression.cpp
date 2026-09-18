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
    if (sl::fsr::chooseProviderVersion(ids, names, "3.1.7", selected))
    {
        std::cerr << "Unavailable provider version was accepted.\n";
        return 1;
    }
    if (sl::fsr::parseVersion("3.1", selected) ||
        sl::fsr::parseVersion("3.1.5-preview", selected) ||
        sl::fsr::parseVersion(nullptr, selected))
    {
        std::cerr << "Malformed provider version was accepted.\n";
        return 1;
    }

    std::cout << "FSR provider version regressions passed.\n";
    return 0;
}
