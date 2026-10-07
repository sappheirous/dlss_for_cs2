cbuffer UpscaleConstants : register(b0) {
    float4 uv_transform;
    float4 uv_bounds;
};

Texture2D<float4> source_color : register(t0);
SamplerState linear_clamp : register(s0);

struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex vertex_main(uint id : SV_VertexID) {
    Vertex vertex;
    vertex.uv = float2((id << 1) & 2, id & 2);
    vertex.position = float4(vertex.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vertex;
}

float4 pixel_main(Vertex vertex) : SV_Target {
    const float2 uv = clamp(vertex.uv * uv_transform.xy + uv_transform.zw, uv_bounds.xy, uv_bounds.zw);
    return source_color.SampleLevel(linear_clamp, uv, 0.0f);
}
