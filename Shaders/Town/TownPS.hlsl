// ============================================================
//  TownPS.hlsl — Unreal T3D 町シーン用 フル PBR (D3D12/SM6.6)
//  SP31 TownPS からの移植。2点だけ DX12 に合わせて改変:
//   (1) IBL を equirect HDR ではなく DX12 の cubemap (irradiance/
//       prefilter/brdfLut) から取得。
//   (2) 影を DX12 ShadowSystem の CSM (Texture2DArray t0,space2 +
//       ShadowCB b1,space2) から取得。
//  ・ACES/ガンマ末尾は削除 → 線形 HDR 出力 (PostProcess が露出/
//    トーンマップ/ガンマを行うため二重適用を回避)。
//  ・直接光: 太陽 Cook-Torrance / 点光源(街灯 b9) / POM / ガラス。
// ============================================================

Texture2D    g_Base   : register(t0, space0);
Texture2D    g_Normal : register(t1, space0);
Texture2D    g_MR     : register(t2, space0);   // G=roughness, B=metallic
Texture2D    g_AO     : register(t3, space0);
Texture2D    g_Height : register(t4, space0);   // POM 用ハイトマップ
TextureCube  g_Prefilter  : register(t6, space0);
TextureCube  g_Irradiance : register(t7, space0);
Texture2D    g_BrdfLut    : register(t8, space0);
// C1 ガラスSSR: t9=シーン深度(R32_FLOAT), t10=不透明シーンカラーのコピー。param10 テーブルで
// ガラスパスのみバインド（不透明パスは分岐に入らず未参照）。
Texture2D<float> g_SceneDepth     : register(t9,  space0);
Texture2D        g_SceneColorCopy : register(t10, space0);
SamplerState g_Sampler    : register(s0, space0);

// ---- DX12 CSM 影 (space2) ----
Texture2DArray        g_Csm         : register(t0, space2);
SamplerComparisonState g_ShadowSmp  : register(s1, space2);
cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[4];      // 4 カスケード
    float4 CascadeSplits;   // view 空間 far (cascade 0/1/2/3)
    float4 CascadeTexelWorld; // world m/テクセル (cascade0..3)。BUG3: ペナンブラ幅統一用
};

// ---- V4: VSM 太陽シャドウ（gUseVsm=1 のとき CSM の代わりにサンプル）----
#include "../Vsm/Vsm.hlsli"   // 純関数アドレッシング（Vsm_SelectLevel/VirtualPage/PageTableIndex/PhysicalUV/NormalizeDepth）
StructuredBuffer<uint> Vsm_PageTable : register(t11, space0);
Texture2D<float>       Vsm_Atlas     : register(t12, space0);
SamplerState           Vsm_Smp       : register(s2,  space0);
cbuffer VsmCB : register(b3, space0)
{
    matrix Vsm_LightView;
    matrix Vsm_InvViewProj;
    float4 Vsm_Params;              // x=levelCount, y=pageSize, z=vppr, w=appr
    float4 Vsm_ZParams;             // x=zNear, y=zFar, z=camLightX, w=camLightY
    float4 Vsm_DepthDim;
    float4 Vsm_LevelCenterExtent[8];
};
cbuffer VsmFlag : register(b4, space0) { uint gUseVsm; uint3 _vsmPad; };

