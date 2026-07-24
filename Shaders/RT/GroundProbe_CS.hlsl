// ============================================================
//  GroundProbe_CS.hlsl — キャラの接地スナップ用。プレイヤーXZから下向きに TLAS へ 1 本レイを飛ばし、
//  地面のワールドY を取得する。inline RayQuery(SM6.6)。静的のみ(mask 0x01)＝動的キャラ自身を除外。
//  結果は readback で CPU へ返し、PlayerSystem が足を接地させる（数フレーム遅延, 歩行では実用上OK）。
//  将来: 法線を GeoInfo フェッチで追加すれば Foot IK/足首法線合わせ/接地AO に再利用可。
// ============================================================
cbuffer GPCB : register(b0)
{
    float3 gOrigin;   // レイ始点（キャラ頭上少し）
    float  gTMax;     // 最大距離
};
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<float4> gOut : register(u0);   // [0] = (groundY, 0, 0, hit)

[numthreads(1, 1, 1)]
void main()
{
    RayDesc r;
    r.Origin = gOrigin;
    r.Direction = float3(0.0f, -1.0f, 0.0f);
    r.TMin = 0.0f;
    r.TMax = gTMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(Scene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0x01u, r);   // 静的のみ(キャラ除外)
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        gOut[0] = float4(gOrigin.y - q.CommittedRayT(), 0.0f, 0.0f, 1.0f);
    else
        gOut[0] = float4(-100000.0f, 0.0f, 0.0f, 0.0f);   // miss
}
