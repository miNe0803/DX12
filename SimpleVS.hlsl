// PBR instanced path: b0 = View/Proj (space0), StructuredBuffer World at t0 space1.
// Root: CBV0 scene, CBV1 material, Root SRV instance (t0,s1), tables t0-t5 材質 / t6-t8 IBL (space0) on PS.
// View/Proj: C++ は XMMATRIX をそのまま書く（列メジャー格納）→ HLSL 既定 column_major と一致
cbuffer SceneCB : register(b0)
{
    matrix View;
    matrix Proj;
    float4 CameraWorld;
};

struct InstanceData
{
    matrix World;
    float4 NprPerMesh;
};

StructuredBuffer<InstanceData> gInstanceData : register(t0, space1);

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
    float4 nprPerMesh : TEXCOORD2;
};

VSOutput vert(VSInput input, uint instanceID : SV_InstanceID)
{
    VSOutput output;

    matrix worldM = gInstanceData[instanceID].World;
    output.nprPerMesh = gInstanceData[instanceID].NprPerMesh;

    float4 worldPos = mul(float4(input.pos, 1.0f), worldM);
    float4 viewPos = mul(worldPos, View);
    output.svpos = mul(viewPos, Proj);

    // 頂点カラーが未設定のメッシュでは COLOR が 0 になり、シェーダで albedo *= input.color の結果が黒になってしまう。
    // その場合だけ RGB を白にフォールバックする（意図的に黒の場合は例外的に残る）。
    float4 c = input.color;
    if ((abs(c.r) + abs(c.g) + abs(c.b)) < 1e-4f)
        c.rgb = float3(1.0f, 1.0f, 1.0f);
    output.color = c;
    output.uv = input.uv;

    float3 n = (length(input.normal) > 0.1) ? input.normal : float3(0, 0, 1);
    float3 t = (length(input.tangent) > 0.1) ? input.tangent : float3(1, 0, 0);

    output.normal = normalize(mul(n, (float3x3)worldM));
    output.tangent = normalize(mul(t, (float3x3)worldM));
    output.worldPos = worldPos.xyz;

    return output;
}
