#include "include/sl_fsr_g.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <type_traits>

static_assert(
    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION ==
    FFX_FRAMEGENERATION_SWAPCHAIN_DX12_MAKE_VERSION(3, 1, 7));
static_assert(
    FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_HOST_INPUT_COMPLETION_DX12_V1 ==
    FFX_API_MAKE_BACKEND_EFFECT_SUB_ID(
        FFX_API_BACKEND_ID_DX12,
        FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN,
        0x0c));
static_assert(std::is_same_v<
    decltype(ffxQueryDescFrameGenerationSwapChainHostInputCompletionDX12V1{}.pOutFence),
    void**>);
static_assert(std::is_same_v<
    decltype(ffxQueryDescFrameGenerationSwapChainHostInputCompletionDX12V1{}.pOutFenceValue),
    uint64_t*>);
static_assert(static_cast<uint32_t>(sl::FSRGCompletionMode::eFence) == 1);
static_assert(static_cast<uint32_t>(sl::FSRGCompletionMode::eVendorCompletionUnavailable) == 2);

namespace
{

int failures{};

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };
}

void checkContains(
    const std::string& source,
    std::string_view expected,
    const char* message)
{
    if (source.find(expected) == std::string::npos)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: fsr-completion-contract-regression "
            "<swapchain-source> <provider-source> <plugin-source>\n";
        return 2;
    }

    const std::string swapchain = readFile(argv[1]);
    const std::string provider = readFile(argv[2]);
    const std::string plugin = readFile(argv[3]);
    checkContains(
        swapchain,
        "ID3D12Fence* fence = presentInfo.presentFence;",
        "completion query uses the final presentation fence");
    checkContains(
        swapchain,
        "const UINT64 fenceValue = framesSentForPresentation;",
        "completion query snapshots the last scheduled presentation value");
    checkContains(
        swapchain,
        "fence->AddRef();",
        "completion query transfers an owned fence reference");
    checkContains(
        swapchain,
        "GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS",
        "UI callback ownership follows the project-built provider module");
    checkContains(
        provider,
        "ffxGetFrameinterpolationHostInputCompletionDX12(",
        "the public provider routes the completion query");
    checkContains(
        plugin,
        "if (ctx.swapchainContext)",
        "completion capability is queried once the private swapchain exists");
    checkContains(
        plugin,
        "ctx.runtime.query(&ctx.swapchainContext, &completion.header)",
        "the Streamline plugin queries the vendor completion dependency");
    checkContains(
        plugin,
        "state->completionMode = FSRGCompletionMode::eFence;",
        "an idle or active private swapchain reports the fence contract");
    checkContains(
        plugin,
        "state->completionMode = FSRGCompletionMode::eVendorCompletionUnavailable;",
        "an unsupported provider is distinguishable from an uncreated swapchain");
    checkContains(
        plugin,
        "state->completionFenceValue = 0;",
        "an idle supported provider can report value zero without a dependency");

    if (failures)
    {
        std::cerr << failures << " FSR completion contract regression(s) failed\n";
        return 1;
    }
    std::cout << "FSR completion contract regressions passed.\n";
    return 0;
}
