#include "TerrainGenerator.h"
#include "SharedStruct.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"
#include "Engine.h"
#include "EXRLoader.h"
#include "DebugLog.h"
#include <DirectXTex.h>
#include <DirectXMath.h>
#include <vector>
#include <algorithm>
#include <cassert>

#pragma comment(lib, "DirectXTex.lib")

using namespace DirectX;

namespace
{
	/// セル単位チャンクの一辺長（大きすぎるとカリングが粗い／小さすぎると境界が目立つ。8m セルで 128→約 1km）
	constexpr UINT kTerrainChunkCellsPerAxis = 128u;

	void LogTerrainMetrics(const TerrainGenerateResult& out, float cellSpacing, float maxHeight)
	{
		if (out.HeightData.empty() || out.GridWidth == 0 || out.GridDepth == 0)
			return;

		float minNorm = out.HeightData[0];
		float maxNorm = out.HeightData[0];
		for (float h : out.HeightData)
		{
			minNorm = (std::min)(minNorm, h);
			maxNorm = (std::max)(maxNorm, h);
		}

		const float widthM = static_cast<float>(out.GridWidth) * cellSpacing;
		const float depthM = static_cast<float>(out.GridDepth) * cellSpacing;
		const float minHeightM = minNorm * maxHeight;
		const float maxHeightM = maxNorm * maxHeight;
		const UINT k = kTerrainChunkCellsPerAxis;
		const UINT chunksX = (out.GridWidth + k - 1u) / k;
		const UINT chunksZ = (out.GridDepth + k - 1u) / k;

		DebugLog(
			"[Terrain] Grid=%ux%u cell=%.2fm world=%.1fm x %.1fm chunks=%ux%u(%zu) chunkCell=%u heightNorm=[%.4f..%.4f] heightM=[%.2f..%.2f]\n",
			out.GridWidth, out.GridDepth,
			cellSpacing,
			widthM, depthM,
			chunksX, chunksZ, out.Chunks.size(), k,
			minNorm, maxNorm,
			minHeightM, maxHeightM);
	}

	float Clamp01(float v)
	{
		return (std::max)(0.0f, (std::min)(v, 1.0f));
	}

	float SampleHeight(const uint8_t* pixels, UINT width, UINT height, size_t rowPitch, UINT i, UINT j)
	{
		UINT x = (std::min)(i, width > 0 ? width - 1u : 0u);
		UINT y = (std::min)(j, height > 0 ? height - 1u : 0u);
		const uint8_t* p = pixels + y * rowPitch + x * 4;
		float r = p[0] / 255.0f;
		float g = p[1] / 255.0f;
		float b = p[2] / 255.0f;
		return (r + g + b) / 3.0f;
	}

	void ComputeNormal(UINT i, UINT j, UINT w, UINT h, const std::vector<float>& heightData,
		float cellSpacing, float maxHeight, XMFLOAT3& outN)
	{
		float hC = heightData[i + j * w];
		float hL = (i > 0) ? heightData[(i - 1) + j * w] : hC;
		float hR = (i + 1 < w) ? heightData[(i + 1) + j * w] : hC;
		float hD = (j > 0) ? heightData[i + (j - 1) * w] : hC;
		float hU = (j + 1 < h) ? heightData[i + (j + 1) * w] : hC;

		float dx = 2.0f * cellSpacing;
		float dz = 2.0f * cellSpacing;
		XMVECTOR v1 = XMVectorSet(dx, (hR - hL) * maxHeight, 0.0f, 0.0f);
		XMVECTOR v2 = XMVectorSet(0.0f, (hU - hD) * maxHeight, dz, 0.0f);
		XMVECTOR n = XMVector3Cross(v1, v2);
		n = XMVector3Normalize(n);
		XMStoreFloat3(&outN, n);
	}

		void SmoothHeightMap(std::vector<float>& heightData, UINT w, UINT h, float blend)
	{
		if (heightData.empty() || w == 0 || h == 0)
			return;

		blend = Clamp01(blend);
		if (blend <= 0.0f)
			return;

		std::vector<float> src = heightData;
		for (UINT j = 0; j < h; ++j)
		{
			for (UINT i = 0; i < w; ++i)
			{
				float sum = 0.0f;
				int count = 0;
				for (int dj = -1; dj <= 1; ++dj)
				{
					int y = (std::max)(0, (std::min)(static_cast<int>(j) + dj, static_cast<int>(h) - 1));
					for (int di = -1; di <= 1; ++di)
					{
						int x = (std::max)(0, (std::min)(static_cast<int>(i) + di, static_cast<int>(w) - 1));
						sum += src[x + y * w];
						++count;
					}
				}
				float blurred = (count > 0) ? (sum / static_cast<float>(count)) : src[i + j * w];
				heightData[i + j * w] = src[i + j * w] * (1.0f - blend) + blurred * blend;
			}
		}
	}

