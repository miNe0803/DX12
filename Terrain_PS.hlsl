struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
};

cbuffer Transform : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;
};

SamplerState smp : register(s0);
Texture2D _TreeMask    : register(t0);
Texture2D _NatureMask  : register(t1);
Texture2D _GroundDiff  : register(t2);
Texture2D _GroundDisp  : register(t3);
Texture2D _RiversMask  : register(t4); // WaterColor_Mask_main.png (主要川マスク, シェーダ内ぼかし)
Texture2D _SnowMask    : register(t5); // Snow_Snow.png
TextureCube _PrefilterEnv  : register(t6);
TextureCube _IrradianceMap : register(t7);
Texture2D _BrdfLut         : register(t8);

// 拡張テレインテクスチャ (t9-t12) — Gaea が出力した補助マスクを総動員
Texture2D _RiversDirection : register(t9);   // RG: 川の流れ方向
Texture2D _WaterColorTint  : register(t10);  // RGB: 川/湿地の本物の水色
Texture2D _FreshWaterMask  : register(t11);  // 湿地・水際のマスク (川岸を緑/苔で潤す)
Texture2D _InhibitorsMask  : register(t12);  // 植生抑制マスク (岩肌・痩せ地; 草を生やさない領域)

cbuffer TerrainParams : register(b1)
{
    float4 LayerColor[6]; // 0=ground, 1..3=tree species, 4=snow, 5=river
    float4 CameraPos;
    // x: stage 0..3, y: cheap path on, z: grazing threshold, w: near preserve (m, 0=off)
    float4 DebugParams;
    float4 SunDirection; // .xyz = normalised dir TO light
    float4 SunColor;     // .rgb
};

// Shadow mapping (space2) — 4 cascades
Texture2DArray _ShadowMap : register(t0, space2);
SamplerComparisonState shadowSampler : register(s1, space2);
cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[4];
    float4 CascadeSplits; // .x/.y/.z/.w = view-space far for cascade 0/1/2/3
};

static const float kShadowBias = 0.005;
static const float kShadowTexelSize = 1.0 / 512.0;

float SampleShadowPCF(float3 worldPos, float viewDepth)
{
    uint cascade = 0; // カスケード 0 のみ使用
    if (viewDepth >= CascadeSplits.x)
        return 1.0;

    float4 lightClip = mul(float4(worldPos, 1.0), LightVP[cascade]);
    float3 projCoord = lightClip.xyz / lightClip.w;
    float2 shadowUV = projCoord.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    if (shadowUV.x < 0 || shadowUV.x > 1 || shadowUV.y < 0 || shadowUV.y > 1)
        return 1.0;

    float compareDepth = projCoord.z - kShadowBias;
    // 4-tap PCF (2×2): 9-tap に対して品質ほぼ同等、サンプル数 56% 削減
    float shadow = 0;
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(shadowUV + float2(-0.5, -0.5) * kShadowTexelSize, (float)cascade), compareDepth);
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(shadowUV + float2( 0.5, -0.5) * kShadowTexelSize, (float)cascade), compareDepth);
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(shadowUV + float2(-0.5,  0.5) * kShadowTexelSize, (float)cascade), compareDepth);
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(shadowUV + float2( 0.5,  0.5) * kShadowTexelSize, (float)cascade), compareDepth);
    return shadow * 0.25;
};

float3 BlendTreeLayers(float3 mask, float3 ground, float3 tree0, float3 tree1, float3 tree2)
{
    float weightSum = mask.r + mask.g + mask.b;
    if (weightSum <= 1e-4)
        return ground;

    return (tree0 * mask.r + tree1 * mask.g + tree2 * mask.b) / weightSum;
}

