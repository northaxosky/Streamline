#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HLSL_SM 51
#define FFX_HALF 0
#define FFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL 0
#define FSR3UPSCALER_BIND_CB_FSR3UPSCALER 0

#include "../external/fidelityfx-sdk/Kits/FidelityFX/upscalers/fsr3/include/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_hlsl.h"

RWStructuredBuffer<float4> results : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    const float3 input = float3(0.5f, 0.5f, 0.5f);
    results[0] = float4(DecodeInputColor(input), 1.0f);
    results[1] = float4(EncodeOutputColor(DecodeInputColor(input)), 1.0f);
    results[2] = float4(DecodeInputColor(float3(-0.25f, 0.5f, 1.0f)), 1.0f);
}