	/// 頂点グリッドとチャンク順インデックス、チャンク AABB を構築
	bool BuildTerrainMeshFromHeightGrid(
		const std::vector<float>& heightData,
		UINT w,
		UINT h,
		float cellSpacing,
		float maxHeight,
		TerrainGenerateResult& out)
	{
		if (heightData.size() < static_cast<size_t>(w) * h || w == 0 || h == 0)
			return false;

		const UINT k = kTerrainChunkCellsPerAxis;
		const UINT chunksX = (w + k - 1u) / k;
		const UINT chunksZ = (h + k - 1u) / k;

		const float halfWidth = static_cast<float>(w) * cellSpacing * 0.5f;
		const float halfDepth = static_cast<float>(h) * cellSpacing * 0.5f;

		UINT numVerts = (w + 1u) * (h + 1u);
		std::vector<Vertex> vertices(numVerts);

		for (UINT j = 0; j <= h; j++)
		{
			for (UINT i = 0; i <= w; i++)
			{
				UINT ii = (std::min)(i, w - 1u);
				UINT jj = (std::min)(j, h - 1u);
				float height = heightData[ii + jj * w] * maxHeight;

				Vertex v = {};
				v.Position = XMFLOAT3(
					static_cast<float>(i) * cellSpacing - halfWidth,
					height,
					static_cast<float>(j) * cellSpacing - halfDepth
				);
				v.UV = XMFLOAT2(
					(w > 0u) ? static_cast<float>(i) / static_cast<float>(w) : 0.0f,
					(h > 0u) ? static_cast<float>(j) / static_cast<float>(h) : 0.0f
				);
				ComputeNormal(ii, jj, w, h, heightData, cellSpacing, maxHeight, v.Normal);
				v.Tangent = XMFLOAT3(1.0f, 0.0f, 0.0f);
				v.Color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
				v.BoneIndex[0] = v.BoneIndex[1] = v.BoneIndex[2] = v.BoneIndex[3] = 0;
				v.BoneWeight[0] = 1.0f;
				v.BoneWeight[1] = v.BoneWeight[2] = v.BoneWeight[3] = 0.0f;

				vertices[i + j * (w + 1u)] = v;
			}
		}

		std::vector<uint32_t> indicesLod0;
		std::vector<uint32_t> indicesLod1;
		indicesLod0.reserve(static_cast<size_t>(w) * h * 6u);
		indicesLod1.reserve(static_cast<size_t>(w) * h * 6u / 4u);
		out.Chunks.clear();
		out.Chunks.reserve(static_cast<size_t>(chunksX) * chunksZ);

		for (UINT cz = 0; cz < chunksZ; ++cz)
		{
			for (UINT cx = 0; cx < chunksX; ++cx)
			{
				const UINT i0 = cx * k;
				const UINT j0 = cz * k;
				const UINT i1_cell = (std::min)((cx + 1u) * k, w);
				const UINT j1_cell = (std::min)((cz + 1u) * k, h);

				TerrainChunkDesc chunk = {};
				chunk.ChunkId = static_cast<uint32_t>(out.Chunks.size());
				chunk.MaxLod = kTerrainLodCount - 1u;
				chunk.StartIndex[0] = static_cast<uint32_t>(indicesLod0.size());
				chunk.StartIndex[1] = static_cast<uint32_t>(indicesLod1.size());

				auto emitLod = [&](std::vector<uint32_t>& dst, UINT step)
				{
					if (step == 0) step = 1;
					const UINT endI = i1_cell - ((i1_cell - i0) % step);
					const UINT endJ = j1_cell - ((j1_cell - j0) % step);
					for (UINT j = j0; j + step <= endJ; j += step)
					{
						for (UINT i = i0; i + step <= endI; i += step)
						{
							const UINT v00 = i + j * (w + 1u);
							const UINT v10 = (i + step) + j * (w + 1u);
							const UINT v01 = i + (j + step) * (w + 1u);
							const UINT v11 = (i + step) + (j + step) * (w + 1u);
							dst.push_back(v00);
							dst.push_back(v10);
							dst.push_back(v01);
							dst.push_back(v10);
							dst.push_back(v11);
							dst.push_back(v01);
						}
					}
				};
				emitLod(indicesLod0, 1u);
				emitLod(indicesLod1, 2u);

				chunk.IndexCount[0] = static_cast<uint32_t>(indicesLod0.size()) - chunk.StartIndex[0];
				chunk.IndexCount[1] = static_cast<uint32_t>(indicesLod1.size()) - chunk.StartIndex[1];

				bool haveBounds = false;
				for (UINT vj = j0; vj <= j1_cell; ++vj)
				{
					for (UINT vi = i0; vi <= i1_cell; ++vi)
					{
						const XMFLOAT3& p = vertices[vi + vj * (w + 1u)].Position;
						if (!haveBounds)
						{
							chunk.LocalBounds.Min = chunk.LocalBounds.Max = p;
							haveBounds = true;
						}
						else
						{
							chunk.LocalBounds.Min.x = (std::min)(chunk.LocalBounds.Min.x, p.x);
							chunk.LocalBounds.Min.y = (std::min)(chunk.LocalBounds.Min.y, p.y);
							chunk.LocalBounds.Min.z = (std::min)(chunk.LocalBounds.Min.z, p.z);
							chunk.LocalBounds.Max.x = (std::max)(chunk.LocalBounds.Max.x, p.x);
							chunk.LocalBounds.Max.y = (std::max)(chunk.LocalBounds.Max.y, p.y);
							chunk.LocalBounds.Max.z = (std::max)(chunk.LocalBounds.Max.z, p.z);
						}
					}
				}

				if (chunk.IndexCount[0] > 0u && haveBounds)
					out.Chunks.push_back(chunk);
			}
		}

		// 単一IBに LOD0, LOD1 を連結して格納（StartIndex は連結後位置へ補正）
		const uint32_t lod0Count = static_cast<uint32_t>(indicesLod0.size());
		std::vector<uint32_t> indices;
		indices.reserve(indicesLod0.size() + indicesLod1.size());
		indices.insert(indices.end(), indicesLod0.begin(), indicesLod0.end());
		indices.insert(indices.end(), indicesLod1.begin(), indicesLod1.end());
		for (auto& ch : out.Chunks)
			ch.StartIndex[1] += lod0Count;

		out.IndexCount = static_cast<uint32_t>(indices.size());
		out.pVB = new VertexBuffer(numVerts * sizeof(Vertex), sizeof(Vertex), vertices.data());
		out.pIB = new IndexBuffer(indices.size() * sizeof(uint32_t), indices.data());
		if (!out.pVB->IsValid() || !out.pIB->IsValid())
		{
			delete out.pVB;
			delete out.pIB;
			out.pVB = nullptr;
			out.pIB = nullptr;
			out.IndexCount = 0;
			out.Chunks.clear();
			return false;
		}
		return true;
	}
}

