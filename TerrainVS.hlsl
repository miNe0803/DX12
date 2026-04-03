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

    float4 worldPos = mul(float4(input.pos, 1.0f), World);
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
