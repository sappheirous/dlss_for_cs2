// RCAS adapted from AMD FidelityFX FSR 1. See THIRD_PARTY_NOTICES.md for the MIT license.
cbuffer RcasConstants : register(b0) {
    uint2 output_size;
    float sharpness;
    float padding;
};

Texture2D<float4> source_color : register(t0);
RWTexture2D<float4> output_color : register(u0);

float4 load_color(int2 position) {
    return source_color.Load(int3(clamp(position, int2(0, 0), int2(output_size) - 1), 0));
}

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= output_size)) return;

    const int2 position = int2(id.xy);
    const float3 top = load_color(position + int2(0, -1)).rgb;
    const float3 left = load_color(position + int2(-1, 0)).rgb;
    const float4 center = load_color(position);
    const float3 right = load_color(position + int2(1, 0)).rgb;
    const float3 bottom = load_color(position + int2(0, 1)).rgb;

    const float3 minimum = min(min(top, left), min(right, bottom));
    const float3 maximum = max(max(top, left), max(right, bottom));
    const float3 hit_minimum = min(minimum, center.rgb) / max(4.0f * maximum, 1e-6f);
    const float3 hit_maximum = (1.0f - max(maximum, center.rgb)) / min(4.0f * minimum - 4.0f, -1e-6f);
    const float3 channel_lobe = max(-hit_minimum, hit_maximum);
    const float lobe = max(-0.1875f, min(max(channel_lobe.r, max(channel_lobe.g, channel_lobe.b)), 0.0f)) * sharpness;
    const float3 color = (center.rgb + lobe * (top + left + right + bottom)) / (1.0f + 4.0f * lobe);
    output_color[id.xy] = float4(color, center.a);
}
