#ifndef DDGI_HLSLI
#define DDGI_HLSLI
// ============================================================
//  Ddgi.hlsli — DXR-GI Phase G: SH-L1 irradiance probe field の共有ヘルパ。
//  プローブ更新CS（放射輝度をSHへ投影）と TownPS（プローブ場をサンプル）の双方が include し、
//  「格納した係数」と「読み出す評価」の規約を一致させる。band0+band1（各色4係数=12float/probe）。
// ============================================================

// 1プローブ = RGB 各 SH-L1（c0, c1(y), c2(z), c3(x)）= 48 bytes（C++ 側 struct と一致）。
struct ProbeSH { float4 r; float4 g; float4 b; };

static const float DDGI_Y0 = 0.2820947918f;  // 1/(2√π)
static const float DDGI_Y1 = 0.4886025119f;  // √(3/(4π))

// 放射輝度サンプル L を方向 dir で SH へ加算（積分の重み 4π/N はループ後に一括適用）。
void SH_ProjectAddRadiance(inout ProbeSH acc, float3 dir, float3 L)
{
    float w0 = DDGI_Y0;
    float w1 = DDGI_Y1 * dir.y;
    float w2 = DDGI_Y1 * dir.z;
    float w3 = DDGI_Y1 * dir.x;
    acc.r += float4(L.r * w0, L.r * w1, L.r * w2, L.r * w3);
    acc.g += float4(L.g * w0, L.g * w1, L.g * w2, L.g * w3);
    acc.b += float4(L.b * w0, L.b * w1, L.b * w2, L.b * w3);
}

ProbeSH SH_Zero()
{
    ProbeSH p;
    p.r = float4(0, 0, 0, 0); p.g = float4(0, 0, 0, 0); p.b = float4(0, 0, 0, 0);
    return p;
}
ProbeSH SH_Scale(ProbeSH p, float s) { p.r *= s; p.g *= s; p.b *= s; return p; }
ProbeSH SH_Lerp(ProbeSH a, ProbeSH b, float t)
{
    ProbeSH o; o.r = lerp(a.r, b.r, t); o.g = lerp(a.g, b.g, t); o.b = lerp(a.b, b.b, t); return o;
}

// SH 放射輝度係数を法線 n 方向の「irradiance/π」(= TownPS の irr と同義, albedo を掛ければ拡散) に評価。
// コサインローブ畳み込み A0=π, A1=2π/3 → E/π = Y0*c0 + (2/3)*Y1*(n·(c1,c2,c3 の並びは y,z,x))。
float3 SH_EvalIrradiance(ProbeSH p, float3 n)
{
    float3 c0 = float3(p.r.x, p.g.x, p.b.x);
    float3 c1 = float3(p.r.y, p.g.y, p.b.y);
    float3 c2 = float3(p.r.z, p.g.z, p.b.z);
    float3 c3 = float3(p.r.w, p.g.w, p.b.w);
    float3 E = DDGI_Y0 * c0 + (2.0f / 3.0f) * DDGI_Y1 * (n.y * c1 + n.z * c2 + n.x * c3);
    return max(E, 0.0f);
}

// グリッド座標ヘルパ
uint Ddgi_ProbeIndex(uint3 coord, uint3 dims) { return coord.x + dims.x * (coord.y + dims.y * coord.z); }
float3 Ddgi_ProbeWorldPos(uint3 coord, float3 origin, float3 spacing) { return origin + (float3)coord * spacing; }

// ワールド位置 wp・法線 n でプローブ場をトライリニアにサンプル → irr 相当（float3）。
// G-a はプレーンなトライリニア。G-c で法線ベースのコーナー重み（漏れ抑制）を追加予定。
float3 Ddgi_SampleField(StructuredBuffer<ProbeSH> probes, float3 wp, float3 n,
                        float3 origin, float3 spacing, uint3 dims)
{
    float3 g = (wp - origin) / spacing;
    float3 gf = clamp(g, 0.0f, (float3)(dims - 1));
    int3 base = (int3)floor(gf);
    base = clamp(base, int3(0, 0, 0), (int3)dims - 2);
    float3 t = saturate(gf - (float3)base);

    float3 sum = 0.0f; float wsum = 0.0f;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 c = base + off;
        float w = (off.x ? t.x : 1.0f - t.x) * (off.y ? t.y : 1.0f - t.y) * (off.z ? t.z : 1.0f - t.z);
        uint idx = Ddgi_ProbeIndex((uint3)c, dims);
        sum += w * SH_EvalIrradiance(probes[idx], n);
        wsum += w;
    }
    return sum / max(wsum, 1e-4f);
}

#endif
