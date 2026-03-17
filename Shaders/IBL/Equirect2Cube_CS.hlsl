// 1 にすると 6 面を単色で出力（+X=赤,-X=水,+Y=緑,-Y=黄,+Z=青,-Z=マゼンタ）。面の対応確認用。
#define DEBUG_CUBEMAP_FACES 0

cbuffer Params : register(b0)
{
    uint faceIndex;
    uint width;
    uint height;
    uint padding;
}

Texture2D<float4> Equirect : register(t0);
RWTexture2D<float4> CubeFace : register(u0);
SamplerState LinearSampler : register(s0);

static const float PI = 3.14159265359;

// DirectX TextureCube の「方向→(face,s,t)」の逆: 書き込み (face, u, v) に対応する方向を返す。
// サンプル時: +X で s=(z/x+1)/2, t=(y/x+1)/2 等。よって書き込み (s,t) には方向 (1, 2t-1, 2s-1) の色を入れる。
// uv は [0,1]。ndc = 2*uv-1 で (s,t) と一致させる。
float3 FaceToDirection(uint face, float2 uv)
{
    float u = uv.x * 2.0f - 1.0f;  // ndc
    float v = uv.y * 2.0f - 1.0f;
    
    switch (face)
    {
        case 0: return normalize(float3(1.0f, v, u));    // +X: (1, 2t-1, 2s-1) = (1,v,u)
        case 1: return normalize(float3(-1.0f, -v, u));   // -X
        case 2: return normalize(float3(u, 1.0f, -v));     // +Y
        case 3: return normalize(float3(u, -1.0f, -v));   // -Y: (2s-1,-1,1-2t) = (u,-1,-v)
        case 4: return normalize(float3(u, -v, 1.0f));    // +Z
        case 5: return normalize(float3(-u, -v, -1.0f));  // -Z
        default: return float3(0, 0, 0);
    }
}

// 方向 → Equirect UV。v=0 を上（北極・空）、u=0 を正面方向に合わせる。
float2 DirectionToEquirectUV(float3 dir)
{
    float lon = atan2(dir.z, dir.x);
    float lat = asin(clamp(dir.y, -1.0f, 1.0f));
    float u = lon / (2.0f * PI) + 0.5f;
    float v = 1.0f - (lat / PI + 0.5f);  // lat=+π/2 (上) → v=0
    return float2(u, v);
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= height)
        return;
    
#if DEBUG_CUBEMAP_FACES
    float4 faceColors[6] = {
        float4(1, 0, 0, 1),   // +X 赤
        float4(0, 1, 1, 1),   // -X シアン
        float4(0, 1, 0, 1),   // +Y 緑（上＝空）
        float4(1, 1, 0, 1),   // -Y 黄
        float4(0, 0, 1, 1),   // +Z 青
        float4(1, 0, 1, 1)   // -Z マゼンタ
    };
    CubeFace[DTid.xy] = faceColors[faceIndex];
    return;
#endif
    
    float2 uv = (float2(DTid.xy) + 0.5f) / float2(width, height);
    float3 dir = FaceToDirection(faceIndex, uv);
    float2 equirectUV = DirectionToEquirectUV(dir);
    
    CubeFace[DTid.xy] = Equirect.SampleLevel(LinearSampler, equirectUV, 0);
}
