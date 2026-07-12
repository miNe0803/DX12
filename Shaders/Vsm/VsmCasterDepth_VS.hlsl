// ============================================================
//  VsmCasterDepth_VS.hlsl — VSM V3c-m3: キャスタ深度をページの物理アトラスタイルへ配置。
//  slot0=POSITION(84B Vertex), slot1=PER_INSTANCE uint2(worldIdx, physPage)。
//  world→ライト空間→ページ内local→アトラスタイルclip。タイル境界は SV_ClipDistance×4 で
//  クリップ（隣タイル汚染防止）。Z は saturate せず HW 深度クリップに任せる（レビュー H2）。
//  UV 規約は Vsm.hlsli の Vsm_PhysicalUV / Vsm_VirtualPage と厳密一致（Y 反転に注意）。
// ============================================================
#include "VsmBinning.hlsli"   // Caster 構造体 + VsmCB(b0)

StructuredBuffer<Caster> Casters          : register(t0);   // root SRV
StructuredBuffer<float4> PageCenterExtent : register(t1);   // (cx,cy,extent,level)/page
StructuredBuffer<uint4>  PageTile         : register(t2);   // (px,py,tx,ty)/page

cbuffer DepthCb : register(b1)
{
    uint gCasterCount;
    uint gPhysCap;
    uint _a;
    uint _b;
};

struct VSIn
{
    float3 pos  : POSITION;      // slot0（84B Vertex の先頭）
    uint2  inst : TEXCOORD1;     // slot1 PER_INSTANCE: (worldIdx, physPage)
};

struct VSOut
{
    float4 pos  : SV_POSITION;
    float4 clip : SV_ClipDistance0;   // タイル矩形 4 本（>=0 で内側）
};

VSOut main(VSIn i)
{
    VSOut o;
    uint worldIdx = i.inst.x;
    uint phys     = i.inst.y;
    // レビュー C1: 範囲外（cap 超過の未書込み残骸など）は退場させ NaN/OOB を封じる
    if (worldIdx >= gCasterCount || phys >= gPhysCap)
    {
        o.pos = float4(2.0f, 2.0f, 2.0f, 1.0f);
        o.clip = float4(-1.0f, -1.0f, -1.0f, -1.0f);
        return o;
    }

    Caster c = Casters[worldIdx];
    float3 wp = mul(float4(i.pos, 1.0f), c.world).xyz;        // world（TownVS と同規約）
    float3 ls = mul(float4(wp, 1.0f), Vsm_LightView).xyz;     // ライト空間

    float4 ce = PageCenterExtent[phys];
    uint4  pt = PageTile[phys];
    float vppr = Vsm_Params.z, appr = Vsm_Params.w;
    float E = ce.z, pw = E / vppr;
    if (pw <= 1e-6f)
    {
        o.pos = float4(2.0f, 2.0f, 2.0f, 1.0f);
        o.clip = float4(-1.0f, -1.0f, -1.0f, -1.0f);
        return o;
    }

    // このページ（px,py）の左下（ライト空間）→ ページ内 [0,1]（= サンプル側 inPageUV）
    float ox = (ce.x - E * 0.5f) + (float)pt.x * pw;
    float oy = (ce.y - E * 0.5f) + (float)pt.y * pw;
    float lx = (ls.x - ox) / pw;
    float ly = (ls.y - oy) / pw;

    // アトラス [0,1]（タイル tx,ty） → NDC（Y 反転: V 下向き）
    float ax = ((float)pt.z + lx) / appr;
    float ay = ((float)pt.w + ly) / appr;
    float ndcX = ax * 2.0f - 1.0f;
    float ndcY = 1.0f - ay * 2.0f;
    float ndcZ = (ls.z - Vsm_ZParams.x) / max(Vsm_ZParams.y - Vsm_ZParams.x, 1e-3f); // saturate しない(H2)
    o.pos = float4(ndcX, ndcY, ndcZ, 1.0f);

    // タイル矩形（アトラス NDC）へクリップ。内締め無し（=物理タイル境界ちょうど）でページを
    // 最外テクセルまで満たす。半texel内締めは最外テクセルを未描画(=1.0)に残し、受光面が
    // そこを引いて「光」になり→ページ境界に破線アクネが出ていた（内締め除去で解消）。
    // 物理タイルは互いに独立領域なので、境界ちょうどのクリップで隣タイルへの汚染は起きない。
    float xmin = ((float)pt.z / appr) * 2.0f - 1.0f;
    float xmax = ((float)(pt.z + 1) / appr) * 2.0f - 1.0f;
    float ytop = 1.0f - ((float)pt.w / appr) * 2.0f;
    float ybot = 1.0f - ((float)(pt.w + 1) / appr) * 2.0f;
    o.clip = float4(ndcX - xmin, xmax - ndcX, ndcY - ybot, ytop - ndcY);
    return o;
}
