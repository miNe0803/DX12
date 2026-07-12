// ============================================================
//  TownDeferredDecalVS.hlsl — スクリーン空間デファードデカールの
//  ボックス投影 VS。共有ユニットキューブ([-1,1]^3)を、per-decal の
//  ボックスワールド行列(= transpose(BW)) で world→clip へ変換する。
//  ボックスは g_Worlds(t0,space1) から gBaseInstance で選択（TownVS と同じ経路）。
//  PS が深度からワールド座標を復元するので出力は SV_POSITION のみ。
// ============================================================

StructuredBuffer<float4x4> g_Worlds : register(t0, space1);   // per-decal BOX world = transpose(BW)

cbuffer InstBase : register(b0, space1)
{
    uint  gBaseInstance;   // = デカール index（1 ドロー 1 ボックス）
    uint3 _instPad;
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

// 共有 Vertex(84B) 入力レイアウト。使うのは .pos のみ。
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

float4 vert(VSInput input, uint iid : SV_InstanceID) : SV_POSITION
{
    float4x4 BoxWorld = g_Worlds[gBaseInstance + iid];   // = transpose(BW)
    float4 wp = mul(float4(input.pos, 1.0f), BoxWorld);  // 単位キューブ頂点→world
    float4 vp = mul(wp, View);
    return mul(vp, Proj);
}
