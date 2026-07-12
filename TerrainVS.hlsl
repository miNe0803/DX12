cbuffer Transform : register(b0)
{
    matrix World;
    matrix View;
    matrix Proj;
};

struct ChunkDrawPayload
{
    uint chunkId;
    uint lod;
    float morph;
    float pad;
};
StructuredBuffer<ChunkDrawPayload> gDrawPayload : register(t0, space1);

// t0-t5 のテレインマスク (PS と同じレイアウト)。VS では川マスクのみ使用。
SamplerState smp : register(s0);
Texture2D _TreeMaskVS   : register(t0);
Texture2D _NatureMaskVS : register(t1);
Texture2D _GroundDiffVS : register(t2);
Texture2D _GroundDispVS : register(t3);
Texture2D _RiversMaskVS : register(t4);
Texture2D _SnowMaskVS   : register(t5);

// Rivers_Rivers (Gaea) は 0/1 のシャープなパスマスク。
// 多点ガウスぼかしで滑らかな勾配に変換してから掘削に使用 → 急な崖を回避。
static const float kRiverCarveDepth = 6.0;   // ぼかし後の値=1.0 のときの最大掘削量 (m)
static const float kRiverCarveStart = 0.10;  // 水可視 (0.20) より早めに掘り始めて土手を作る

// 5x5 ガウス相当のぼかし (1テクセル = 1/512)
float SmoothRiverMaskVS(float2 uv)
{
    const float t = 1.0 / 512.0;
    float v = 0.0;
    // 中央 (重み 4)
    v += _RiversMaskVS.SampleLevel(smp, uv, 0).r * 4.0;
    // 4近傍 (重み 2)
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( t,  0), 0).r * 2.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-t,  0), 0).r * 2.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0,  t), 0).r * 2.0;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0, -t), 0).r * 2.0;
    // 角 (重み 1)
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( t,  t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-t,  t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( t, -t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-t, -t), 0).r;
    // 2 テクセル外側 (重み 1) で更に幅広に
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 2*t, 0), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2(-2*t, 0), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0,  2*t), 0).r;
    v += _RiversMaskVS.SampleLevel(smp, uv + float2( 0, -2*t), 0).r;
    return v / 20.0; // 4+2*4+1*4+1*4 = 20
}

struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
};


struct VSOutput
{
    float4 svpos : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
};

VSOutput vert(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;
    (void)instanceId;

    // 川パスマスクをぼかして滑らかな勾配にしてから掘削
    float riverSmooth = SmoothRiverMaskVS(input.uv);
    float carve = saturate((riverSmooth - kRiverCarveStart) / (1.0 - kRiverCarveStart));
    carve = carve * carve * (3.0 - 2.0 * carve); // smoothstep でさらに滑らかに
    float3 localPos = input.pos;
    localPos.y -= carve * kRiverCarveDepth;

    float4 worldPos = mul(float4(localPos, 1.0f), World);

    float4 viewPos = mul(worldPos, View);
    output.svpos = mul(viewPos, Proj);

    output.color = input.color;
    output.uv = input.uv;

    float3 n = (length(input.normal) > 0.1) ? input.normal : float3(0, 0, 1);
    float3 t = (length(input.tangent) > 0.1) ? input.tangent : float3(1, 0, 0);

    output.normal = normalize(mul(n, (float3x3)World));
    output.tangent = normalize(mul(t, (float3x3)World));
    output.worldPos = worldPos.xyz;

    return output;
}