// 1タップ: ライト空間XYを完全再アドレッシング→アトラス深度比較。1=光,0=影,-1=未割当。
float VsmShadowTap(float2 lxy, float lz)
{
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // V5b: .z=pageWorld → extent0=pw0*vppr
    uint L = Vsm_SelectLevel(lxy, Vsm_ZParams.zw, (uint)Vsm_Params.x, base);
    float2 uvp;
    uint2 vp = Vsm_VirtualPage(lxy, Vsm_LevelCenterExtent[L].xy, Vsm_LevelCenterExtent[L].z, (uint)Vsm_Params.z, uvp);
    uint idx = Vsm_PageTableIndex(L, vp, (uint)Vsm_Params.z);
    uint phys = Vsm_PageTable[idx];
    if (phys == 0xFFFFu) return -1.0f;
    float2 auv = Vsm_PhysicalUV(phys, uvp, (uint)Vsm_Params.w);
    float stored = Vsm_Atlas.SampleLevel(Vsm_Smp, auv, 0);
    float mine = Vsm_NormalizeDepth(lz, Vsm_ZParams.x, Vsm_ZParams.y);
    return (mine - 0.004f <= stored) ? 1.0f : 0.0f;
}
// 8タップ ライト空間PCF（各タップ再アドレッシング=スパース安全）。1=光,0=影。
float SampleSunShadowVSM(float3 worldPos)
{
    float3 ls = mul(float4(worldPos, 1.0f), Vsm_LightView).xyz;
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // V5b: .z=pageWorld → extent0=pw0*vppr
    uint L = Vsm_SelectLevel(ls.xy, Vsm_ZParams.zw, (uint)Vsm_Params.x, base);
    float radius = Vsm_LevelCenterExtent[L].w * 1.5f;   // texelWorld×1.5
    const int TAPS = 8;
    float sum = 0.0f, wsum = 0.0f;
    [unroll] for (int t = 0; t < TAPS; ++t)
    {
        float ang = (t + 0.5f) * (6.2831853f / TAPS);
        float r = sqrt((t + 0.5f) / TAPS) * radius;
        float s = VsmShadowTap(ls.xy + float2(cos(ang), sin(ang)) * r, ls.z);
        if (s >= 0.0f) { sum += s; wsum += 1.0f; }
    }
    return (wsum < 0.5f) ? -1.0f : (sum / wsum);   // 全未割当 → -1（VSM非カバー＝CSMへフォールバック）
}

cbuffer SceneCB : register(b1, space0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;   // .xyz world pos
    float4 SunDirection;  // .xyz 光へ向かう方向, .w 強度
    float4 SunColor;      // .rgb
    matrix InvViewProj;
};

// 町の調整パラメータ
cbuffer TownParams : register(b2, space0)
{
    float4 Params;   // x=glass flag, y=iblReflect, z=normalStrength, w=normalFlipG
    float4 Params2;  // x=iblDiffuse, y=heightScale(POM), z=ambientBoost, w=sunScale
};

// ---- 街灯の点光源 ----
#define MAX_TOWN_LIGHTS 64
cbuffer TownLightBuffer : register(b9, space0)
{
    float4 TL_Count;                       // x = 有効光源数
    float4 TL_PosRadius[MAX_TOWN_LIGHTS];   // xyz=位置, w=半径
    float4 TL_Color[MAX_TOWN_LIGHTS];       // rgb=色×強度
};

static const float PI = 3.14159265359f;
static const float PREFILTER_MIP_COUNT = 5.0f;
static const float kShadowBias = 0.001f;

struct PS_IN
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD1;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float d = max(dot(T, T), dot(B, B));
    float invmax = rsqrt(max(d, 1e-20f));
    return float3x3(T * invmax, B * invmax, N);
}

float3 SampleNormalTS(float2 uv, float2 dx, float2 dy)
{
    float3 n = g_Normal.SampleGrad(g_Sampler, uv, dx, dy).xyz * 2.0f - 1.0f;
    if (Params.w > 0.5f) n.y = -n.y;   // OpenGL <-> DirectX
    n.xy *= Params.z;                  // 法線の強さ
    return n;
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0f);
    float d = (NdotH * NdotH * (a2 - 1.0f) + 1.0f);
    return a2 / (PI * d * d + 1e-5f);
}
float GeometrySchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotX / (NdotX * (1.0f - k) + k);
}
float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, V), 0.0f), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0f), roughness);
}
float3 FresnelSchlick(float cosT, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosT), 5.0f);
}

