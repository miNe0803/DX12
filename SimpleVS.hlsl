// --- [b0: Transform] ---
// DirectXMath は行メジャー・行ベクトル (pos * W * V * P)。cbuffer の既定は列メジャーなので row_major を付けないと
// 平行移動・スケールがシェーダー側で取り違えられ、エディタの Position が画面に反映されないことがある。
cbuffer Transform : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;
};

// --- [Vertex input] ---
// Must match C++ Vertex layout order
struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
};

// --- [Vertex output] ---
// Must match StandardPBR_PS.hlsl VSOutput exactly
struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
};

VSOutput vert(VSInput input)
{
    VSOutput output;

    // Transform
    float4 worldPos = mul(float4(input.pos, 1.0f), World);
    float4 viewPos = mul(worldPos, View);
    output.svpos = mul(viewPos, Proj);

    output.color = input.color;
    output.uv = input.uv;

    // Normal and tangent: fallback (0,0,1) / (1,0,0) if not present (avoid black in PS)
    float3 n = (length(input.normal) > 0.1) ? input.normal : float3(0, 0, 1);
    float3 t = (length(input.tangent) > 0.1) ? input.tangent : float3(1, 0, 0);

    output.normal = normalize(mul(n, (float3x3) World));
    output.tangent = normalize(mul(t, (float3x3) World));
    output.worldPos = worldPos.xyz;

    return output;
}
