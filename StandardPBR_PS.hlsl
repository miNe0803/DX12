// --- [VS output struct] ---
struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;   // world-space normal
    float3 tangent : TANGENT; // world-space tangent
    float3 worldPos : TEXCOORD1;
    float4 nprPerMesh : TEXCOORD2;
};

// --- [Textures and sampler] --- (space0: VS instance buffer uses space1)
SamplerState smp : register(s0);
Texture2D _AlbedoMap : register(t0, space0);
Texture2D _NormalMap : register(t1, space0);
Texture2D _MetallicMap : register(t2, space0);
Texture2D _RoughnessMap : register(t3, space0);
Texture2D _RampTex : register(t4, space0);
TextureCube _PrefilterEnv  : register(t5, space0);
TextureCube _IrradianceMap : register(t6, space0);
Texture2D _BrdfLut         : register(t7, space0);

// --- [Material params (b1)] ---
// RimParams.y = NormalScale, CameraPos.xyz = カメラ位置（反射用）
cbuffer MaterialParams : register(b1, space0)
{
    float4 RimParams;
    float4 CameraPos;
    float4 NprTuning;
    float4 NprTuning2;
};

// --- [Pixel shader main] ---
float4 main(VSOutput input) : SV_TARGET
{
    // 1. Sample textures（質感はピクセル単位でマップから自動判別）
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    clip(albedo.a - 0.5f);
    // sRGB → リニア（アルベドのみ。法線/メタル/ラフはデータなので変換しない）
    albedo.rgb = pow(albedo.rgb, 2.2);
    float4 nSample = _NormalMap.Sample(smp, input.uv);
    float metallic = _MetallicMap.Sample(smp, input.uv).r;
    float roughness =_RoughnessMap.Sample(smp, input.uv).r;
    // マップ未設定時（C++で黒/グレーにしているが、読み込み失敗で白が来た場合のフォールバック）
    if (metallic >= 0.98 && roughness >= 0.98)
    {
        metallic = 0.0;
        roughness = 0.92;
    }
    roughness = max(roughness, 0.04);

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

    // 事前準備: 視線・F0・フレネル（5/6番で共有）
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float NdotV = max(dot(worldNormal, V), 0.0001);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);
    float3 F = F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);

    // 5. Ambient (Diffuse IBL) — Irradiance を法線方向でサンプル、エネルギー保存則で kD
    float3 irradiance = _IrradianceMap.Sample(smp, worldNormal).rgb;
    float3 kD = 1.0 - F;
    kD *= (1.0 - metallic);
    float3 ambientLight = irradiance * albedo.rgb * kD;

    // 6. Reflection (Specular IBL) — Split Sum: PrefilterEnv * (F0*A + B)
    float3 R = reflect(-V, worldNormal);
    const float PREFILTER_MIP_COUNT = 5.0;
    float mip = roughness * (PREFILTER_MIP_COUNT - 1.0);
    float3 prefiltered = _PrefilterEnv.SampleLevel(smp, R, mip).rgb;
    float2 brdf = _BrdfLut.Sample(smp, float2(NdotV, roughness)).rg;
    float3 specularPart = prefiltered * (F0 * brdf.x + brdf.y);

    // 7. Composite
    float3 finalColor = directLight + ambientLight + specularPart;
    // t4 / NPR インスタンス拡張: 未使用警告回避
    finalColor += _RampTex.Sample(smp, float2(0.5f, 0.5f)).rgb * 0.0f;
    finalColor += (input.nprPerMesh.x + NprTuning2.x) * 0.0;

    return float4(finalColor, albedo.a);
}
