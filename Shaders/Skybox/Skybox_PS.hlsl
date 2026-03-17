// Skybox: Equirect 2D を直接サンプル（面のつなぎ目なし）
// ワールド方向 → 経度・緯度 → (u,v) で 1 枚テクスチャを参照

Texture2D<float4> Equirect : register(t0);
SamplerState LinearSampler : register(s0);

cbuffer SkyboxCB : register(b0)
{
    matrix InvProj;
    matrix InvViewNoTrans;
};

struct PSInput
{
    float4 svpos : SV_POSITION;
    float2 ndc   : TEXCOORD0;
};

static const float PI = 3.14159265359;

float4 main(PSInput input) : SV_TARGET
{
    float4 clip = float4(input.ndc, 1.0, 1.0);
    float4 view = mul(clip, InvProj);
    float3 dirVS = normalize(view.xyz / max(view.w, 1e-6));
    float3 dirWS = normalize(mul(float4(dirVS, 0.0), InvViewNoTrans).xyz);

    // Equirect: lon = atan2(z,x), lat = asin(y). u=0..1 が経度、v=0 を上(北)に合わせる
    float lon = atan2(dirWS.z, dirWS.x);
    float lat = asin(clamp(dirWS.y, -1.0, 1.0));
    float u = lon / (2.0 * PI) + 0.5;
    float v = 0.5 - lat / PI;

    float4 color = Equirect.Sample(LinearSampler, float2(u, v));
    if (color.a < 0.01)
        color = float4(0.25, 0.45, 0.85, 1.0);
    return color;
}
