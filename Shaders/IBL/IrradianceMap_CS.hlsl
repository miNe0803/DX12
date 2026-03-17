// IrradianceMap_CS.hlsl
// 生成済みの環境キューブマップ(EnvMap)を読み込み、半球積分して拡散用の放射照度マップを作る

#define PI 3.14159265359

cbuffer Params : register(b0)
{
    uint faceIndex; // 0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z（Equirect2Cube_CS と同じ順）
    uint width;     // IrradianceMap の 1 面のサイズ（通常 32）
    uint padding0;
    uint padding1;
}

TextureCube<float4> EnvMap : register(t0);        // 環境キューブマップ
RWTexture2D<float4> IrradianceFace : register(u0); // 書き込み先の 1 面

SamplerState LinearSampler : register(s0);

// Equirect2Cube_CS.hlsl と同じ DirectX ルール（面の対応を一致させる）
float3 FaceToDirection(uint face, float2 uv)
{
    float u = uv.x * 2.0f - 1.0f;
    float v = uv.y * 2.0f - 1.0f;

    switch (face)
    {
        case 0: return normalize(float3(1.0f, v, u));     // +X
        case 1: return normalize(float3(-1.0f, -v, u));   // -X
        case 2: return normalize(float3(u, 1.0f, -v));    // +Y
        case 3: return normalize(float3(u, -1.0f, -v));   // -Y
        case 4: return normalize(float3(u, -v, 1.0f));     // +Z
        case 5: return normalize(float3(-u, -v, -1.0f)); // -Z
        default: return float3(0, 0, 0);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= width || DTid.y >= width)
        return;

    // このピクセルが表す方向（法線 N）
    float2 uv = (float2(DTid.xy) + 0.5f) / float(width);
    float3 N = FaceToDirection(faceIndex, uv);

    // 半球積分用の接空間基底
    float3 up = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    float3 irradiance = float3(0.0, 0.0, 0.0);
    uint sampleCount = 0;
    float sampleDelta = 0.025;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            irradiance += EnvMap.SampleLevel(LinearSampler, sampleVec, 0).rgb * cos(theta) * sin(theta);
            sampleCount++;
        }
    }

    irradiance = PI * irradiance * (1.0 / float(sampleCount));
    IrradianceFace[DTid.xy] = float4(irradiance, 1.0);
}
