// ============================================================
// Water Pixel Shader (Terrain Root Signature 対応版)
// 川マスクで透明度制御、IBL 反射、Beer's Law 疑似深度
// ============================================================

struct VSOutput
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 normal   : NORMAL;
};

// terrain ルートシグに合わせる
cbuffer Transform : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;
};

cbuffer TerrainParams : register(b1)
{
    float4 LayerColor[6];
    float4 CameraPos;    // .w = time
    float4 DebugParams;
    float4 SunDirection;
    float4 SunColor;
};

SamplerState smp : register(s0);

// テレインと同じスロット (t0-t8)
Texture2D _TreeMask    : register(t0);
Texture2D _NatureMask  : register(t1);
Texture2D _GroundDiff  : register(t2);
Texture2D _GroundDisp  : register(t3);  // 水面法線として使用
Texture2D _RiversMask  : register(t4);
Texture2D _SnowMask    : register(t5);
TextureCube _PrefilterEnv  : register(t6);
TextureCube _IrradianceMap : register(t7);
Texture2D _BrdfLut         : register(t8);

// 拡張テレインテクスチャ (t9-t12)
Texture2D _RiversDirection : register(t9);   // RG: 川の流れ方向 (B=128 中立)
Texture2D _WaterColorTint  : register(t10);  // RGB: Gaea が出した本物の水色
Texture2D _FreshWaterMask  : register(t11);  // 湿地マスク
Texture2D _InhibitorsMask  : register(t12);  // 植生抑制マスク (未使用、地形側で参照)

// Shadow (space2)
Texture2DArray _ShadowMap : register(t0, space2);
SamplerComparisonState shadowSampler : register(s1, space2);
cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[4];
    float4 CascadeSplits;
};

static const float kShadowBias = 0.002;

// 川マスクの 5x5 ガウスぼかし (TerrainVS と同じカーネル)
float SmoothRiverMask(float2 uv)
{
    const float t = 1.0 / 512.0;
    float v = 0.0;
    v += _RiversMask.Sample(smp, uv).r * 4.0;
    v += _RiversMask.Sample(smp, uv + float2( t,  0)).r * 2.0;
    v += _RiversMask.Sample(smp, uv + float2(-t,  0)).r * 2.0;
    v += _RiversMask.Sample(smp, uv + float2( 0,  t)).r * 2.0;
    v += _RiversMask.Sample(smp, uv + float2( 0, -t)).r * 2.0;
    v += _RiversMask.Sample(smp, uv + float2( t,  t)).r;
    v += _RiversMask.Sample(smp, uv + float2(-t,  t)).r;
    v += _RiversMask.Sample(smp, uv + float2( t, -t)).r;
    v += _RiversMask.Sample(smp, uv + float2(-t, -t)).r;
    v += _RiversMask.Sample(smp, uv + float2( 2*t, 0)).r;
    v += _RiversMask.Sample(smp, uv + float2(-2*t, 0)).r;
    v += _RiversMask.Sample(smp, uv + float2( 0,  2*t)).r;
    v += _RiversMask.Sample(smp, uv + float2( 0, -2*t)).r;
    return v / 20.0;
}

float SampleShadowPCF(float3 worldPos, float viewDepth)
{
    if (viewDepth >= CascadeSplits.x)
        return 1.0;
    uint cascade = 0;
    float4 lc = mul(float4(worldPos, 1.0), LightVP[cascade]);
    float3 pc = lc.xyz / lc.w;
    float2 suv = pc.xy * 0.5 + 0.5;
    suv.y = 1.0 - suv.y;
    if (suv.x < 0 || suv.x > 1 || suv.y < 0 || suv.y > 1) return 1.0;
    float cmp = pc.z - kShadowBias;
    float ts = 1.0 / 512.0;
    float shadow = 0;
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(suv + float2(-0.5,-0.5)*ts, (float)cascade), cmp);
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(suv + float2( 0.5,-0.5)*ts, (float)cascade), cmp);
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(suv + float2(-0.5, 0.5)*ts, (float)cascade), cmp);
    shadow += _ShadowMap.SampleCmpLevelZero(shadowSampler, float3(suv + float2( 0.5, 0.5)*ts, (float)cascade), cmp);
    return shadow * 0.25;
}

