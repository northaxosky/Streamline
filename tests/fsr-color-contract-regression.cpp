#include "include/sl_fsr.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/api/include/ffx_api_types.h"
#include "external/fidelityfx-sdk/Kits/FidelityFX/upscalers/include/ffx_upscale.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

static_assert(static_cast<uint32_t>(sl::FSRColorSpace::eLinear) == 0);
static_assert(static_cast<uint32_t>(sl::FSRColorSpace::eSRGB) == 1);
static_assert(static_cast<uint32_t>(sl::FSRColorSpace::ePQ) == 2);
static_assert(static_cast<uint32_t>(sl::FSRColorSpace::eGamma22) == 3);
static_assert(FFX_API_BACKBUFFER_TRANSFER_FUNCTION_GAMMA_2_2 == 3);
static_assert(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_GAMMA_2_2 == (1 << 3));

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

void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void checkContains(
    const std::string& source,
    std::string_view expected,
    const char* message)
{
    check(source.find(expected) != std::string::npos, message);
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 9)
    {
        std::cerr << "usage: fsr-color-contract-regression "
            "<sr-provider> <sr-shader> <core-shader> <fg-shader> "
            "<of-shader> <swapchain-source> <sr-plugin> <fg-plugin>\n";
        return 2;
    }

    const float gamma22Linear = std::pow(0.5f, 2.2f);
    const float srgbLinear = std::pow((0.5f + 0.055f) / 1.055f, 2.4f);
    check(
        std::abs(gamma22Linear - srgbLinear) > 0.003f,
        "gamma 2.2 remains numerically distinct from IEC sRGB");
    check(
        std::abs(std::pow(gamma22Linear, 1.0f / 2.2f) - 0.5f) < 0.00001f,
        "gamma 2.2 decode and encode are reciprocal");

    const std::string srProvider = readFile(argv[1]);
    const std::string srShader = readFile(argv[2]);
    const std::string coreShader = readFile(argv[3]);
    const std::string fgShader = readFile(argv[4]);
    const std::string opticalFlowShader = readFile(argv[5]);
    const std::string swapchain = readFile(argv[6]);
    const std::string srPlugin = readFile(argv[7]);
    const std::string fgPlugin = readFile(argv[8]);

    checkContains(
        srProvider,
        "FFX_FSR3UPSCALER_DISPATCH_NON_LINEAR_COLOR_GAMMA_2_2",
        "FSR 3.1.5 provider preserves the gamma 2.2 dispatch contract");
    checkContains(
        srShader,
        "ffxLinearFromGamma(",
        "FSR decodes gamma 2.2 before temporal processing");
    checkContains(
        srShader,
        "ffxGammaFromLinear(",
        "FSR re-encodes gamma 2.2 in its existing output path");
    checkContains(
        srShader,
        "FfxFloat32x4(EncodeOutputColor(fColor), 1.f)",
        "FSR preserves its existing opaque-alpha output contract");
    checkContains(
        coreShader,
        "return pow(color, ffxBroadcast3(power));",
        "the pinned SDK defines gamma decode as pow(c, power)");
    checkContains(
        coreShader,
        "return pow(value, ffxBroadcast3(power));",
        "the pinned SDK defines gamma encode as pow(c, reciprocal power)");
    checkContains(
        fgShader,
        "ffxLinearFromGamma(",
        "frame interpolation linearizes gamma 2.2 for luminance decisions");
    checkContains(
        opticalFlowShader,
        "backbufferTransferFunction == 3",
        "optical flow treats gamma 2.2 as an SDR perceptual input");
    checkContains(
        swapchain,
        "FFX_API_BACKBUFFER_TRANSFER_FUNCTION_GAMMA_2_2",
        "DXGI G22 maps to the gamma 2.2 provider contract");
    checkContains(
        srPlugin,
        "if (options.useAutoExposure == Boolean::eTrue) flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;",
        "the color translation preserves automatic exposure");
    checkContains(
        srPlugin,
        "options->colorSpace == FSRColorSpace::eGamma22",
        "the Streamline SR ABI exposes gamma 2.2 explicitly");
    checkContains(
        fgPlugin,
        "case FSRColorSpace::eGamma22: return FFX_API_BACKBUFFER_TRANSFER_FUNCTION_GAMMA_2_2;",
        "Streamline frame generation does not alias gamma 2.2 to sRGB");

    if (failures)
    {
        std::cerr << failures << " FSR color contract regression(s) failed\n";
        return 1;
    }
    std::cout << "FSR color contract regressions passed.\n";
    return 0;
}
