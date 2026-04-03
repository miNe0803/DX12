#pragma once

#include "Core/ModelBounds.h"
#include "SharedStruct.h"
#include <cstdint>
#include <string>
#include <vector>

/// GPU 向け静的メッシュ。ExecuteIndirect / 材質テーブルは呼び出し側で species×LOD×part に束ねる。
/// .tmesh v1: リトルエンディアン、先頭 magic = 0x31484D54 ("TMH1")。頂点は `Vertex`（84B、`SharedStruct.h` と同一）。
namespace TreeMeshAsset
{
	/// @return true かつ out が非空なら成功。失敗時は out をクリア。
	bool LoadTmeshV1(const wchar_t* resolvedPath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
		ModelBounds& outBounds);

	/// FBX/OBJ 等を AssimpLoader 経由でロードし、全サブメッシュをマージして返す。
	bool LoadFbxMerged(const wchar_t* resolvedPath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
		ModelBounds& outBounds);

	struct PartData {
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		ModelBounds bounds{};
		std::string materialName;
		std::wstring diffuseMap, normalMap, metallicMap, roughnessMap, opacityMap;
	};
	bool LoadFbxParts(const wchar_t* resolvedPath, std::vector<PartData>& outParts, ModelBounds& outMergedBounds);

	/// ファイルが無い／壊れている場合のフォールバック（種0・LOD0 用の簡易幹＋円錐キャノピ）。
	void GenerateProceduralSpecies0Lod0(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
		ModelBounds& outBounds);

	/// 既定パスを試し、ダメなら手続きメッシュ。outSourceLabel はデバッグ表示用（ファイル名 or "procedural"）。
	bool LoadSpecies0Lod0Mesh(const std::wstring& resolvedTmeshPath, std::vector<Vertex>& outVertices,
		std::vector<uint32_t>& outIndices, ModelBounds& outBounds, std::wstring& outSourceLabel);
}