float4 main(VSOutput input) : SV_TARGET
{
    // *** デバッグ: Water (川) パスは純赤 ***
    return float4(1.0, 0.0, 0.0, 1.0);

    // ---- 1) 川マスクをぼかして判定 (TerrainVS と整合) ----
    float riverSmooth = SmoothRiverMask(input.uv);
    if (riverSmooth < 0.20)
        discard;
    // 水深 0..1 正規化 (岸辺=0, 中心=1)
    float riverDepth = riverSmooth;
    float waterDepth01 = saturate((riverSmooth - 0.20) / 0.80);

    // ---- 2) 時間アニメーション ----
    float time = CameraPos.w;
    float wrappedTime = fmod(time, 500.0);

    // ---- 3) 川の流れ方向: Rivers_Direction.png (Gaea生成) を直接使用 ----
    // RG チャンネルが [0,1] にエンコードされた 2D 流れベクトル (0.5,0.5 = 静止)
    float2 dirRG = _RiversDirection.Sample(smp, input.uv).rg;
    float2 dirVec = dirRG * 2.0 - 1.0; // [-1,1] にデコード
    // ベクトルが極小なら勾配ベースのフォールバック
    float2 flowDir;
    if (length(dirVec) > 0.05)
    {
        flowDir = normalize(dirVec);
    }
    else
    {
        const float texelUV = 1.0 / 512.0;
        float mR = _RiversMask.Sample(smp, input.uv + float2( texelUV, 0)).r;
        float mL = _RiversMask.Sample(smp, input.uv + float2(-texelUV, 0)).r;
        float mU = _RiversMask.Sample(smp, input.uv + float2(0,  texelUV)).r;
        float mD = _RiversMask.Sample(smp, input.uv + float2(0, -texelUV)).r;
        float2 grad = float2(mR - mL, mU - mD);
        flowDir = (length(grad) > 1e-4) ? normalize(float2(-grad.y, grad.x)) : float2(1, 0);
    }

    // ---- 4) 手続き的なリップル法線 (岩 disp は使わない、クリーンな水面) ----
    // 流れ方向に沿ってスクロールする 3 つの正弦波を重ね、自然な小さなさざ波を作る
    float2 wp = input.worldPos.xz;
    float t1 = wrappedTime * 0.50;
    float t2 = wrappedTime * 0.85;
    float t3 = wrappedTime * 1.30;
    float2 d1 = flowDir;
    float2 d2 = float2(-flowDir.y, flowDir.x); // 流れ垂直
    float2 d3 = normalize(flowDir + float2(-flowDir.y, flowDir.x) * 0.5);
    float wave1 = sin(dot(wp, d1) * 0.55 + t1);
    float wave2 = sin(dot(wp, d2) * 0.90 + t2) * 0.60;
    float wave3 = sin(dot(wp, d3) * 1.40 + t3) * 0.30;
    // 法線勾配 (波の傾き) - 控えめに
    float2 waveGrad = float2(
        cos(dot(wp, d1) * 0.55 + t1) * 0.55 * d1.x +
        cos(dot(wp, d2) * 0.90 + t2) * 0.90 * 0.60 * d2.x,
        cos(dot(wp, d1) * 0.55 + t1) * 0.55 * d1.y +
        cos(dot(wp, d2) * 0.90 + t2) * 0.90 * 0.60 * d2.y) * 0.05;
    float3 waterNormal = normalize(float3(waveGrad.x, 1.0, waveGrad.y));
    // 後段のフォーム判定で使う「波の高さ」サロゲート
    float wA = (wave1 + wave2 + wave3) * 0.5 + 0.5; // 0..1 化

    // ---- 5) PBR 反射計算 ----
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float NdotV = saturate(dot(waterNormal, V));

    // Fresnel (Schlick, F0 = 0.02 for water)
    const float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    // IBL reflection
    float3 R = reflect(-V, waterNormal);
    float3 reflection = _PrefilterEnv.SampleLevel(smp, R, 0.5).rgb;

    // ---- 6) クリアな水深色: 浅瀬→深部のハッキリしたグラデーション ----
    // 浅瀬: 透明感のある淡いエメラルド (川底の色がうっすら混ざる)
    // 深部: 飽和した深いシアン青 (光が届かない)
    float3 shallowColor = float3(0.55, 0.82, 0.78);   // 浅瀬: 明るいエメラルド
    float3 midColor     = float3(0.20, 0.55, 0.65);   // 中間: ターコイズ
    float3 deepColor    = float3(0.04, 0.16, 0.28);   // 深部: 紺
    // 滑らかな 3 段グラデーション
    float3 waterBase;
    if (waterDepth01 < 0.5)
        waterBase = lerp(shallowColor, midColor, waterDepth01 * 2.0);
    else
        waterBase = lerp(midColor, deepColor, (waterDepth01 - 0.5) * 2.0);
    // Gaea が生成した水色を控えめにブレンド (主役は手続き深度色)
    float3 gaeaWaterColor = saturate(_WaterColorTint.Sample(smp, input.uv).rgb * 4.0);
    waterBase = lerp(waterBase, gaeaWaterColor, 0.20);

    // ---- 7) 合成 ----
    float3 waterColor = lerp(waterBase, reflection, fresnel);

    // ---- 8) 太陽スペキュラ ----
    float3 L = normalize(SunDirection.xyz);
    float3 H = normalize(L + V);
    float NdotH = saturate(dot(waterNormal, H));
    float sunSpec = pow(NdotH, 128.0) * fresnel * 2.0;

    float4 viewPos = mul(float4(input.worldPos, 1.0), View);
    float shadowFactor = SampleShadowPCF(input.worldPos, viewPos.z);
    waterColor += SunColor.rgb * sunSpec * shadowFactor;

    // ---- 9) 岸辺フォーム (riverSmooth 0.20-0.40 の浅瀬で強調) - 控えめに ----
    float foam = smoothstep(0.40, 0.20, riverSmooth);
    float foamWave = smoothstep(0.45, 0.75, wA);
    waterColor = lerp(waterColor, float3(0.92, 0.96, 1.0), foam * foamWave * 0.35);

    // ---- 10) 半透明度（ほぼ不透明）----
    // 浅瀬=85%、深部=97% に揃え、奥の木が透けて見えないように
    float alpha = lerp(0.85, 0.97, waterDepth01);

    return float4(waterColor * alpha, alpha); // プリマルチプライドアルファ
}
