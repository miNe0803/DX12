// Skinned character VS (NPR/PBR path). = SimpleVS.hlsl + GPU スケルタルスキニング。
// SimpleVS と同じ b0 SceneCB / gInstanceData(t0,space1) を使い、追加で bone palette を t1,space1 から読む。
//   palette[b] = transpose( offsetMatrix[b] * globalAnim[b] )  ※行ベクトル系→格納は転置(WorldMatrixと同規約)。
//   HLSL StructuredBuffer<float4x4> は既定 column-major なのでこの転置が打ち消され mul(pos, palette) が正しくスキン。
// 本増分(Inc1)は palette=全単位行列＝バインドポーズ（出力は SimpleVS と画素一致のはず）。
// root param[5] = SRV(t1,space1,VERTEX) を palette に流用（RootSignature.cpp で予約済・従来は null バインド）。
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
StructuredBuffer<float4x4>     gBonePalette  : register(t1, space1);   // 骨行列パレット（この増分は全単位）

struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    uint4  boneIndex : BONEINDEX;    // R16G16B16A16_UINT を uint4 として受ける（IAが4x16bit展開）
    float4 boneWeight : BONEWEIGHT;  // ロード時に上位4本が合計1へ正規化済（VSで再正規化しない）
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

    float3 n0 = (length(input.normal) > 0.1f) ? input.normal : float3(0, 0, 1);
    float3 t0 = (length(input.tangent) > 0.1f) ? input.tangent : float3(1, 0, 0);

    // --- GPU スキニング（モデル空間で頂点/法線/接線を骨行列でブレンド）---
    float3 sp, sn, st;
    float wsum = dot(input.boneWeight, float4(1, 1, 1, 1));
    if (wsum < 1e-4f)   // 無ウェイト頂点は素通り（原点collapse回避）
    {
        sp = input.pos; sn = n0; st = t0;
    }
    else
    {
        sp = float3(0, 0, 0); sn = float3(0, 0, 0); st = float3(0, 0, 0);
        [unroll] for (int i = 0; i < 4; ++i)
        {
            float4x4 M = gBonePalette[input.boneIndex[i]];
            float w = input.boneWeight[i];
            sp += w * mul(float4(input.pos, 1.0f), M).xyz;
            sn += w * mul(n0, (float3x3)M);
            st += w * mul(t0, (float3x3)M);
        }
    }

    float4 worldPos = mul(float4(sp, 1.0f), worldM);
    float4 viewPos = mul(worldPos, View);
    output.svpos = mul(viewPos, Proj);

    float4 c = input.color;
    if ((abs(c.r) + abs(c.g) + abs(c.b)) < 1e-4f)
        c.rgb = float3(1.0f, 1.0f, 1.0f);
    output.color = c;
    output.uv = input.uv;

    output.normal = normalize(mul(sn, (float3x3)worldM));
    output.tangent = normalize(mul(st, (float3x3)worldM));
    output.worldPos = worldPos.xyz;

    return output;
}
