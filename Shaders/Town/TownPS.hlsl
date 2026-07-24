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
// gVsmFpLod: Phase 1 フットプリントLOD の ON/OFF（マーカー/デバッグとロックステップ）。OFF=従来の距離LOD。
// gUseDdgi: Phase G DDGI 拡散GI の ON/OFF（0 で従来の偽ambient経路＝バイト一致）。
// gUseRtShadow: レイトレース影 ON/OFF（1 で VSM/CSM の代わりに TLAS へシャドウレイ）。
cbuffer VsmFlag : register(b4, space0) { uint gUseVsm; uint gVsmFpLod; uint gUseDdgi; uint gUseRtShadow; uint gUseGlassRtr; };

// レイトレース太陽影: 町 TLAS へ inline RayQuery（PS内, SM6.6）。関数定義は SceneCB(SunDirection) 後方に。
RaytracingAccelerationStructure Scene_RT : register(t14, space0);

// ---- Phase G: DDGI 拡散GI（gUseDdgi=1 のとき偽ambient(sky-tint)を実プローブirradianceで置換）----
#include "../RT/Ddgi.hlsli"
StructuredBuffer<ProbeSH> Ddgi_Probes : register(t13, space0);
cbuffer DdgiCB : register(b5, space0)
{
    float3 gDdgiOrigin;  uint  gDdgiProbeCount;
    float3 gDdgiSpacing; uint  gDdgiFrameIndex;
    uint3  gDdgiDims;    float gDdgiNormalBias;
    float3 gDdgiSunDir;  float gDdgiEmaAlpha;
    float4 gDdgiSunColor;
    uint   gDdgiRayCount; float gDdgiIntensity; float2 _ddgiPad;
};

// 1タップ: 呼び出し側が選んだレベル L で完全再アドレッシング→アトラス深度比較。1=光,0=影,-1=未割当。
// L・bias はタップ毎に再計算せず引数受け取り（フットプリントLOD/スロープバイアスは画素単位=全タップ共通）。
float VsmShadowTap(float2 lxy, float lz, uint L, float bias)
{
    float2 uvp;
    uint2 vp = Vsm_VirtualPage(lxy, Vsm_LevelCenterExtent[L].xy, Vsm_LevelCenterExtent[L].z, (uint)Vsm_Params.z, uvp);
    uint idx = Vsm_PageTableIndex(L, vp, (uint)Vsm_Params.z);
    uint phys = Vsm_PageTable[idx];
    if (phys == 0xFFFFu) return -1.0f;
    float2 auv = Vsm_PhysicalUV(phys, uvp, (uint)Vsm_Params.w);
    float stored = Vsm_Atlas.SampleLevel(Vsm_Smp, auv, 0);
    float mine = Vsm_NormalizeDepth(lz, Vsm_ZParams.x, Vsm_ZParams.y);
    return (mine - bias <= stored) ? 1.0f : 0.0f;
}
// 8タップ ライト空間PCF（各タップ再アドレッシング=スパース安全）。1=光,0=影。
// 指定レベル L で 8-tap ライト空間PCF。戻り: 0..1（影率, 1=光）, 全タップ未割当なら -1。
// slope=受光面と光の成す角のtan（スロープスケールバイアス用）。
float VsmPcfLevel(float3 ls, uint L, float slope)
{
    float texelW = Vsm_LevelCenterExtent[L].w;
    float radius = texelW * 1.5f;   // texelWorld×1.5（ペナンブラ幅）
    // スロープスケール深度バイアス: 傾斜(grazing)ほど1texelあたり深度変化が大→バイアス増。
    // peter-panning(平面の浮き)と acne(傾斜の自己影)を同時回避。base 1.5 texel + slope 2.5 texel。
    float bias = (texelW * (1.5f + 2.5f * slope)) / max(Vsm_ZParams.y - Vsm_ZParams.x, 1e-3f) + 3e-6f;
    const int TAPS = 8;
    float sum = 0.0f, wsum = 0.0f;
    [unroll] for (int t = 0; t < TAPS; ++t)
    {
        float ang = (t + 0.5f) * (6.2831853f / TAPS);
        float r = sqrt((t + 0.5f) / TAPS) * radius;
        float s = VsmShadowTap(ls.xy + float2(cos(ang), sin(ang)) * r, ls.z, L, bias);
        if (s >= 0.0f) { sum += s; wsum += 1.0f; }
    }
    return (wsum < 0.5f) ? -1.0f : (sum / wsum);
}