// POM: 接空間視線に沿ってハイトを追跡し UV をずらす
float2 ParallaxOcclusion(float2 uv, float3 viewTS, float heightScale)
{
    heightScale *= saturate((viewTS.z - 0.15f) * 3.0f);
    if (heightScale <= 0.0001f) return uv;
    const float numLayers = 32.0f;
    float layerDepth = 1.0f / numLayers;
    float2 P = (viewTS.xy / max(viewTS.z, 0.2f)) * heightScale;
    float2 deltaUV = P / numLayers;
    float curDepth = 0.0f;
    float2 curUV = uv;
    float curH = 1.0f - g_Height.SampleLevel(g_Sampler, curUV, 0).r;
    [loop] for (int i = 0; i < 32; i++)
    {
        if (curDepth >= curH) break;
        curUV -= deltaUV;
        curH = 1.0f - g_Height.SampleLevel(g_Sampler, curUV, 0).r;
        curDepth += layerDepth;
    }
    float2 prevUV = curUV + deltaUV;
    float afterD = curH - curDepth;
    float beforeD = (1.0f - g_Height.SampleLevel(g_Sampler, prevUV, 0).r) - curDepth + layerDepth;
    float w = afterD / (afterD - beforeD);
    return lerp(curUV, prevUV, saturate(w));
}

// DX12 CSM 影: 高解像度(2048)CSM + 16-tap Vogel ソフト PCF（TownShadow.hlsli）。
// g_Csm / g_ShadowSmp / ShadowCB(LightVP,CascadeSplits) / SceneCB(View) は上で宣言済み。
#include "TownShadow.hlsli"

// ハイブリッド太陽影: VSM がカバーする画素は VSM（近~中の高精細・世界固定）、未割当画素（遠景/プール溢れ）は
// CSM（安価・堅牢なフォールバック）。VSM OFF 時は常に CSM。→ VSM を ON にしても遠景で影が抜けない。
float SampleSunShadowHybrid(float3 worldPos, float3 Nw, float2 svpos)
{
    if (gUseVsm != 0u)
    {
        float s = SampleSunShadowVSM(worldPos);
        if (s >= 0.0f) return s;   // VSM がこの画素をカバー
    }
    return SampleSunShadowSoft(worldPos, Nw, svpos);   // 非VSM or VSM未割当 → CSM
}

