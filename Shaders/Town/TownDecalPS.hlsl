// ============================================================
//  TownDecalPS.hlsl — デカール（地面/壁へ投影するRGBAクアッド）。
//  横断歩道・道路白線・ひび割れ・水たまり・汚れなど。
//
//  重要: デカールは「路面と同じライティング」で描く。
//   従来は未ライティング（生テクセル）で HDR に書いていたため、
//   ・影の中では路面が暗いのにデカールだけ一定＝明るく浮く
//   ・汚れ/水たまり（暗いアルベド）は逆に見えなくなる
//   という不整合が出ていた。ここで太陽(影付き)+環境光(irradiance)を
//   地面法線で評価し、デカールのアルベドに掛ける。→ 路面と一貫。
//
//  出力は事前乗算アルファ( premultiplied )。PSO は ONE/INV_SRC_ALPHA。
//  ルートシグネチャは町の共有 sig（不透明パスで b1/t7/CSM は既にバインド済み）。
// ============================================================

Texture2D    g_Base       : register(t0, space0);   // デカール BaseColor (RGBA, a=マスク)
Texture2D    g_Normal     : register(t1, space0);   // B5: 兄弟 _normal（無ければ flat=(0.5,0.5,1)）
Texture2D    g_MR         : register(t2, space0);   // B5: 兄弟 _rough を G に（無ければ 0.8）
TextureCube  g_Irradiance : register(t7, space0);   // 環境光（拡散 IBL）
SamplerState g_Sampler    : register(s0, space0);

// ---- DX12 CSM 影 (space2) ----
Texture2DArray         g_Csm        : register(t0, space2);
SamplerComparisonState g_ShadowSmp  : register(s1, space2);
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
    float4 SunDirection;  // .xyz 光へ向かう方向, .w 強度
    float4 SunColor;      // .rgb
    matrix InvViewProj;
};

cbuffer DecalParams : register(b2, space0)
{
    float4 DTint;   // rgb = 明るさ/色, a = 全体の不透明度
    float4 _dpad;
};

struct PS_IN
{
    float4 svpos    : SV_POSITION;
    float3 worldPos : TEXCOORD1;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

static const float PI = 3.14159265359f;
static const float kShadowBias = 0.001f;
static const float kSunScale = 3.0f;   // TownParams.Params2.w と一致させる

// 接線が頂点に無い（デカールクアッドは Tangent=0）ため、スクリーン微分から TBN を導出
// （Christian Schüler の cotangent frame）。N=幾何法線, p=worldPos, uv=デカールUV。
float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1 = ddx(p),  dp2 = ddy(p);
    float2 du1 = ddx(uv), du2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;
    float invmax = rsqrt(max(dot(T, T), dot(B, B)));
    return float3x3(T * invmax, B * invmax, N);
}

// GGX 太陽スペキュラ（誘電体 F0=0.04）。濡れた/塗装ラインの光沢ハイライト用。
float3 SunSpecularGGX(float3 N, float3 V, float3 L, float3 radiance, float rough)
{
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float NdotV = saturate(dot(N, V)) + 1e-4f;
    float NdotL = saturate(dot(N, L));
    float VdotH = saturate(dot(V, H));
    float a  = rough * rough;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    float D  = a2 / (PI * d * d);
    float3 F0 = 0.04f;
    float3 F  = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
    float k  = a * 0.5f;
    float Gv = NdotV / (NdotV * (1.0f - k) + k);
    float Gl = NdotL / (NdotL * (1.0f - k) + k);
    float3 spec = (D * Gv * Gl) * F / max(4.0f * NdotV * NdotL, 1e-4f);
    return radiance * spec * NdotL;
}

// 高解像度 CSM + 16-tap Vogel ソフト PCF（TownPS と共有）。全カスケード対応。
#include "TownShadow.hlsli"

void main(in PS_IN In, out float4 outColor : SV_Target)
{
    float4 t = g_Base.Sample(g_Sampler, In.uv);
    float3 albedo = pow(saturate(t.rgb), 2.2f);   // sRGB→linear（不透明 TownPS.hlsl:229 と一致）
    float a = t.a * DTint.a;
    clip(a - 0.004f);

    // B5: 兄弟法線マップで幾何法線を摂動（無ければ flat=(0.5,0.5,1)→摂動なし）。
    float3 Ng = normalize(In.normal);                 // 幾何法線（ほぼ +Y）
    float3 nTS = g_Normal.Sample(g_Sampler, In.uv).xyz * 2.0f - 1.0f;
    float3x3 TBN = CotangentFrame(Ng, In.worldPos, In.uv);
    float3 N = normalize(mul(nTS, TBN));              // 摂動後の面法線
    // B5: 兄弟ラフネス（G）。無ければ fallback MR の G=0.8（＝つや消し）。
    float rough = clamp(g_MR.Sample(g_Sampler, In.uv).g, 0.04f, 1.0f);

    float3 L = normalize(SunDirection.xyz);
    float3 V = normalize(CameraWorld.xyz - In.worldPos);
    float NdotL = saturate(dot(N, L));
    float shadow = SampleSunShadowSoft(In.worldPos, N, In.svpos.xy);

    float3 radiance = SunColor.rgb * max(SunDirection.w, 1.0f) * kSunScale;
    float3 direct = (albedo / PI) * radiance * NdotL * shadow;   // Lambert 直接光（影付き）
    // B5: GGX 太陽スペキュラ（影付き）。ラフネスに応じたハイライト（濡れ/塗装ライン用）。
    float3 spec   = SunSpecularGGX(N, V, L, radiance, rough) * shadow;
    float3 ambient = g_Irradiance.Sample(g_Sampler, N).rgb * albedo; // 拡散 IBL
    float3 lit = (direct + ambient) * DTint.rgb + spec;

    outColor = float4(lit * a, a);   // premultiplied（路面と同じ露出/影に追従）
}
