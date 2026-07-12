// ============================================================
//  TownVS.hlsl — 町シーン用 頂点シェーダ (D3D12/SM6.6, GPU インスタンシング)
//  ・World は StructuredBuffer から SV_InstanceID + gBaseInstance で参照
//    （同一メッシュのインスタンスを 1 回の DrawIndexedInstanced でまとめる）
//  ・b1 = SceneConstants ( View/Proj/Camera/Sun 再利用 )
//  頂点は DX12 共通 Vertex(84B)。接線は PS 側でスクリーン微分から生成。
// ============================================================

StructuredBuffer<float4x4> g_Worlds : register(t0, space1);   // per-instance World (= transpose(BuildLocal*G))
cbuffer InstBase : register(b0, space1)
{
    uint  gBaseInstance;   // このドローのインスタンス先頭スロット
    uint  gWindEnable;     // 1=葉の風(WPO)を適用
    uint2 _instPad;
};

cbuffer SceneCB : register(b1, space0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
    float4 SunDirection;
    float4 SunColor;
    matrix InvViewProj;
};

struct VSInput
{
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD;
    float3 tangent : TANGENT;
    float4 color   : COLOR;
    uint4  boneIdx : BONEINDEX;
    float4 boneWt  : BONEWEIGHT;
};

struct PS_IN
{
    float4 svpos     : SV_POSITION;
    float3 worldPos  : TEXCOORD1;
    float3 normal    : NORMAL;
    float2 uv        : TEXCOORD0;
};

PS_IN vert(VSInput input, uint iid : SV_InstanceID)
{
    PS_IN o;
    float4x4 World = g_Worlds[gBaseInstance + iid];
    float4 worldPos = mul(float4(input.pos, 1.0f), World);

    // 葉の風（World-Position-Offset）: 基部からの高さが大きい頂点ほど揺れる。
    // 時間は CameraWorld.w。gWindEnable=1（葉パス）のときだけ適用。
    if (gWindEnable != 0u)
    {
        float  t = CameraWorld.w;
        float2 wp = worldPos.xz;
        float  phase = t * 1.6f + wp.x * 0.25f + wp.y * 0.25f;
        float  gust = sin(phase) + 0.35f * sin(phase * 2.3f + 1.7f);
        float  h = max(worldPos.y - World._42, 0.0f);      // 基部からの世界高さ(m)
        float  weight = h * 0.14f;
        float2 dir = normalize(float2(0.8f, 0.6f));
        worldPos.xz += dir * (gust * weight * 0.06f);       // 主ガスト
        worldPos.xz += (0.02f * weight) * sin(t * 6.0f + wp.x * 2.0f); // 高周波フラッター
    }

    float4 viewPos  = mul(worldPos, View);
    o.svpos    = mul(viewPos, Proj);
    o.worldPos = worldPos.xyz;

    float3 n = (length(input.normal) > 0.1f) ? input.normal : float3(0, 1, 0);
    o.normal = normalize(mul(n, (float3x3)World));
    o.uv     = input.uv;
    return o;
}
