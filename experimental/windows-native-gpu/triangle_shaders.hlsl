// SPDX-License-Identifier: MIT
struct Vertex { float4 position : SV_Position; float3 color : COLOR0; };
cbuffer DrawParameters : register(b0) { uint colorRotation; };
Vertex VSMain(uint id : SV_VertexID) {
    const float2 positions[3] = {float2(-0.75,-0.75),float2(0.0,0.75),float2(0.75,-0.75)};
    const float3 colors[3] = {float3(1,0,0),float3(0,1,0),float3(0,0,1)};
    Vertex result;
    result.position = float4(positions[id % 3], 0.5, 1.0);
    result.color = colors[(id + colorRotation) % 3];
    return result;
}
float4 PSMain(Vertex input) : SV_Target { return float4(input.color, 1.0); }
