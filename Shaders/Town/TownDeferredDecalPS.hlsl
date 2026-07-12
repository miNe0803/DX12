// ============================================================
//  TownDeferredDecalPS.hlsl — スクリーン空間デファード(ボックス)デカール。
//  シーン深度からワールド座標を復元→デカールボックス局所へ変換→範囲外破棄→
//  ボックス面 UV でアルベドをサンプル→路面と同じライティング(太陽*影 + IBL)。
//  法線フェードで斜め/裏面へのにじみを防ぐ。出力は事前乗算アルファ。
//  UE の DBuffer デカール相当：曲面(Corner Cafe の awning 等)にも巻き付く。
// ============================================================

Texture2D        g_Base       : register(t0, space0);   // デカール BaseColor (RGBA, a=マスク)
TextureCube      g_Irradiance : register(t7, space0);   // 拡散 IBL（TownDecalPS と同じ）
Texture2D<float> g_Depth      : register(t9, space0);   // シーン深度 SRV (R32_FLOAT)
SamplerState     g_Sampler    : register(s0, space0);

// ---- DX12 CSM 影 (space2) — TownDecalPS と同一 ----
Texture2DArray         g_Csm       : register(t0, space2);
SamplerComparisonState g_ShadowSmp : register(s1, space2);
cbuffer ShadowCB : register(b1, space2)
{
    matrix LightVP[4];
    float4 CascadeSplits;
    float4 CascadeTexelWorld; // BUG3: ペナンブラ幅統一 (TownShadow.hlsli)
};

cbuffer SceneCB : register(b1, space0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
    float4 SunDirection;   // .xyz 光へ向かう方向, .w 強度
    float4 SunColor;       // .rgb
    matrix InvViewProj;    // = transpose(inverse(view*proj))
};

cbuffer DecalParams : register(b2, space0)
{
    float4 DTint;        // rgb 明るさ/色, a 不透明度
    float4 DScreenInv;   // .xy = (1/rtWidth, 1/rtHeight)
};

// per-decal PS データ ( gBaseInstance で参照, g_Worlds と並行 )
struct DecalGpu
{
    float4x4 worldToBox;    // = transpose(inverse(BW)); world → box 局所 [-1,1]^3
    float4   projAxisWorld; // .xyz = ボックス局所 +X 軸(=投影/法線軸) の world 方向
};
StructuredBuffer<DecalGpu> g_Decals : register(t1, space1);

cbuffer InstBase : register(b0, space1)
{
    uint  gBaseInstance;   // = デカール index
    uint3 _instPad;
};

static const float PI = 3.14159265359f;
static const float kShadowBias = 0.001f;
static const float kSunScale   = 3.0f;   // TownParams.Params2.w / TownDecalPS と一致

// 高解像度 CSM + 16-tap Vogel ソフト PCF（TownPS/TownDecalPS と共有）。全カスケード対応。
#include "TownShadow.hlsli"

void main(in float4 svpos : SV_POSITION, out float4 outColor : SV_Target)
{
    // 1) スクリーン UV + シーン深度（非線形, R32_FLOAT）
    int2   pix   = int2(svpos.xy);
    float  depth = g_Depth.Load(int3(pix, 0));
    float2 uv    = svpos.xy * DScreenInv.xy;

    // 2) ワールド座標復元（row-vector: clip * InvViewProj）
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    float4 clipP = float4(ndc, depth, 1.0f);
    float4 wpos4 = mul(clipP, InvViewProj);
    float3 worldPos = wpos4.xyz / wpos4.w;

    // 3) デカールボックス局所へ→範囲外破棄
    DecalGpu D = g_Decals[gBaseInstance];
    float3 local = mul(float4(worldPos, 1.0f), D.worldToBox).xyz;   // 内側なら [-1,1]^3
    clip(1.0f - max(max(abs(local.x), abs(local.y)), abs(local.z)));

    // 面内 UV: 局所 X が投影/法線軸、Y/Z が footprint（U を長さ側 z に合わせる）
    float2 dUV = float2(local.z, local.y) * 0.5f + 0.5f;
    float4 tex = g_Base.Sample(g_Sampler, dUV);

    // 4) 深度からの面法線 + 法線フェード（斜面/裏面を除外）
    float3 dPx = ddx(worldPos);
    float3 dPy = ddy(worldPos);
    float3 N   = normalize(cross(dPy, dPx));
    // cross の符号は曖昧なので、可視面法線＝カメラ向きになるよう補正（受け面法線を確定）。
    float3 V = normalize(CameraWorld.xyz - worldPos);
    if (dot(N, V) < 0.0f) N = -N;
    // 投影軸に面した所へ乗せる。曲面にも回り込むよう帯を広めに。
    float3 projDir = normalize(D.projAxisWorld.xyz);
    float  fade  = smoothstep(0.15f, 0.55f, dot(N, projDir));

    // 5) アルベド + 不透明度
    float3 albedo = pow(saturate(tex.rgb), 2.2f);   // sRGB→linear（TownDecalPS と一致）
    float  a = tex.a * DTint.a * fade;
    clip(a - 0.004f);

    // 6) 路面と同じライティング（太陽*影 + irradiance）
    float3 L      = normalize(SunDirection.xyz);
    float  NdotL  = saturate(dot(N, L));
    float  shadow = SampleSunShadowSoft(worldPos, N, svpos.xy);
    float3 radiance = SunColor.rgb * max(SunDirection.w, 1.0f) * kSunScale;
    float3 direct   = (albedo / PI) * radiance * NdotL * shadow;
    float3 ambient  = g_Irradiance.Sample(g_Sampler, N).rgb * albedo;
    float3 lit      = (direct + ambient) * DTint.rgb;

    outColor = float4(lit * a, a);   // premultiplied
}
