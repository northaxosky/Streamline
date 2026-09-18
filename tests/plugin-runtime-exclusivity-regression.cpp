#include "source/core/sl.plugin-manager/runtimeExclusivity.h"

#include <iostream>
#include <string>
#include <vector>

int main()
{
    const std::vector<std::string> fsrConflicts{ "sl.dlss_g" };
    const std::vector<std::string> noConflicts{};

    if (!sl::plugin_manager::areRuntimeIncompatible(
            "sl.fsr_g", fsrConflicts, "sl.dlss_g", noConflicts) ||
        !sl::plugin_manager::areRuntimeIncompatible(
            "sl.dlss_g", noConflicts, "sl.fsr_g", fsrConflicts))
    {
        std::cerr << "One-sided runtime incompatibility was not enforced symmetrically.\n";
        return 1;
    }
    if (sl::plugin_manager::areRuntimeIncompatible(
            "sl.fsr", noConflicts, "sl.dlss", noConflicts))
    {
        std::cerr << "Unrelated plugins were marked runtime-incompatible.\n";
        return 1;
    }

    std::cout << "Plugin runtime exclusivity regressions passed.\n";
    return 0;
}