// 川マスクの 5x5 ガウスぼかし (TerrainVS / Water_PS と同じカーネル)
float SmoothRiverMaskTP(float2 uv)
{
    const float t = 1.0 / 512.0;
    float v = 0.0;
    v += _RiversMask.SampleLevel(smp, uv, 0).r * 4.0;
    v += _RiversMask.SampleLevel(smp, uv + float2( t,  0), 0).r * 2.0;
    v += _RiversMask.SampleLevel(smp, uv + float2(-t,  0), 0).r * 2.0;
    v += _RiversMask.SampleLevel(smp, uv + float2( 0,  t), 0).r * 2.0;
    v += _RiversMask.SampleLevel(smp, uv + float2( 0, -t), 0).r * 2.0;
    v += _RiversMask.SampleLevel(smp, uv + float2( t,  t), 0).r;
    v += _RiversMask.SampleLevel(smp, uv + float2(-t,  t), 0).r;
    v += _RiversMask.SampleLevel(smp, uv + float2( t, -t), 0).r;
    v += _RiversMask.SampleLevel(smp, uv + float2(-t, -t), 0).r;
    v += _RiversMask.SampleLevel(smp, uv + float2( 2*t, 0), 0).r;
    v += _RiversMask.SampleLevel(smp, uv + float2(-2*t, 0), 0).r;
    v += _RiversMask.SampleLevel(smp, uv + float2( 0,  2*t), 0).r;
    v += _RiversMask.SampleLevel(smp, uv + float2( 0, -2*t), 0).r;
    return v / 20.0;
}