// 点光源 1 灯の寄与
float3 PointLightContrib(float3 Nw, float3 V, float3 worldPos,
    float3 albedo, float roughness, float metallic, float3 F0,
    float3 lightPos, float radius, float3 lightColor)
{
    float3 toL = lightPos - worldPos;
    float dist = length(toL);
    if (dist >= radius) return float3(0, 0, 0);
    float3 L = toL / max(dist, 1e-4f);
    float3 H = normalize(V + L);
    float t = saturate(1.0f - dist / radius);
    float3 radiance = lightColor * (t * t);
    float  NDF = DistributionGGX(Nw, H, roughness);
    float  G = GeometrySmith(Nw, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);
    float3 spec = (NDF * G * F) / (4.0f * max(dot(Nw, V), 0.0f) * max(dot(Nw, L), 0.0f) + 1e-4f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float  NdotL = max(dot(Nw, L), 0.0f);
    return (kD * albedo / PI + spec) * radiance * NdotL;
}

// C1: 世界空間レイマーチによるスクリーン空間反射。不透明シーン（コピー）から反射色を拾う。
// P=反射面の世界座標, R=反射方向(world)。ヒットで rgb を返し weight(0..1) を出力、miss は weight=0。
// 深度は標準(近=0/遠=1, クリア=1.0)。ndc.z が格納深度を越えた瞬間＝ジオメトリ背後へ到達＝ヒット。
float3 ScreenSpaceReflect(float3 P, float3 R, out float weight)
{
    weight = 0.0f;
    float4x4 VP = mul(View, Proj);
    float t = 0.25f;            // 開始距離(m)：自己交差回避
    float prevDiff = -1.0f;     // 直前サンプルの (ray.z - scene.z)。負=ジオメトリ手前
    [loop] for (int i = 0; i < 28; ++i)
    {
        P += R * t;
        t *= 1.27f;             // 幾何級数：近くは細かく、遠くは粗く
        float4 clip = mul(float4(P, 1.0f), VP);
        if (clip.w <= 0.0f) return float3(0, 0, 0);         // カメラ背後
        float3 ndc = clip.xyz / clip.w;
        if (any(abs(ndc.xy) > 1.0f)) return float3(0, 0, 0); // 画面外＝miss
        float2 suv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
        float sceneD = g_SceneDepth.SampleLevel(g_Sampler, suv, 0);
        if (sceneD >= 1.0f) { prevDiff = -1.0f; continue; }  // 空：ジオメトリ無し
        float diff = ndc.z - sceneD;
        if (diff > 0.0f && prevDiff <= 0.0f)                 // 手前→背後へ交差＝表面ヒット
        {
            float2 e = abs(suv * 2.0f - 1.0f);
            weight = saturate(1.0f - pow(max(e.x, e.y), 6.0f));  // 画面端フェード
            return g_SceneColorCopy.SampleLevel(g_Sampler, suv, 0).rgb;
        }
        prevDiff = diff;
    }
    return float3(0, 0, 0);
}

void main(in PS_IN In, out float4 outColor : SV_Target)
{
    float3 N = normalize(In.normal);
    float2 uv = In.uv;
    float2 dUVx = ddx(uv);
    float2 dUVy = ddy(uv);

    float3 nTS = SampleNormalTS(uv, dUVx, dUVy);
    float3x3 TBN = CotangentFrame(N, In.worldPos, uv);
    float3 Nw = normalize(mul(nTS, TBN));

    float3 baseCol = g_Base.SampleGrad(g_Sampler, uv, dUVx, dUVy).rgb;
    float3 L = normalize(SunDirection.xyz);
    float3 V = normalize(CameraWorld.xyz - In.worldPos);

    float iblReflect = Params.y;
    float iblDiffuse = Params2.x;

    // ---- ガラス ----
    if (Params.x > 0.5f)
    {
        // C1: ガラスは DSV=null で描画するため深度テストを手動化（不透明より奥は棄却）。
        float sceneDhere = g_SceneDepth.Load(int3((int2)In.svpos.xy, 0));
        if (In.svpos.z > sceneDhere + 1e-6f) discard;

        float3 Rg = reflect(-V, Nw);
        float fres = pow(1.0f - saturate(dot(Nw, V)), 4.0f);
        float3 H = normalize(V + L);
        float spec = pow(saturate(dot(Nw, H)), 400.0f);
        float3 skyRefl = g_Prefilter.SampleLevel(g_Sampler, Rg, 1.5f).rgb;

        // C1: 実際の街をスクリーン空間反射で取得。ヒットしない画素は skyRefl にフォールバック。
        float ssrW = 0.0f;
        float3 ssr = ScreenSpaceReflect(In.worldPos, Rg, ssrW);
        float3 envRefl = lerp(skyRefl, ssr, ssrW);

        // ガラスも太陽の影を受ける（従来は影を参照せず、屋根の影の下でもガラスに太陽ハイライトが
        // 出ていた＝「太陽が2つある」ように見える光漏れ。太陽由来の項を shadow で減衰）。
        float gsh = SampleSunShadowHybrid(In.worldPos, Nw, In.svpos.xy);   // VSM→未割当はCSMフォールバック
        float3 tint = baseCol * 0.5f + float3(0.30f, 0.36f, 0.42f) * 0.5f;
        float3 col = tint * (SunColor.rgb * 0.3f * gsh)   // 太陽由来のベース明るさ→影で減衰
            + envRefl * (0.2f + fres * 0.8f)              // 環境反射(空/街)は影と無関係
            + spec * 4.0f * gsh;                          // 太陽スペキュラ→影で消える
        float alpha = saturate(0.12f + fres * 0.8f + spec * gsh);
        outColor = float4(col, alpha);
        return;
    }

    // ---- POM ----
    float3 viewTS = mul(TBN, V);
    uv = ParallaxOcclusion(uv, viewTS, Params2.y);
    nTS = SampleNormalTS(uv, dUVx, dUVy);
    Nw = normalize(mul(nTS, TBN));
    float4 baseTex = g_Base.SampleGrad(g_Sampler, uv, dUVx, dUVy);
    baseCol = baseTex.rgb;

    clip(baseTex.a - 0.02f);   // 完全透明のみ破棄 ( 葉/看板の背景 )

    float3 albedo = pow(baseCol, 2.2f);
    // MR スロットには単体グレースケール rough（G=roughness）または結合 ORM（G=rough）が入る。
    // 町はほぼ非金属なので metallic は 0 固定（grayscale rough を B から読むと誤って金属化するため）。
    // 町は漆喰/レンガ/塗装でほぼマット。floor を 0.04 にすると低ラフネス画素で GGX が爆発し
    // (~124000x)、点光源/太陽のスペキュラが極小の超高輝度ディスク→ブルームで巨大な白い塊に。
    // マットな町向けに floor を 0.30 に引き上げ、鏡面スパイクを防ぐ。
    float roughness = clamp(g_MR.SampleGrad(g_Sampler, uv, dUVx, dUVy).g, 0.30f, 1.0f);
    float metallic = 0.0f;
    float ao = g_AO.SampleGrad(g_Sampler, uv, dUVx, dUVy).r;

    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float NdotV = max(dot(Nw, V), 1e-4f);

    // ---- 直接光 ( 太陽 ) ----
    float3 H = normalize(V + L);
    float3 radiance = SunColor.rgb * max(SunDirection.w, 1.0f) * Params2.w;
    float  NDF = DistributionGGX(Nw, H, roughness);
    float  G = GeometrySmith(Nw, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0f), F0);
    float3 specular = (NDF * G * F) / (4.0f * NdotV * max(dot(Nw, L), 0.0f) + 1e-4f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float  NdotL = max(dot(Nw, L), 0.0f);
    float3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;
    Lo *= SampleSunShadowHybrid(In.worldPos, Nw, In.svpos.xy);   // VSM→未割当はCSMフォールバック

    // ---- 点光源 ( 街灯 ) ----
    int lcount = (int)TL_Count.x;
    [loop] for (int li = 0; li < lcount; li++)
    {
        Lo += PointLightContrib(Nw, V, In.worldPos,
            albedo, roughness, metallic, F0,
            TL_PosRadius[li].xyz, TL_PosRadius[li].w, TL_Color[li].rgb);
    }

    // ---- 間接光 ( cubemap IBL ) ----
    float3 irr = g_Irradiance.Sample(g_Sampler, Nw).rgb;
    float3 Famb = F0 + (max((1.0f - roughness).xxx, F0) - F0) * pow(1.0f - NdotV, 5.0f);
    float3 kDamb = (1.0f - Famb) * (1.0f - metallic);
    float3 diffuseIBL = irr * albedo * kDamb;

    float3 R = reflect(-V, Nw);
    float mip = roughness * (PREFILTER_MIP_COUNT - 1.0f);
    float3 prefiltered = g_Prefilter.SampleLevel(g_Sampler, R, mip).rgb;
    float2 envBRDF = g_BrdfLut.Sample(g_Sampler, float2(NdotV, roughness)).rg;
    float3 specularIBL = prefiltered * (F0 * envBRDF.x + envBRDF.y);

    // 暫定 GI: 拡散アンビエントを持ち上げ（Lumen の多重バウンス fill を安価に近似）。
    // 日向との差を保つため specular IBL は素のまま。AO で接地/くぼみを締める。
    float ambientBoost = max(Params2.z, 1.0f);
    float3 ambient = (diffuseIBL * iblDiffuse * ambientBoost + specularIBL * iblReflect) * ao;

    float3 color = ambient + Lo;
    outColor = float4(max(color, 0.0f), 1.0f);   // 線形 HDR ( ACES/ガンマは PostProcess )
}
