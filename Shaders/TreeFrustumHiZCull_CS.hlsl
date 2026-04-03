// Tree GPU cull: frustum + Hi-Z occlusion -> visible-index compaction + 1 indirect command per (species,lod,part).
// Goal: avoid "1 tree = 1 command" (TDR risk). We output:
// - visible instance indices into gVisibleIndex[batch][slot]
// - a single DrawIndexedInstanced command in gOutCmd[batch][0] with instanceCount = visibleCount

cbuffer TreeCullCB : register(b0)
{
	float4 CameraPos;
	float4 Params;    // x:InstanceCount
	// 視錐: CPU が Terrain と同じ V*P から抽出（TerrainFrustumCull と整合）。
	float4 FrustumPlanes[6];
	// Hi-Z: SceneConstants と同一の View/Proj → TreeIndirectVS の mul(mul(pos,View),Proj) と一致。
	float4x4 View;
	float4x4 Proj;
	float4 HiZParams; // x:enabled(0/1), y:width, z:height, w:mipCount
	float4 HiZTuning; // x:nearDisableDist, y:depthBias, z:maxPixelRadius, w:unused
	uint4 IndexCountsTrunk;    // x:lod0, y:lod1, z:lod2
	uint4 IndexCountsLeaves;   // x:lod0, y:lod1, z:lod2
	uint4 IndexCountsBranches; // x:lod0, y:lod1, z:lod2
	float4 LodParams; // x:lod1Start (m), y:lod2Start (m), z:maxDrawDist (m, 0=unlimited), w:unused
	float4 _padTo512[10];
};

struct DrawIndexedArgs
{
	uint indexCountPerInstance;
	uint instanceCount;
	uint startIndexLocation;
	int baseVertexLocation;
	uint startInstanceLocation;
};

struct TreeInfo
{
	float4 centerRadius; // xyz center, w radius
	row_major float4x4 worldRow;
	uint2 instanceGpuVA; // 64-bit GPUVA (low/high)
	uint speciesIndex;
	uint _pad0;
	uint2 _pad64_a; // C++ UINT64 _pad1
	uint2 _pad64_b; // C++ UINT64 _pad2（StructuredBuffer stride 112 と一致必須）
};

StructuredBuffer<TreeInfo> gTrees : register(t0);
Texture2D<float> gHiZ : register(t1);

// C++ の D3D12_DRAW_INDEXED_ARGUMENTS と同一 20 バイト（ExecuteIndirect / UAV stride と一致必須）
struct TreeIndirectCmd
{
	DrawIndexedArgs draw;
};

static const uint kSpeciesCount = 3;
static const uint kLodCount = 3;
static const uint kPartCount = 3;
static const uint kBatchCount = kSpeciesCount * kLodCount * kPartCount; // 27

RWStructuredBuffer<uint> gVisibleIndex[kBatchCount] : register(u0);
RWStructuredBuffer<TreeIndirectCmd> gOutCmd[kBatchCount] : register(u27);
RWStructuredBuffer<uint> gCounter[kBatchCount] : register(u54);

bool TryAllocVisibleSlotBounded(uint batch, uint limit, out uint outSlot)
{
	// One InterlockedAdd per thread — avoids CAS spin under heavy contention (same batch, 100k+ threads).
	uint slot;
	InterlockedAdd(gCounter[batch][0], 1, slot);
	if (slot >= limit)
	{
		uint unused;
		InterlockedAdd(gCounter[batch][0], 0xFFFFFFFFu, unused);
		outSlot = 0;
		return false;
	}
	outSlot = slot;
	return true;
}

uint BatchIndex(uint species, uint lod, uint part)
{
	return (species * kLodCount + lod) * kPartCount + part;
}

uint IndexCountByPartLod(uint part, uint lod)
{
	uint4 ic = (part == 0) ? IndexCountsTrunk : ((part == 1) ? IndexCountsLeaves : IndexCountsBranches);
	return (lod == 0) ? ic.x : ((lod == 1) ? ic.y : ic.z);
}

float3 CornerFromSphere(float3 c, float r, int i)
{
	return c + float3(
		(i & 1) ? r : -r,
		(i & 2) ? r : -r,
		(i & 4) ? r : -r);
}

bool SphereOutsidePlane(float3 c, float r, float4 pl)
{
	// TerrainFrustumCull_CS と同じ未正規化平面 ax+by+cz+d=0。符号距離（世界単位）は d/|n|。
	// 球が平面の「外側」半空間に完全にある ⇔ 中心からの符号距離が半径より大きい ⇔ d > r*|n|。
	float3 n = pl.xyz;
	float len = length(n);
	if (len < 1e-5)
		return false;
	float d = dot(n, c) + pl.w;
	return (d > r * len);
}

bool IsCulled(float3 c, float r, float4 planes[6])
{
	[unroll]
	for (int p = 0; p < 6; ++p)
	{
		if (SphereOutsidePlane(c, r, planes[p]))
			return true;
	}
	return false;
}