float4 main(VSOutput input) : SV_TARGET
{
    //return float4(1,0,1,1); // マゼンタテスト: テレイン範囲確認用。コメント解除で有効化
    float2 uv = input.uv;
    float2 groundUv = uv * 8.0f;
    float3 groundDiff = _GroundDiff.Sample(smp, groundUv).rgb;

    float3 N = input.normal;
    if (length(N) < 0.001f) N = float3(0.0f, 1.0f, 0.0f);
    else N = normalize(N);
    float3 L = normalize(SunDirection.xyz);
    float ndotl = max(dot(N, L), 0.0f);

    // Shadow — コントラスト強化: ambient を下げて影を際立たせる
    float4 viewPos = mul(float4(input.worldPos, 1.0), View);
    float shadowFactor = SampleShadowPCF(input.worldPos, viewPos.z);
    int kTerrainDebugStage = clamp((int)round(DebugParams.x), 0, 3);

    float3 albedo = groundDiff;
    if (kTerrainDebugStage == 0)
    {
        float3 c0 = albedo * (0.15 + 0.85 * ndotl * shadowFactor);
        return float4(c0, 1.0f);
    }

    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float nDotV = abs(dot(N, V));
    float grazing = 1.0f - nDotV;
    float distToCam = distance(CameraPos.xyz, input.worldPos);

    // Sample water/snow masks BEFORE cheapPath
    // Rivers_Rivers をぼかして滑らかな勾配にする (TerrainVS / Water_PS と整合)
    float riverDepthRaw = saturate(SmoothRiverMaskTP(uv));
    float waterMask = 0.0; // 水面は別パス(Water_PS)で描画。地形は水底として表示
    float snowMask = saturate(_SnowMask.SampleLevel(smp, uv, 0).r);
    // 川底マスク (水が覆う領域; Water_PS の discard 閾値 0.20 と整合)
    float riverbedMask = smoothstep(0.20, 0.50, riverDepthRaw);

    // 診断: cheapPath 無効化で円形境界が消えるか確認
    bool useCheapPath = false;

    if (useCheapPath)
    {
        float3 cheapLit = albedo * (0.15f + 0.85f * ndotl);
        if (waterMask > 0.0f)
            cheapLit = lerp(cheapLit, float3(0.15, 0.30, 0.45), waterMask * 0.7);
        if (snowMask > 0.1f)
            cheapLit = lerp(cheapLit, float3(0.90, 0.92, 0.98) * 0.7, snowMask * smoothstep(0.45, 0.85, dot(N, float3(0,1,0))));
        cheapLit *= (0.3 + 0.7 * shadowFactor);
        return float4(cheapLit, 1.0f);
    }

    float disp = saturate(_GroundDisp.Sample(smp, groundUv).r);
    float3 treeMask = saturate(_TreeMask.Sample(smp, uv).rgb);

    albedo *= lerp(0.90f, 1.15f, disp);
    float3 lit = albedo * (0.15f + 0.85f * ndotl);
    if (kTerrainDebugStage == 1)
    {
        lit *= (0.3 + 0.7 * shadowFactor);
        return float4(lit, 1.0f);
    }

    float treeWeight = saturate(max(treeMask.r, max(treeMask.g, treeMask.b)));
    float3 treeTint = BlendTreeLayers(treeMask, lit, LayerColor[1].rgb, LayerColor[2].rgb, LayerColor[3].rgb);
    float3 c = lerp(lit, lit * treeTint, treeWeight * 0.35f);
    if (kTerrainDebugStage == 2)
        return float4(c, 1.0f);

    // 川底には雪・樹木は乗らない (水が覆う領域)
    snowMask *= (1.0 - riverbedMask);

    // Snow
    if (snowMask > 0.01f)
    {
        float slopeAngle = dot(N, float3(0, 1, 0));
        float slopeFactor = smoothstep(0.45, 0.85, slopeAngle);
        snowMask *= slopeFactor;
        float noise = saturate(disp * 1.5 - 0.2);
        snowMask = saturate(snowMask - (1.0 - noise) * 0.25);
        float3 snowAlbedo = float3(0.90, 0.92, 0.98) + disp * 0.08;
        c = lerp(c, snowAlbedo * (0.30 + 0.70 * ndotl * shadowFactor), snowMask);
        float3 H = normalize(L + V);
        c += pow(max(dot(N, H), 0), 128.0) * snowMask * 0.25 * float3(1.0, 0.98, 0.95);
    }

    // 川底 (水没する地形): 濡れて暗く、わずかに青緑へシフト
    if (riverbedMask > 0.0)
    {
        float3 wetTint = float3(0.55, 0.70, 0.80);
        float3 wetAlbedo = c * wetTint * 0.6; // 濡れて暗く、色相シフト
        c = lerp(c, wetAlbedo, riverbedMask);
    }

    // ---- 湿地 (FreshWater): 川岸の苔・湿った緑のティント ----
    float freshWater = saturate(_FreshWaterMask.SampleLevel(smp, uv, 0).r);
    if (freshWater > 0.05)
    {
        // 値が大きい = 水が近い = 苔・湿地・濡れた草の色
        float3 mossyGreen = float3(0.32, 0.42, 0.22) * (0.55 + 0.45 * ndotl * shadowFactor);
        c = lerp(c, mossyGreen, freshWater * 0.55 * (1.0 - riverbedMask));
    }

    // ---- INHIBITORS: 痩せ地・岩肌のマスク。彩度を下げて荒涼感を演出 ----
    float inhibitor = saturate(_InhibitorsMask.SampleLevel(smp, uv, 0).r);
    if (inhibitor > 0.05)
    {
        // 彩度低下 + 暗化 + 微赤 (錆/酸化を示唆)
        float gray = dot(c, float3(0.299, 0.587, 0.114));
        float3 desat = lerp(c, float3(gray, gray, gray) * float3(1.05, 0.96, 0.90), 0.65);
        c = lerp(c, desat, inhibitor * 0.50);
    }

    // Water: 物理ベース水面（水深 + 流れ + フォーム + PBR反射）
    if (waterMask > 0.0f)
    {
        const float time = CameraPos.w;
        const float wrappedTime = fmod(time, 500.0);

        // ---- 1) 水深計算（Rivers_Depth は既に深度勾配）----
        float waterDepth = saturate(riverDepthRaw); // 0=岸辺, 1=深い
        float depthMeters = waterDepth * 3.0; // 疑似メートル

        // ---- 2) フロー方向を川マスクの勾配から算出（上流→下流の自然な流れ）----
        const float texelUV = 1.0 / 512.0;
        float mR = _RiversMask.Sample(smp, uv + float2( texelUV, 0)).r;
        float mL = _RiversMask.Sample(smp, uv + float2(-texelUV, 0)).r;
        float mU = _RiversMask.Sample(smp, uv + float2(0,  texelUV)).r;
        float mD = _RiversMask.Sample(smp, uv + float2(0, -texelUV)).r;
        // 川幅の勾配: 細い方が上流、太い方が下流。勾配ベクトルを流れ方向に変換
        float2 gradRiver = float2(mR - mL, mU - mD);
        float gradLen = length(gradRiver);
        // 勾配の垂直方向が川に沿った流れ方向
        float2 flowTangent = (gradLen > 1e-4) ? normalize(float2(-gradRiver.y, gradRiver.x)) : float2(1, 0);

        // 3 レイヤーの波（主流れに沿って + 変化）
        float2 flowDir1 = flowTangent;
        float2 flowDir2 = normalize(flowTangent + float2(-0.3, 0.5));
        float2 flowDir3 = normalize(flowTangent + float2(0.4, -0.3));

        float2 waveUV1 = uv * 4.0 + flowDir1 * wrappedTime * 0.02;
        float2 waveUV2 = uv * 10.0 + flowDir2 * wrappedTime * 0.04;
        float2 waveUV3 = uv * 20.0 + flowDir3 * wrappedTime * 0.08;

        float w1 = _GroundDisp.Sample(smp, waveUV1).r;
        float w2 = _GroundDisp.Sample(smp, waveUV2).r;
        float w3 = _GroundDisp.Sample(smp, waveUV3).r;

        // 波の合成: 大きな波 + 中波 + 細波
        float3 waterN = normalize(float3(
            (w1 - 0.5) * 0.20 + (w2 - 0.5) * 0.10 + (w3 - 0.5) * 0.05,
            1.0,
            (w1 - 0.5) * 0.15 + (w2 - 0.5) * 0.12 + (w3 - 0.5) * 0.06));

        // ---- 3) PBR マテリアル（水深に応じた色）----
        const float waterRoughness = lerp(0.15, 0.05, waterDepth); // 浅瀬は乱れ、深い所は鏡面
        const float3 F0 = float3(0.02, 0.02, 0.02);
        // 水深による色: 浅瀬=エメラルド、深部=紺碧
        float3 shallowColor = float3(0.35, 0.55, 0.50);
        float3 deepColor = float3(0.02, 0.10, 0.18);
        float3 albedoWater = lerp(shallowColor, deepColor, waterDepth);

        float NdotV = saturate(dot(waterN, V));
        float NdotL = saturate(dot(waterN, L));
        float3 H = normalize(L + V);
        float NdotH = saturate(dot(waterN, H));
        float VdotH = saturate(dot(V, H));

        // Schlick Fresnel
        float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

        // GGX
        float a = waterRoughness * waterRoughness;
        float a2 = a * a;
        float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
        float D = a2 / (3.14159265 * denom * denom);

        // Smith Geometry
        float k = (waterRoughness + 1.0) * (waterRoughness + 1.0) * 0.125;
        float Gv = NdotV / (NdotV * (1.0 - k) + k);
        float Gl = NdotL / (NdotL * (1.0 - k) + k);
        float G = Gv * Gl;

        float3 specularBRDF = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
        float3 directSpec = specularBRDF * SunColor.rgb * NdotL * shadowFactor;

        // IBL
        float3 R = reflect(-V, waterN);
        float3 prefilterColor = _PrefilterEnv.SampleLevel(smp, R, waterRoughness * 4.0).rgb;
        float2 brdfLut = _BrdfLut.Sample(smp, float2(NdotV, waterRoughness)).rg;
        float3 indirectSpec = prefilterColor * (F * brdfLut.x + brdfLut.y);

        // ---- 4) Beer's Law: 水深による光吸収 ----
        // 赤系が先に吸収される → 深いほど青緑に
        float3 absorption = float3(0.6, 0.15, 0.05); // 色ごとの吸収係数
        float3 attenuation = exp(-absorption * depthMeters);
        float3 terrainThroughWater = c * attenuation;

        // ---- 5) 拡散成分（散乱光）----
        float3 kD = (1.0 - F);
        float3 irradiance = _IrradianceMap.Sample(smp, waterN).rgb;
        float3 indirectDiffuse = kD * albedoWater * irradiance * waterDepth;

        // ---- 6) 合成: 地形 + 水中散乱 + 反射 ----
        // 浅瀬: 地形透過がメイン、深部: 反射と水色がメイン
        float3 refractedLight = lerp(c, terrainThroughWater, waterDepth);
        float3 scatteredLight = lerp(refractedLight, albedoWater, waterDepth * 0.7);
        float3 waterColor = scatteredLight + directSpec + indirectSpec + indirectDiffuse;

        // ---- 7) 岸辺のフォーム（白い泡）----
        // 岸辺 (浅瀬 depth 0.05-0.25) + 波が高い所にフォーム
        float shoreFoam = smoothstep(0.25, 0.05, riverDepthRaw) * smoothstep(0.3, 0.7, w1);
        float3 foamColor = float3(0.9, 0.95, 1.0);
        waterColor = lerp(waterColor, foamColor, shoreFoam * 0.7);

        // ---- 8) 最終合成 ----
        float waterEdge = smoothstep(0.05, 0.20, waterMask);
        c = lerp(c, waterColor, waterEdge);
    }

    // 影を最終出力に適用（shadowFactor=1.0 の領域は変化なし → 円アーティファクトなし）
    c *= (0.3 + 0.7 * shadowFactor);
    return float4(c, 1.0f);
}
