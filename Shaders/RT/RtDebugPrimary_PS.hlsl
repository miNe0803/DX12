// ============================================================
//  RtDebugPrimary_PS.hlsl — DXR-GI F1 検証ビュー（フルスクリーンPS + inline RayQuery, SM6.5+）。
//  カメラから各ピクセル方向へ primary ray を TLAS へトレースし、そのヒット距離を
//  シーン深度から復元した距離と比較して色分けする。TLAS 登録とインスタンスの
//  トランスフォーム(3x4)が正しければ、ジオメトリ面はほぼ緑一色になる。
//    緑   = RTヒット距離 ≈ ラスタ深度距離（TLAS 正しい）
//    赤   = 不一致（トランスフォーム/ジオメトリずれ）
//    青   = ラスタにジオメトリ有りだが RT が取りこぼし（TLAS 穴 or 除外フォリッジ）
//    暗   = 空（RTミス かつ 深度=遠）
// ============================================================
cbuffer RtDbgCB : register(b0)
{
    matrix InvViewProj;   // clip -> world（アップロード時に転置済 = HLSL column-major）
    float4 CameraPos;     // .xyz
    float4 _rtpad0;       // waterSurfaceY/outputW/H（未使用, RTConstants と同レイアウト）
};

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float>                SceneDepth : register(t1);
SamplerState                    PointSmp : register(s0);

struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(PSInput i) : SV_TARGET
{
    float d = SceneDepth.SampleLevel(PointSmp, i.uv, 0);

    // ピクセル方向のワールド レイ（遠平面点 - カメラ）。
    float2 ndc = i.uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 wpf = mul(float4(ndc, 1.0f, 1.0f), InvViewProj);
    float3 farP = wpf.xyz / wpf.w;
    float3 ro = CameraPos.xyz;
    float3 rd = normalize(farP - ro);

    RayQuery<RAY_FLAG_NONE> q;
    RayDesc ray;
    ray.Origin = ro; ray.Direction = rd; ray.TMin = 0.02f; ray.TMax = 100000.0f;
    q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFFu, ray);
    q.Proceed();

    bool hit = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT);
    if (!hit)
        return (d < 1.0f) ? float4(0.0f, 0.0f, 1.0f, 1.0f)      // ラスタ有り・RTミス = 青
                          : float4(0.02f, 0.02f, 0.05f, 1.0f);  // 空
    if (d >= 1.0f)
        return float4(1.0f, 0.0f, 1.0f, 1.0f);   // 空だが RT ヒット = マゼンタ（想定外）

    float rtT = q.CommittedRayT();
    float4 wp = mul(float4(ndc, d, 1.0f), InvViewProj);
    float3 wpos = wp.xyz / wp.w;
    float rasterT = length(wpos - ro);
    float rel = abs(rtT - rasterT) / max(rasterT, 1.0f);   // 相対誤差
    // 緑(一致)→赤(不一致)。rel~0 で緑、rel>=0.02 で赤。
    float m = saturate(rel * 50.0f);
    return float4(m, 1.0f - m, 0.0f, 1.0f);
}
