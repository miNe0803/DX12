// ============================================================
//  Rtao_CS.hlsl — DXR-GI Phase R: レイトレース アンビエントオクルージョン（inline RayQuery, SM6.6）。
//  シーン深度からワールド座標＋法線を復元し、コサイン半球でN本の遮蔽レイを TLAS へ飛ばして
//  可視率(=AO)を推定。半解像度 R16 出力。適用は GTAOApply_PS が HDR に乗算（GTAOと同経路）。
//
//  重要（同一平面自己交差対策）: 町は道路プレーンと地形ハイトフィールドがほぼ同一平面で重なる
//  （F1検証ビューの道路赤の正体）。レイ原点をそのまま面上に置くと即座に隣接コプラナ面へ当たり
//  地面が真っ黒に潰れる。→ 原点を法線方向へ NormalBias(数cm) 押し出し、かつ TMin>0 で近接ヒットを
//  スキップする。TMax=AO半径 で遮蔽レンジとレイコストを同時に制限。
// ============================================================

cbuffer RtaoCb : register(b0)
{
    matrix InvViewProj;   // clip→world（深度からワールド復元）
    float4 CamPos;        // .xyz カメラ世界位置
    float2 InvRes;        // (1/幅, 1/高さ)  ※半解像度
    float  Radius;        // AO 半球半径 = レイ TMax (world m)
    float  NormalBias;    // レイ原点の法線押し出し量 (world m, コプラナ自己交差回避)
    float  TMin;          // レイ最小距離 (world m, 近接自己交差スキップ)
    int    RayCount;      // 1ピクセルあたりの遮蔽レイ本数
    float  Strength;      // AO 強度 0..1
    uint   FrameIndex;    // （R2 時間デノイズ用のジッタ種。R1では固定で使用）
};

RaytracingAccelerationStructure Scene : register(t0);   // 町 TLAS（ルートSRVでバインド）
Texture2D<float>  Depth : register(t1);
RWTexture2D<float> AO    : register(u0);
SamplerState      Smp    : register(s0);

float3 ReconWorld(float2 uv, float d)
{
    float2 ndc = uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 c = float4(ndc, d, 1.0f);
    float4 w = mul(c, InvViewProj);
    return w.xyz / w.w;
}

// 低偏差列（Hammersley）+ ピクセル毎の Cranley-Patterson 回転で、少数レイでも均一に分散。
float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f; // / 2^32
}
float2 Hammersley(uint i, uint n) { return float2((float)i / (float)n, RadicalInverseVdC(i)); }
float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5f) * InvRes;
    float d = Depth.SampleLevel(Smp, uv, 0);
    if (d >= 1.0f) { AO[id.xy] = 1.0f; return; }   // 空はAO無し

    float3 P = ReconWorld(uv, d);
    // 深度の中心差分からワールド法線（G-buffer 無し）。前方差分より対称でノイズ半減
    // → 平面上でハミスフィアが傾かず、路面の偽遮蔽ムラを抑える。
    float2 dx = float2(InvRes.x, 0), dy = float2(0, InvRes.y);
    float3 Pxp = ReconWorld(uv + dx, Depth.SampleLevel(Smp, uv + dx, 0));
    float3 Pxm = ReconWorld(uv - dx, Depth.SampleLevel(Smp, uv - dx, 0));
    float3 Pyp = ReconWorld(uv + dy, Depth.SampleLevel(Smp, uv + dy, 0));
    float3 Pym = ReconWorld(uv - dy, Depth.SampleLevel(Smp, uv - dy, 0));
    float3 N = normalize(cross(Pxp - Pxm, Pyp - Pym));
    if (dot(N, CamPos.xyz - P) < 0.0f) N = -N;

    // 接空間基底
    float3 up = abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    // 原点をコプラナ自己交差の外へ押し出す（最重要）
    float3 O = P + N * NormalBias;

    // ピクセル毎の回転種 + フレーム毎の前進（R2 黄金比列）。時間デノイザが跨フレームで
    // 別方向を積分できるよう毎フレーム別の分散にする（これが無いと時間平均が効かない）。
    float2 cp = float2(Hash12(id.xy), Hash12(id.xy + 17.0f));
    cp = frac(cp + (float)FrameIndex * float2(0.7548776662f, 0.5698402909f));

    int rc = max(RayCount, 1);
    float occ = 0.0f;
    [loop] for (int i = 0; i < rc; ++i)
    {
        float2 u = Hammersley((uint)i, (uint)rc);
        u = frac(u + cp);                              // Cranley-Patterson 回転
        float r = sqrt(u.x);
        float phi = 6.28318530718f * u.y;
        float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0f, 1.0f - u.x))); // コサイン半球
        float3 dir = T * local.x + B * local.y + N * local.z;

        RayDesc ray;
        ray.Origin = O;
        ray.Direction = dir;
        ray.TMin = TMin;      // 近接コプラナ面スキップ
        ray.TMax = Radius;    // AO 半径

        // closest-hit（ACCEPT_FIRST_HIT を外す）→ 最近傍遮蔽の距離で重み付け。
        // 近い遮蔽ほど強く、遠い擦過ヒット（隣接路面/コプラナ面）は弱く数える
        // ＝ 開けた平面の偽遮蔽ムラを抑えつつ接地の陰は保つ（UE風フォールオフ）。
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(Scene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0x01u, ray);   // 静的のみ(動的キャラ除外, デノイズ残像防止)
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            float t = q.CommittedRayT();
            occ += saturate(1.0f - t / Radius);   // 距離減衰
        }
    }

    float ao = saturate(1.0f - occ / (float)rc);
    ao = lerp(1.0f, ao, Strength);
    AO[id.xy] = ao;
}
