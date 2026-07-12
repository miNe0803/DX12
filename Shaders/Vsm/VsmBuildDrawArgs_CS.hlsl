// ============================================================
//  VsmBuildDrawArgs_CS.hlsl — VSM V3c-m2e: submesh バッチ毎に間接描画引数を生成。
//  各バッチ b（あるモデルの1 submesh）に対し、InstanceCount=そのモデルのペア数、
//  StartInstance=そのモデルの InstancePairs 領域先頭。ExecuteIndirect が per-submesh に発行。
//  レビュー C1: InstanceCount を実書込み可能領域 [base, kMaxPairs) にクランプ（OOB 読み防止）。
// ============================================================
struct SubmeshGeo { uint indexCount; uint startIndex; uint baseVertex; uint modelId; };
struct DrawArgs   { uint indexCountPerInstance; uint instanceCount; uint startIndex; uint baseVertex; uint startInstance; };

StructuredBuffer<SubmeshGeo> SubmeshGeoTable : register(t0);   // root SRV
RWStructuredBuffer<uint>     PairBase  : register(u0);         // 読取（UAV 状態のまま）
RWStructuredBuffer<uint>     PairCount : register(u1);
RWStructuredBuffer<DrawArgs> DrawArgsOut : register(u2);

cbuffer ArgCb : register(b0)
{
    uint gBatchCount;
    uint gMaxPairs;
    uint _a;
    uint _b;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint b = id.x;
    if (b >= gBatchCount) return;
    SubmeshGeo s = SubmeshGeoTable[b];
    uint m = s.modelId;
    uint base = PairBase[m];
    uint cnt = (base >= gMaxPairs) ? 0u : min(PairCount[m], gMaxPairs - base);   // C1 クランプ

    DrawArgs a;
    a.indexCountPerInstance = s.indexCount;
    a.instanceCount         = cnt;
    a.startIndex            = s.startIndex;
    a.baseVertex            = s.baseVertex;
    a.startInstance         = base;
    DrawArgsOut[b] = a;
}
