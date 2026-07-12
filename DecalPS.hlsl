// デカール専用ピクセルシェーダー

Texture2D g_DecalAlbedo : register(t0, space0);
Texture2D g_DecalNormal : register(t1, space0);
Texture2D g_DecalRoughness : register(t2, space0);
Texture2D g_DecalOpacity : register(t3, space0);
SamplerState g_DefualtSampler : register(s0, space0);

cbuffer DecalBuffer : register(b1, space0)
{
    float4x4 g_WorldMatrix;
    uint g_IsPuddle; // 0: 通常デカール, 1: 水たまりデカール
    float3 g_Padding; // パディング
};

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float opacity = g_DecalOpacity.Sample(g_DefualtSampler, input.UV).r;
    
    clip(opacity - 0.01f); // 透明度が低い場合は描画しない
    float3 albedo = g_DecalAlbedo.Sample(g_DefualtSampler, input.UV).rgb;
    float3 normalMap = g_DecalNormal.Sample(g_DefualtSampler, input.UV).rgb;
    float roughness = g_DecalRoughness.Sample(g_DefualtSampler, input.UV).r;
    
    if(g_IsPuddle == 1)
    {
        // 水たまりデカールの場合、反射や屈折の処理を追加する
        // ここでは簡易的にアルベドを暗くして水っぽさを表現
        roughness = 0.0f; // 水たまりは滑らかにする
        normalMap = float3(0.0f, 1.0f, 0.0f); // 法線を上向きにする
        albedo *= 0.5f;
    }
    
    return float4(albedo, opacity);
}
