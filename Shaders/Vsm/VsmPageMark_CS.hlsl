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

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)Vsm_DepthDim.x || id.y >= (uint)Vsm_DepthDim.y) return;
    float d = Depth.Load(int3(id.xy, 0));
    if (d >= 1.0f) return;   // 空はページ不要

    // 深度 → world
    float2 uv = (float2(id.xy) + 0.5f) * Vsm_DepthDim.zw;
    float2 ndc = uv * 2.0f - 1.0f; ndc.y = -ndc.y;
    float4 wp = mul(float4(ndc, d, 1.0f), Vsm_InvViewProj);
    float3 P = wp.xyz / wp.w;

    // world → light 空間 → レベル/ページ（V5b: .z は pageWorld なので base=pw0*vppr）
    float3 ls = mul(float4(P, 1.0f), Vsm_LightView).xyz;
    float base = Vsm_LevelCenterExtent[0].z * Vsm_Params.z;   // level0 extent = pw0 * vppr
    uint L = Vsm_SelectLevel(ls.xy, Vsm_ZParams.zw, (uint)Vsm_Params.x, base);
    float2 uvp;
    uint2 vp = Vsm_VirtualPage(ls.xy, Vsm_LevelCenterExtent[L].xy,   // origin
                               Vsm_LevelCenterExtent[L].z, (uint)Vsm_Params.z, uvp);  // pageWorld
    uint idx = Vsm_PageTableIndex(L, vp, (uint)Vsm_Params.z);

    // V5b 永続キャッシュ: 要求フラグに「このスロットが今欲しい絶対ページ(packed)」を載せる。
    // Allocate 側が residentAP と比較し、別世界ページに巻いた(wrap)スロットだけ再割当/再描画する。
    // bit31=有効, [29:15]=apX(15bit signed), [14:0]=apY。窓内では 1スロット=1絶対ページなので
    // 同一スロットへ書く全画素は同値→非アトミック格納で安全（非キャッシュ経路も !=0 判定で従来通り）。
    float pw = Vsm_LevelCenterExtent[L].z;
    int apx = (int)floor(ls.x / max(pw, 1e-6f));
    int apy = (int)floor(ls.y / max(pw, 1e-6f));
    uint packed = 0x80000000u | (((uint)(apx & 0x7FFF)) << 15) | ((uint)(apy & 0x7FFF));
    Request[idx] = packed;
}
