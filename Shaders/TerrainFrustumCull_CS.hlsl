// GPU 視錐台カリング → 可視チャンクの DrawIndexed 引数を Append（カウンタ付き）
// 6 平面は C++ で VP 行から抽出（mul(worldPos, ViewProj) と整合）

cbuffer FrustumCullCB : register(b0)
{
	float4 Planes[6];
	float4 CameraPos;
	float4 CullParams; // x:ChunkCount, y:Lod0Start, z:Lod1Start
	float4x4 ViewProj;
	float4 HiZParams;  // x:enabled(0/1), y:width, z:height, w:mipCount
	float4 HiZTuning;  // x:nearDisableDist, y:depthBias, z:maxPixelRadius, w:unused
	float4 _padTo256[2];
};

struct ChunkInfo
{
	float4 aabbMin;
	float4 aabbMax;
	uint startIndex0;
	uint startIndex1;
	uint indexCount0;
	uint indexCount1;
	uint2 pad2;
};

struct DrawIndexedArgs
{
	uint indexCountPerInstance;
	uint instanceCount;
	uint startIndexLocation;
	int baseVertexLocation;
	uint startInstanceLocation;
};

StructuredBuffer<ChunkInfo> gChunks : register(t0);
Texture2D<float> gHiZ : register(t1);
RWStructuredBuffer<DrawIndexedArgs> gOutArgs : register(u0);
RWStructuredBuffer<uint> gCounter : register(u1);
struct ChunkDrawPayload
{
	uint chunkId;
	uint lod;
	float morph;
	float pad;
};
RWStructuredBuffer<ChunkDrawPayload> gOutPayload : register(u2);

float3 CornerFromAabb(float3 bmin, float3 bmax, int i)
{
	return float3(
		(i & 1) ? bmax.x : bmin.x,
		(i & 2) ? bmax.y : bmin.y,
		(i & 4) ? bmax.z : bmin.z);
}

// C++ の ExtractFrustumPlanes（r3±r0 等）と対。ある平面について 8 コーナーすべてが d>0 なら箱はその平面の「外側」半空間に完全にありカリング。
bool AabbOutsidePlane(float3 bmin, float3 bmax, float4 pl)
{
	bool allPositive = true;
	[unroll]
	for (int c = 0; c < 8; ++c)
	{
		float3 pt = CornerFromAabb(bmin, bmax, c);
		float d = dot(pl.xyz, pt) + pl.w;
		if (d <= 0.0)
		{
			allPositive = false;
			break;
		}
	}
	return allPositive;
}

bool IsAabbCulled(float3 bmin, float3 bmax)
{
	[unroll]
	for (int p = 0; p < 6; ++p)
	{
		if (AabbOutsidePlane(bmin, bmax, Planes[p]))
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

bool IsHiZOccluded(float3 bmin, float3 bmax, float dist)
{
	if (HiZParams.x < 0.5)
		return false;
	if (dist < HiZTuning.x) // 近距離は保守的に Hi-Z 無効
		return false;

	float2 uvMin = float2(1.0, 1.0);
	float2 uvMax = float2(0.0, 0.0);
	float nearestDepth = 1.0;
	[unroll]
	for (int c = 0; c < 8; ++c)
	{
		float3 pt = CornerFromAabb(bmin, bmax, c);
		float4 clip = mul(float4(pt, 1.0), ViewProj);
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

	// Forward-Z + max-depth pyramid: 対象深度がタイルの最大深度より十分大きいときのみ遮蔽とみなす。
	return (nearestDepth > hizDepth + HiZTuning.y);
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
	uint id = dtid.x;
	uint chunkCount = (uint)CullParams.x;
	if (id >= chunkCount)
		return;

	ChunkInfo ch = gChunks[id];
	float3 bmin = ch.aabbMin.xyz;
	float3 bmax = ch.aabbMax.xyz;
	if (IsAabbCulled(bmin, bmax))
		return;

	float3 center = 0.5 * (bmin + bmax);
	float dist = distance(CameraPos.xyz, center);
	if (IsHiZOccluded(bmin, bmax, dist))
		return;
	float lodF = saturate((dist - CullParams.y) / max(1.0, (CullParams.z - CullParams.y)));
	uint lod = (lodF >= 1.0 || CullParams.w > 0.5) ? 1 : 0;
	float morph = saturate(lodF);

	DrawIndexedArgs args;
	args.indexCountPerInstance = (lod == 0) ? ch.indexCount0 : ch.indexCount1;
	args.instanceCount = 1;
	args.startIndexLocation = (lod == 0) ? ch.startIndex0 : ch.startIndex1;
	args.baseVertexLocation = 0;
	args.startInstanceLocation = 0;

	uint slot;
	InterlockedAdd(gCounter[0], 1, slot);
	args.startInstanceLocation = slot;
	gOutArgs[slot] = args;
	ChunkDrawPayload p;
	p.chunkId = id;
	p.lod = lod;
	p.morph = morph;
	p.pad = 0.0;
	gOutPayload[slot] = p;
}
