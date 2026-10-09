cbuffer DlssConstants : register(b0) {
    row_major float4x4 reprojection;
    float4 viewport;
    float4 params;
    float4 jitter;
};

Texture2D<float> source_depth : register(t0);
RWTexture2D<float2> motion_vectors : register(u0);
RWTexture2D<float> depth_copy : register(u1);

[numthreads(8, 8, 1)] void main(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= (uint)viewport.z || thread_id.y >= (uint)viewport.w) return;

    const uint2 source_pixel = thread_id.xy + (uint2)viewport.xy;
    const float raw_depth = source_depth[source_pixel];
    const float depth_range = params.z - params.y;
    float depth = 1.0f;
    if (isfinite(raw_depth)) {
        depth = saturate((raw_depth - params.y) / depth_range);
    }

    // Use the same normalized depth for NGX and reprojection, in compact render-sized buffers.
    depth_copy[thread_id.xy] = depth;
    const float2 uv = (thread_id.xy + 0.5f) / viewport.zw;
    const float2 raster_position = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float2 current = raster_position - jitter.xy;
    // The CPU folds inverse jitter into reprojection; both endpoints exclude raster jitter.
    const float4 previous = mul(reprojection, float4(raster_position, depth, 1.0f));

    float2 motion = float2(0.0f, 0.0f);
    if (all(isfinite(previous)) && previous.w > 1e-6f) {
        motion = (previous.xy / previous.w - current) * float2(0.5f, -0.5f) * viewport.zw;
        if (!all(isfinite(motion))) motion = float2(0.0f, 0.0f);
    }

    // Prevent overflow when storing reprojection into R16G16_FLOAT.
    const float max_half = 65504.0f;
    motion_vectors[thread_id.xy] = clamp(motion * params.x, -max_half, max_half);
}
