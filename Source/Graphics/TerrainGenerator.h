#pragma once

#include <vector>
#include <cstdint>

#include "ModelBounds.h"

struct Vertex;
class VertexBuffer;
class IndexBuffer;

static constexpr uint32_t kTerrainLodCount = 2u;

/// 共有 IB 上の連続範囲＋ローカル AABB（視錐台／将来 Hi-Z 用）
struct TerrainChunkDesc
{
	uint32_t    StartIndex[kTerrainLodCount] = {};
	uint32_t    IndexCount[kTerrainLodCount] = {};
	ModelBounds LocalBounds{};
	uint32_t    MaxLod = kTerrainLodCount - 1u;
	uint32_t    ChunkId = 0;
};

struct TerrainGenerateResult
{
	std::vector<float> HeightData;
	VertexBuffer*      pVB = nullptr;
	IndexBuffer*       pIB = nullptr;
	uint32_t           IndexCount = 0;
	uint32_t           GridWidth  = 0;
	uint32_t           GridDepth  = 0;
	std::vector<TerrainChunkDesc> Chunks;
};

// ハイトマップ画像（PNG等 WIC）から地形メッシュと高さデータを生成。
bool TerrainGenerator_GenerateFromFile(
	const wchar_t*     heightmapPath,
	float              cellSpacing,
	float              maxHeight,
	TerrainGenerateResult& out);

// EXR（float）から高さを読み地形メッシュを生成。pathUtf8 は UTF-8 パス。
bool TerrainGenerator_GenerateFromExr(
	const char*        pathUtf8,
	float              cellSpacing,
	float              maxHeight,
	TerrainGenerateResult& out);

void TerrainGenerator_ReleaseResult(TerrainGenerateResult& result);
