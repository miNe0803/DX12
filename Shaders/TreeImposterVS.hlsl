// LOD1 インポスター: ビルボード + 8方向アトラス UV（BakeTreeLOD0 8 スライス横並び）
cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
};

cbuffer TreeVisibleCB : register(b2, space1)
{
    uint u0, u1, u2, u3, u4, u5, u6, u7;
};
static const uint VisibleBase = u0;

struct InstanceData
{
    matrix World;
    float4 NprPerMesh;
};

StructuredBuffer<InstanceData> gInstanceData : register(t0, space1);
StructuredBuffer<uint> gVisibleIndex : register(t1, space1);

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
    float2 uv : TEXCOORD;       // y = アトラス縦、x は未使用（PS で角度から U を再計算）
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float3 worldPos : TEXCOORD1;
    float4 nprPerMesh : TEXCOORD2;
    float sliceU : TEXCOORD3;   // クワッド横方向 0..1（隣接スライス補間用）
    nointerpolation float3 treeBase : TEXCOORD4; // インスタンスの根元（スライス方位は板の各点ではなくここ基準＝VS と一致）
};

static const float kImposterHalfW = 6.0;
static const float kImposterHalfH = 10.0;

VSOutput vert(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;



    const uint instanceIndex = gVisibleIndex[instanceID] + u0;
    matrix worldM = gInstanceData[instanceIndex].World;
    output.nprPerMesh = gInstanceData[instanceIndex].NprPerMesh;

    float3 footLocal = float3(asfloat(u1), asfloat(u2), asfloat(u3));
    float billboardHalfW = asfloat(u4);
    float billboardHeight = asfloat(u5);
    if (billboardHeight < 1e-4f)
        billboardHeight = 1.0f;
    if (billboardHalfW < 1e-4f)
        billboardHalfW = 1.0f;

    float4 footH = mul(float4(footLocal, 1.0f), worldM);
    float3 footWorld = footH.xyz;

    float3 toCam = CameraWorld.xyz - footWorld;
    toCam.y = 0.0;
    float lenL = length(toCam);
    if (lenL < 1e-4f)
        toCam = float3(0, 0, 1);
    else
        toCam /= lenL;

    float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), toCam));
    float3 up = float3(0.0f, 1.0f, 0.0f);

    // クワッド pos.xy は [-0.5,0.5]：底辺 y=-0.5 を足元、上方向に billboardHeight だけ伸ばす（ベイクの足元原点と一致）
    float3 pos = footWorld + right * (input.pos.x * 2.0f * billboardHalfW)
        + up * ((input.pos.y + 0.5f) * billboardHeight);

    float4 viewPos = mul(float4(pos, 1.0f), View);
    output.svpos = mul(viewPos, Proj);

    // U（アトラス横）は PS でカメラ方位の連続値から隣接スライスを lerp。ここでは縦 UV と横ブレンド用のみ渡す。
    float4 c = input.color;
    if ((abs(c.r) + abs(c.g) + abs(c.b)) < 1e-4f)
        c.rgb = float3(1.0f, 1.0f, 1.0f);
    output.color = c;
    output.uv = float2(0.0f, input.uv.y);
    output.sliceU = input.uv.x;
    output.normal = float3(0, 0, 1);
    output.tangent = float3(1, 0, 0);
    output.worldPos = pos;
    output.treeBase = footWorld;
    return output;
}