float SampleHiZMax5(float2 uv, float mip, float2 mipSize)
{
	float2 uvMin = clamp(uv - 0.5 / mipSize, float2(0.0, 0.0), float2(1.0, 1.0));
	float2 uvMax = clamp(uv + 0.5 / mipSize, float2(0.0, 0.0), float2(1.0, 1.0));
	int2 p00 = int2(clamp(uvMin * mipSize, float2(0.0, 0.0), mipSize - 1.0));
	int2 p10 = int2(clamp(float2(uvMax.x, uvMin.y) * mipSize, float2(0.0, 0.0), mipSize - 1.0));
	int2 p01 = int2(clamp(float2(uvMin.x, uvMax.y) * mipSize, float2(0.0, 0.0), mipSize - 1.0));
	int2 p11 = int2(clamp(uvMax * mipSize, float2(0.0, 0.0), mipSize - 1.0));
	int2 pc = int2(clamp(uv * mipSize, float2(0.0, 0.0), mipSize - 1.0));
	float z00 = gHiZ.Load(int3(p00, (int)mip));
	float z10 = gHiZ.Load(int3(p10, (int)mip));
	float z01 = gHiZ.Load(int3(p01, (int)mip));
	float z11 = gHiZ.Load(int3(p11, (int)mip));
	float zc = gHiZ.Load(int3(pc, (int)mip));
	return max(max(max(z00, z10), max(z01, z11)), zc);
}

bool IsHiZOccluded(float3 c, float r, float dist)
{
	if (HiZParams.x < 0.5)
		return false;
	if (dist < HiZTuning.x)
		return false;

	float2 uvMin = float2(1.0, 1.0);
	float2 uvMax = float2(0.0, 0.0);
	float nearestDepth = 1.0;
	[unroll]
	for (int i = 0; i < 8; ++i)
	{
		float3 pt = CornerFromSphere(c, r, i);
		float4 clip = mul(mul(float4(pt, 1.0), View), Proj);
		if (clip.w <= 1e-5)
			return false;
		float2 uv = clip.xy / clip.w * 0.5 + 0.5;
		uvMin = min(uvMin, uv);
		uvMax = max(uvMax, uv);
		nearestDepth = min(nearestDepth, saturate(clip.z / clip.w));
	}

	if (uvMax.x < 0.0 || uvMin.x > 1.0 || uvMax.y < 0.0 || uvMin.y > 1.0)
		return false;

	uvMin = clamp(uvMin, float2(0.0, 0.0), float2(1.0, 1.0));
	uvMax = clamp(uvMax, float2(0.0, 0.0), float2(1.0, 1.0));
	if (uvMax.x <= uvMin.x || uvMax.y <= uvMin.y)
		return false;

	float2 rectPixels = (uvMax - uvMin) * HiZParams.yz;
	float pixelRadius = 0.5 * max(rectPixels.x, rectPixels.y);
	if (pixelRadius > HiZTuning.z)
		return false;
	float mip = clamp(floor(log2(max(1.0, pixelRadius))), 0.0, HiZParams.w - 1.0);
	float2 mipSize = max(float2(1.0, 1.0), HiZParams.yz / exp2(mip));
	float2 uvCenter = 0.5 * (uvMin + uvMax);
	float hizDepth = SampleHiZMax5(uvCenter, mip, mipSize);

	return (nearestDepth > hizDepth + HiZTuning.y);
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	uint id = dtid.x;
	uint n = (uint)Params.x;
	if (id >= n)
		return;

	TreeInfo t = gTrees[id];
	float3 c = t.centerRadius.xyz;
	float r = t.centerRadius.w;

	// XZ 距離（地上カメラでも横方向の遠さで LOD を分ける）
	float2 dXZ = CameraPos.xz - c.xz;
	float dist = length(dXZ);

	// 最大描画距離カリング: LodParams.z > 0 のとき、それ以遠のインスタンスは一切描画しない
	if (LodParams.z > 0.0 && dist >= LodParams.z)
		return;

	if (IsCulled(c, r, FrustumPlanes))
		return;
	if (IsHiZOccluded(c, r, dist))
		return;

	uint slot;
	uint lod = (dist >= LodParams.y) ? 2u : ((dist >= LodParams.x) ? 1u : 0u);

	const uint species = min(t.speciesIndex, kSpeciesCount - 1);

	// LOD1/LOD2: part0 only (merged mesh, no part-split)
	if (lod >= 1u)
	{
		const uint batch = BatchIndex(species, lod, 0u);
		if (!TryAllocVisibleSlotBounded(batch, n, slot))
			return;
		gVisibleIndex[batch][slot] = id;
		return;
	}

	// LOD0
	[unroll]
	for (uint part = 0; part < kPartCount; ++part)
	{
		const uint idxCount = IndexCountByPartLod(part, lod);
		if (idxCount == 0)
			continue;

		// Merged FBX: all parts have the same index count → draw part0 only to avoid 3x overdraw
		const uint ic0 = IndexCountByPartLod(0u, lod);
		const uint ic1 = IndexCountByPartLod(1u, lod);
		const uint ic2 = IndexCountByPartLod(2u, lod);
		if (ic0 > 0u && ic0 == ic1 && ic1 == ic2 && part != 0u)
			continue;

		const uint batch = BatchIndex(species, lod, part);
		// Visible index buffer capacity = instanceCount (one index per potential instance).
		const uint limit = n;
		// CRITICAL: visibleCount must never exceed written visibleIndex count.
		if (!TryAllocVisibleSlotBounded(batch, limit, slot))
			continue;

		// Compact visible instance indices.
		gVisibleIndex[batch][slot] = id;

		// Ensure the per-batch indirect command has the correct indexCount (it is constant for the batch).
		// NOTE: indexCount/start* are set from CPU side in DispatchCull.
		(void)idxCount;
	}
}