bool TerrainGenerator_GenerateFromFile(
	const wchar_t*     heightmapPath,
	float              cellSpacing,
	float              maxHeight,
	TerrainGenerateResult& out)
{
	DirectX::TexMetadata meta = {};
	DirectX::ScratchImage scratch = {};
	HRESULT hr = DirectX::LoadFromWICFile(heightmapPath, DirectX::WIC_FLAGS_NONE, &meta, scratch);
	if (FAILED(hr))
		return false;

	const DirectX::Image* img = scratch.GetImage(0, 0, 0);
	if (!img || !img->pixels)
		return false;

	UINT w = static_cast<UINT>(img->width);
	UINT h = static_cast<UINT>(img->height);
	if (w == 0 || h == 0)
		return false;

	size_t rowPitch = img->rowPitch;
	// HeightData: 1 sample per pixel (width * height)
	out.HeightData.resize(w * h);
	for (UINT j = 0; j < h; j++)
		for (UINT i = 0; i < w; i++)
			out.HeightData[i + j * w] = SampleHeight(img->pixels, w, h, rowPitch, i, j);
	SmoothHeightMap(out.HeightData, w, h, 0.25f);

	out.GridWidth = w;
	out.GridDepth = h;

	if (!BuildTerrainMeshFromHeightGrid(out.HeightData, w, h, cellSpacing, maxHeight, out))
		return false;
	LogTerrainMetrics(out, cellSpacing, maxHeight);
	return true;
}

bool TerrainGenerator_GenerateFromExr(
	const char*        pathUtf8,
	float              cellSpacing,
	float              maxHeight,
	TerrainGenerateResult& out)
{
	int w = 0, h = 0;
	float* rgba = nullptr;
	if (!LoadEXRToFloatRgba(pathUtf8, &w, &h, &rgba) || w <= 0 || h <= 0 || !rgba)
		return false;

	const int total = w * h;
	out.HeightData.resize(static_cast<size_t>(total));
	for (int i = 0; i < total; i++)
	{
		const float r = rgba[i * 4 + 0];
		float heightNorm = Clamp01(r);
		heightNorm = heightNorm * 0.85f;
		out.HeightData[static_cast<size_t>(i)] = Clamp01(heightNorm);
	}
	free(rgba);

	out.GridWidth = static_cast<UINT>(w);
	out.GridDepth = static_cast<UINT>(h);
	const UINT uw = out.GridWidth, uh = out.GridDepth;
	SmoothHeightMap(out.HeightData, uw, uh, 0.35f);

	if (!BuildTerrainMeshFromHeightGrid(out.HeightData, uw, uh, cellSpacing, maxHeight, out))
		return false;
	LogTerrainMetrics(out, cellSpacing, maxHeight);
	return true;
}

void TerrainGenerator_ReleaseResult(TerrainGenerateResult& result)
{
	result.HeightData.clear();
	result.Chunks.clear();
	delete result.pVB;
	delete result.pIB;
	result.pVB = nullptr;
	result.pIB = nullptr;
	result.IndexCount = result.GridWidth = result.GridDepth = 0;
}
