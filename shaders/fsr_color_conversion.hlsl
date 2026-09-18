[[vk::binding(0)]] Texture2D<float4> inputColor : register(t0);
[[vk::binding(1)]] RWTexture2D<float4> outputColor : register(u0);

[[vk::binding(2)]] cbuffer ConversionConstants : register(b0)
{
    uint2 conversionSize;
    uint conversionDirection;
    uint unused;
};

static const uint kGamma22ToLinear = 0;
static const uint kLinearToGamma22 = 1;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= conversionSize))
    {
        return;
    }

    const float4 source = inputColor[pixel];
    float3 converted;
    if (conversionDirection == kGamma22ToLinear)
    {
        converted = pow(max(source.rgb, 0.0), 2.2);
    }
    else
    {
        converted = pow(max(source.rgb, 0.0), 1.0 / 2.2);
    }
    outputColor[pixel] = float4(converted, source.a);
}
