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
TextureCube _EnvMap : register(t4);  // 環境マップ（空の光・IBL拡散）

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
    // sRGB → リニア（アルベドのみ。法線/メタル/ラフはデータなので変換しない）
    albedo.rgb = pow(albedo.rgb, 2.2);
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

    // 4. Direct lighting（補助的なディレクショナル）
    float3 L = normalize(float3(0.5, 0.7, -1.0));
    float3 LightColor = float3(1.2, 1.2, 1.2);
    float diffuseFactor = max(dot(worldNormal, L), 0.0);
    float3 directLight = albedo.rgb * diffuseFactor * LightColor;

    // 5. Ambient = 環境マップ（空）から法線方向にサンプル → 空の光で照らす
    float3 envDiffuse = _EnvMap.Sample(smp, worldNormal).rgb;
    float3 ambientLight = albedo.rgb * envDiffuse;
    // フォールバック: 環境が真っ黒な場合用の最小明るさ
    float3 fallbackAmbient = float3(0.03, 0.03, 0.04);
    ambientLight = max(ambientLight, albedo.rgb * fallbackAmbient);

    // 6. Composite（リニアHDRのまま出力。ガンマは ToneMap_PS で一度だけかける）
    float3 finalColor = directLight + ambientLight;

    return float4(finalColor, albedo.a);
}
