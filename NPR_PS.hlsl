// NPR toon shading pixel shader
// - t0: Albedo (sRGB encoded; we linearize here to match current Texture2D behavior)
// - t1: Normal map (data)
// - t2: Ramp (linear)
// - b1: RimParams (NormalScale) + CameraPos (unused here)

struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL; // world-space normal
    float3 tangent : TANGENT; // world-space tangent
    float3 worldPos : TEXCOORD1;
};

SamplerState smp : register(s0);
Texture2D _AlbedoMap : register(t0, space0);
Texture2D _NormalMap : register(t1, space0);
Texture2D _RampTex : register(t2, space0);

cbuffer MaterialParams : register(b1, space0)
{
    float4 RimParams; // RimParams.y = NormalScale
    float4 CameraPos; // unused
};

float4 main(VSOutput input) : SV_TARGET
{
    float4 albedo = _AlbedoMap.Sample(smp, input.uv);
    clip(albedo.a - 0.5f);

    // 1. sRGB -> Linear デコード（既存PBRと整合）
    albedo.rgb = pow(albedo.rgb, 2.2);

    // 2. NormalMapのデコードとTBNワールド法線化 (完璧です)
    float4 nSample = _NormalMap.Sample(smp, input.uv);
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent);
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);

    float3 decodedNormal = nSample.rgb * 2.0 - 1.0;
    float nScale = (RimParams.y > 0.001) ? RimParams.y : 1.0;
    float3 normalTS = normalize(float3(0, 0, 1) + (decodedNormal - float3(0, 0, 1)) * nScale);
    float3 worldNormal = normalize(mul(normalTS, TBN));

    // 3. ライト計算（ハードコードで突破）
    float3 L = normalize(float3(0.5, 0.7, -1.0));
    float NdotL = dot(worldNormal, L);
    float halfLambert = saturate(NdotL * 0.5 + 0.5);

    // 4. 量子化によるトゥーン化 (完璧です)
    const float Steps = 5.0;
    float q = clamp(floor(halfLambert * Steps), 0.0, Steps - 1.0);
    float u = (q + 0.5) / Steps;

    // Rampからの階調サンプリング (Mip0強制)
    float3 rampColor = _RampTex.SampleLevel(smp, float2(u, 0.5), 0).rgb;

    // ★★★ ここからが修正（発光防止と背景馴染み）★★★

    // 5. NPR専用のアアンビエント（環境光）の色
    // ※桜の背景に合わせて少し青紫っぽくしています。適宜調整してください。
    // 日向(1.0)と影(0.0)のコントラストを弱め、影を明るく馴染ませる色。
    float3 ambientColor = float3(0.20, 0.20, 0.25);

    // 6. 最終合成 (Lerpハック)
    // - shadowColor : Rampが0.0(影)の時の色。Albedoに環境光を掛ける。
    // - lightColor  : Rampが1.0(日向)の時の色。Albedoそのまま。
    // rampColor のR値を使って、shadowColor と lightColor を滑らかに(トゥーン化を保って)繋ぐ。
    float3 shadowColor = albedo.rgb * ambientColor;
    float3 lightColor = albedo.rgb;
    
    // RampColorが「陰影の形」を決めるマスクになる。
    float3 finalColor = lerp(shadowColor, lightColor, rampColor.r);
    float nbrExposure = 0.8f;
    finalColor *= nbrExposure; // 全体の明るさ調整（必要に応じて調整してください）

    return float4(finalColor, albedo.a);
}
