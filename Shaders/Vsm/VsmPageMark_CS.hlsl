// ============================================================
//  VsmPageMark_CS.hlsl — VSM V2: シーン深度から「今フレーム必要な仮想ページ」を要求バッファへマーク。
//  各画素: 深度→worldPos→光空間→レベル選択→仮想ページ→Request[idx] を InterlockedOr(1)。
//  V3 でこの要求集合を物理ページへ割当てて描画する。
// ============================================================
#include "Vsm.hlsli"

cbuffer VsmCB : register(b0)
{
    matrix Vsm_LightView;
    matrix Vsm_InvViewProj;
    float4 Vsm_Params;      // x=levelCount, y=pageSize, z=vppr, w=appr
    float4 Vsm_ZParams;     // x=zNear, y=zFar, z=camLightX, w=camLightY
    float4 Vsm_DepthDim;    // x=width, y=height, z=1/width, w=1/height
    float4 Vsm_LevelCenterExtent[8];
};

Texture2D<float>         Depth   : register(t0);
RWStructuredBuffer<uint> Request : register(u0);
// gUseFootprintLod: Phase 1 のフットプリントLODを ON/OFF（サンプラ/デバッグとロックステップ切替）。
// OFF=従来の距離LOD（ビット単位で現行と同一＝非回帰）。gMaxMarkLevel より遠い(高)レベルは要求しない→CSM。
cbuffer MarkCb : register(b1) { uint gMaxMarkLevel; uint gUseFootprintLod; uint2 _mpad; }

// 画素+深度 → 光空間XYZ を復元（MarkPages と同一の InvViewProj→World→LightView 経路）。
float3 ReconstructLS(uint2 pix, float depth)
{
    float2 uv = (float2(pix) + 0.5f) * Vsm_DepthDim.zw;
    float2 ndc = uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 wp = mul(float4(ndc, depth, 1.0f), Vsm_InvViewProj);
    float3 P = wp.xyz / wp.w;
    return mul(float4(P, 1.0f), Vsm_LightView).xyz;
}

// 指定レベルの仮想ページを要求バッファへマーク（packed絶対ページ=残キャッシュのwrap検出キー）。
// 同一ページへ書く全画素は同値（窓内 1スロット=1絶対ページ）→非アトミック格納で安全。
void MarkLevel(float3 ls, uint L)
{
    float2 uvp;
    uint2 vp = Vsm_VirtualPage(ls.xy, Vsm_LevelCenterExtent[L].xy,
                               Vsm_LevelCenterExtent[L].z, (uint)Vsm_Params.z, uvp);
    uint idx = Vsm_PageTableIndex(L, vp, (uint)Vsm_Params.z);
    float pw = Vsm_LevelCenterExtent[L].z;
    int apx = (int)floor(ls.x / max(pw, 1e-6f));
    int apy = (int)floor(ls.y / max(pw, 1e-6f));
    uint packed = 0x80000000u | (((uint)(apx & 0x7FFF)) << 15) | ((uint)(apy & 0x7FFF));
    Request[idx] = packed;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint W = (uint)Vsm_DepthDim.x, H = (uint)Vsm_DepthDim.y;
    if (id.x >= W || id.y >= H) return;
    float d = Depth.Load(int3(id.xy, 0));
    if (d >= 1.0f) return;   // 空はページ不要

    float3 ls = ReconstructLS(id.xy, d);
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // level0 extent = pw0 * vppr

    uint L;
    if (gUseFootprintLod != 0u)
    {
        // 隣接画素(+X,+Y)の光空間XYを有限差分し、1画素が覆う光空間幅(フットプリント)を得る。
        // 深度不連続ガード: 隣接が空(d>=1)ならその軸を捨て、有効な反対軸を両軸へ流用（over-coarsen=予算安全）。
        uint2 nx = uint2(min(id.x + 1u, W - 1u), id.y);
        uint2 ny = uint2(id.x, min(id.y + 1u, H - 1u));
        float dx = Depth.Load(int3(nx, 0));
        float dy = Depth.Load(int3(ny, 0));
        bool okX = (dx < 1.0f);
        bool okY = (dy < 1.0f);
        float3 lsX = okX ? ReconstructLS(nx, dx) : ls;
        float3 lsY = okY ? ReconstructLS(ny, dy) : ls;
        if (okX && !okY) lsY = lsX;
        if (okY && !okX) lsX = lsY;
        float2 dLdx = lsX.xy - ls.xy;
        float2 dLdy = lsY.xy - ls.xy;
        L = Vsm_SelectLevelCombined(ls.xy, Vsm_ZParams.zw, (uint)Vsm_Params.x, base,
                                    dLdx, dLdy, Vsm_LevelCenterExtent[0].w);
    }
    else
    {
        L = Vsm_SelectLevel(ls.xy, Vsm_ZParams.zw, (uint)Vsm_Params.x, base);
    }
    // Phase 2: フットプリントLOD時は対称±1帯 [L-1, L+1] をマーク。サンプラ(PS)のハードウェア導関数は
    // マーカー(CS有限差分)より1レベル細/粗にずれ得るので、マーク集合をサンプラの選ぶ任意レベルの上位集合に
    // して穴/スペックル(未割当ページ読取→CSM漏れ)を防ぐ。粗方向のみ帯 [L,L+1] は誤り（サンプラの細方向 L-1 を
    // 取りこぼす）。ガードは差分破棄(反対軸採用)のみでレベルは上げない=marker>sampler不一致を悪化させない。
    // 距離LOD(OFF)時は従来通り正確な L のみ（ビット一致=非回帰）。
    if (gUseFootprintLod != 0u)
    {
        int lo = max((int)L - 1, 0);
        int hi = min((int)L + 1, (int)gMaxMarkLevel);   // gMaxMarkLevel 超は VSM対象外→CSM
        [loop] for (int Lb = lo; Lb <= hi; ++Lb)
            MarkLevel(ls, (uint)Lb);
    }
    else
    {
        if (L > gMaxMarkLevel) return;   // 遠距離(高レベル)はVSMで賄わない→未割当=町側でCSMフォールバック
        MarkLevel(ls, L);
    }
}