float SampleSunShadowVSM(float3 worldPos, float ndotl)
{
    float3 ls = mul(float4(worldPos, 1.0f), Vsm_LightView).xyz;
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // V5b: .z=pageWorld → extent0=pw0*vppr
    // フットプリントLOD: worldPos の光空間XYのスクリーン導関数で1画素の光空間フットプリントを得る。
    // ★導関数は一様制御フローでのみ有効 → [unroll]ループ・分岐の前に関数入口で無条件に評価（安全）。
    float2 dLdx = ddx(ls.xy);
    float2 dLdy = ddy(ls.xy);
    uint levels = (uint)Vsm_Params.x;
    float tw0 = Vsm_LevelCenterExtent[0].w;
    // 連続LOD（footprint=log2, 整数化しない）と distance LOD の max。これの小数部で隣接レベルをブレンド。
    float f = max(max(abs(dLdx.x), abs(dLdx.y)), max(abs(dLdy.x), abs(dLdy.y)));
    float lodFp = (gVsmFpLod != 0u) ? log2(max(f / max(tw0, 1e-6f), 1.0f)) : 0.0f;
    // 距離LODを連続化。旧: (float)Vsm_SelectLevel は整数 ceil のため、遠方のカメラ正対面では
    // lod=lodDist が正確な整数 → frac=0 → トリリニアが無効化され、クリップマップ窓境界(2^Lc のリング)を
    // カメラ移動で跨ぐ瞬間にレベルが hard フリップ（解像度/ペナンブラが2倍段差）＝「遠影チカチカ」＆
    // 「移動するとカスケード境界がリング状にスウィープ」の主因。floor は被覆レベル Lc のまま（細レベルは
    // 遠点に届かないので粗方向 Lc+1 へのみブレンド）、窓外縁で C0 連続に Lc→Lc+1 へ遷移させ段差を溶かす。
    float dCheb = max(abs(ls.x - Vsm_ZParams.z), abs(ls.y - Vsm_ZParams.w)) * 2.0f;
    uint  Lc = Vsm_SelectLevel(ls.xy, Vsm_ZParams.zw, levels, base);   // ceil 被覆レベル
    float edge = saturate((dCheb / max(base, 1e-3f) / exp2((float)Lc) - 0.5f) * 2.0f);  // 0=窓中央, 1=窓外縁
    float lodDist = (float)Lc + edge;
    float lod = clamp(max(lodDist, lodFp), 0.0f, (float)(levels - 1u));
    uint  L0 = (uint)floor(lod);
    uint  L1 = min(L0 + 1u, levels - 1u);
    float w  = lod - (float)L0;   // ブレンド係数（0=L0, 1=L1）
    float slope = clamp(sqrt(max(1.0f - ndotl * ndotl, 0.0f)) / max(ndotl, 0.15f), 0.0f, 6.0f);
    // トリリニア: 隣接クリップマップレベルをブレンドし、レベル境界の解像度/ペナンブラ段差（移動時に
    // 同心リングとしてスウィープして目立つ「カスケード境界」）を滑らかにする。±1帯が両レベルをマーク済。
    float s0 = VsmPcfLevel(ls, L0, slope);
    float s1 = (w > 0.01f && L1 != L0) ? VsmPcfLevel(ls, L1, slope) : s0;
    if (s0 < 0.0f && s1 < 0.0f) return -1.0f;   // 両レベル未割当 → CSMフォールバック
    if (s0 < 0.0f) return s1;                    // 片方のみ割当 → そちらを使用（境界の穴を防ぐ）
    if (s1 < 0.0f) return s0;
    return lerp(s0, s1, w);
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
// レイトレース太陽影（太陽ディスクへ4レイ＝ソフト半影）。TLAS 除外のフォリッジ/木は落とさない点に注意。
float RtSunShadow(float3 worldPos, float3 Nw, float2 svpos)
{
    float3 L = normalize(SunDirection.xyz);            // 光へ向かう方向
    if (dot(Nw, L) <= 0.0f) return 0.0f;               // 太陽の裏面＝影
    float3 O = worldPos + Nw * 0.05f;                  // 法線バイアス（アクネ回避）
    float3 up = abs(L.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 T = normalize(cross(up, L));
    float3 B = cross(L, T);
    const float coneR = 0.02f;                         // 太陽角半径相当（ソフトさ）
    float rot = frac(sin(dot(svpos, float2(12.9898f, 78.233f))) * 43758.5453f) * 6.2831853f;
    const int N = 4;
    float vis = 0.0f;
    [unroll] for (int i = 0; i < N; ++i)
    {
        float a = rot + i * (6.2831853f / (float)N);
        float2 off = float2(cos(a), sin(a)) * coneR * (0.5f + 0.5f * frac(a * 1.37f));
        float3 dir = normalize(L + T * off.x + B * off.y);
        RayDesc r; r.Origin = O; r.Direction = dir; r.TMin = 0.02f; r.TMax = 100000.0f;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(Scene_RT, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFFu, r);
        q.Proceed();
        vis += (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0f : 1.0f;
    }
    return vis / (float)N;
}

// レイトレース ガラス反射: 反射方向へ TLAS を叩き、ヒットは DDGI irradiance(色付き=太陽+空+バウンス済)を
// 近似反射色に、miss は prefilter 空。GeometryInfo 不要（既存の Scene_RT t14 + DDGI t13/b5 を再利用）。
float3 RtGlassRefl(float3 worldPos, float3 R, float3 skyFallback)
{
    RayDesc ray; ray.Origin = worldPos + R * 0.05f; ray.Direction = R; ray.TMin = 0.02f; ray.TMax = 100000.0f;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(Scene_RT, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFFu, ray);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        float3 hitPos = worldPos + R * q.CommittedRayT();
        if (gUseDdgi != 0)
            return Ddgi_SampleField(Ddgi_Probes, hitPos, -R, gDdgiOrigin, gDdgiSpacing, gDdgiDims) * gDdgiIntensity * 0.5f;
        return skyFallback * 0.4f;   // DDGI無し: 近似の暗めフォールバック
    }
    return g_Prefilter.SampleLevel(g_Sampler, R, 0).rgb;   // 空（鏡面）
}

float SampleSunShadowHybrid(float3 worldPos, float3 Nw, float2 svpos)
{
    if (gUseRtShadow != 0u)
        return RtSunShadow(worldPos, Nw, svpos);   // レイトレース影（VSM/CSM を置換）
    if (gUseVsm != 0u)
    {
        // スロープスケールバイアス用に受光面と太陽の角度余弦を渡す（SunDirection=光へ向かう方向, 正規化）。
        float ndotl = saturate(dot(Nw, normalize(SunDirection.xyz)));
        float s = SampleSunShadowVSM(worldPos, ndotl);
        // VSM主軸: 被覆画素は VSM の影。非被覆(>512m 等ごく僅か)は lit 扱い。CSMカスケードは VSM ON時
        // キャスタ描画をスキップ(空)なので参照しない＝二重計算を排除。VSMは0-512mを世界固定で covers。
        return (s >= 0.0f) ? s : 1.0f;
    }
    return SampleSunShadowSoft(worldPos, Nw, svpos);   // VSM OFF → 従来CSM（キャスタ描画あり）
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
        if (gUseGlassRtr != 0u)
            envRefl = RtGlassRefl(In.worldPos, Rg, skyRefl);   // RT反射（画面外の街も映る）

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
    // Phase G: DDGI 有効時は拡散の入射irradianceを実プローブ場で置換（空間変化する本物のGI）。
    //          gUseDdgi=0 では irr のまま＝従来の偽ambientとバイト一致。
    if (gUseDdgi != 0)
        irr = Ddgi_SampleField(Ddgi_Probes, In.worldPos + N * gDdgiNormalBias, Nw,
                               gDdgiOrigin, gDdgiSpacing, gDdgiDims) * gDdgiIntensity;
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
    if (gUseDdgi != 0) ambientBoost = 1.0f;   // DDGI は物理的な入射光なので人工ブーストを外す
    float3 ambient = (diffuseIBL * iblDiffuse * ambientBoost + specularIBL * iblReflect) * ao;

    float3 color = ambient + Lo;
    outColor = float4(max(color, 0.0f), 1.0f);   // 線形 HDR ( ACES/ガンマは PostProcess )
}
