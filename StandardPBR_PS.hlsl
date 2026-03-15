// --- [VS output struct] ---
struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;   // world-space normal
    float3 tangent : TANGENT; // world-space tangent
};

// --- [Textures and sampler] ---
SamplerState smp : register(s0);
Texture2D _AlbedoMap : register(t0);
Texture2D _NormalMap : register(t1);
Texture2D _MetallicMap : register(t2);
Texture2D _RoughnessMap : register(t3);

// --- [Material params (b1)] ---
// RimParams.y = NormalScale
cbuffer MaterialParams : register(b1)
{
    float4 RimParams;
};

// --- [Pixel shader main] ---
float4 main(VSOutput input) : SV_TARGET
{
    // 1. Sample textures
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    float4 nSample = _NormalMap.Sample(smp, input.uv);

    // 2. TBN matrix (tangent -> world)
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    // 3. Normal map decode and NormalScale
    float3 decodedNormal = nSample.rgb * 2.0 - 1.0;
    float nScale = (RimParams.y > 0.001) ? RimParams.y : 1.0;
    float3 normalTS = normalize(float3(0, 0, 1) + (decodedNormal - float3(0, 0, 1)) * nScale);
    float3 worldNormal = normalize(mul(normalTS, TBN));

    // 4. Direct lighting
    float3 L = normalize(float3(0.5, 0.7, -1.0));
    float3 LightColor = float3(2.5, 2.5, 2.5);
    float diffuseFactor = max(dot(worldNormal, L), 0.0);
    float3 directLight = albedo.rgb * diffuseFactor * LightColor;

    // 5. Ambient (avoid pure black)
    float3 AmbientColor = float3(0.15, 0.15, 0.2);
    float3 ambientLight = albedo.rgb * AmbientColor;

    // 6. Composite
    float3 finalColor = directLight + ambientLight;

    // 7. Gamma correction
    finalColor = pow(finalColor, 1.0 / 2.2);

    return float4(finalColor, albedo.a);
}
