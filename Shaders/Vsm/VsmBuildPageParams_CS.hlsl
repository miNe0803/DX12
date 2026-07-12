// ============================================================
//  VsmBuildPageParams_CS.hlsl — VSM V3c-m1: 割当済み各物理ページの「描画変換パラメータ」を構築。
//  逆引き PhysToVirtual[phys]→ 仮想ページ(level, px, py) を復号し、そのページを描画する時に VS が
//  ワールド→(そのレベルのライト空間 ortho)→ページ内 → 物理アトラスのタイル(tx,ty) へ配置するのに
//  必要な (レベル中心/範囲, px,py, tx,ty) を出力。V3c-m3 の per-page 描画(ExecuteIndirect)で参照。
// ============================================================
StructuredBuffer<uint>     PhysToVirtual : register(t0);   // phys -> vp
StructuredBuffer<uint>     Counter       : register(t1);   // [0]=割当数
RWStructuredBuffer<float4> PageCenterExtent : register(u0); // per phys: (cx, cy, extent, 0)
RWStructuredBuffer<uint4>  PageTile         : register(u1); // per phys: (px, py, tx, ty)

// VsmSystem::VsmConstants と一致（GetConstantsAddress を b0 にバインド）
cbuffer VsmCB : register(b0)
{
    matrix Vsm_LightView;
    matrix Vsm_InvViewProj;
    float4 Vsm_Params;      // x=levelCount, y=pageSize, z=vppr, w=appr
    float4 Vsm_ZParams;
    float4 Vsm_DepthDim;
    float4 Vsm_LevelCenterExtent[8];
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint phys = id.x;
    if (phys >= Counter[0]) return;   // 未割当ページはスキップ

    uint vppr   = (uint)Vsm_Params.z;
    uint appr   = (uint)Vsm_Params.w;
    uint levels = (uint)Vsm_Params.x;

    uint vp = PhysToVirtual[phys];
    uint perLevel = vppr * vppr;
    uint level = vp / perLevel;
    uint rem   = vp % perLevel;
    uint py = rem / vppr;
    uint px = rem % vppr;

    float4 lce = (level < levels) ? Vsm_LevelCenterExtent[level] : Vsm_LevelCenterExtent[0];
    PageCenterExtent[phys] = float4(lce.x, lce.y, lce.z, (float)level);
    PageTile[phys] = uint4(px, py, phys % appr, phys / appr);
}
