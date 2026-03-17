#pragma once

#include <vector>
#include <cstdint>

struct Vertex;
class VertexBuffer;
class IndexBuffer;

struct TerrainGenerateResult
{
	std::vector<float> HeightData;
	VertexBuffer*      pVB = nullptr;
	IndexBuffer*       pIB = nullptr;
	uint32_t           IndexCount = 0;
	uint32_t           GridWidth  = 0;
	uint32_t           GridDepth  = 0;
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
