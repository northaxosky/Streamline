#include "source/core/sl.plugin-manager/runtimeExclusivity.h"
#include "external/json/include/nlohmann/json.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: plugin-runtime-exclusivity-regression <fsr-g-json>\n";
        return 2;
    }

    std::ifstream stream(argv[1]);
    const auto config = nlohmann::json::parse(stream, nullptr, false);
    if (config.is_discarded() ||
        !config.contains("runtime_incompatible_plugins"))
    {
        std::cerr << "FSR frame-generation metadata is invalid.\n";
        return 1;
    }

    const auto fsrConflicts =
        config["runtime_incompatible_plugins"].get<std::vector<std::string>>();
    const std::vector<std::string> noConflicts{};

    if (config.value("name", "") != "sl.fsr_g" ||
        !sl::plugin_manager::areRuntimeIncompatible(
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
