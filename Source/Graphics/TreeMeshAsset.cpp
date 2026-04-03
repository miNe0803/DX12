#include "TreeMeshAsset.h"

#include "AssimpLoader.h"
#include "DebugLog.h"

#include <algorithm>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace DirectX;

namespace fs = std::filesystem;

namespace
{
#pragma pack(push, 1)
	struct TmeshHeaderV1
	{
		uint32_t magic;     // '1HMT' リトルエンディアン = "TMH1"
		uint32_t version;   // 1
		uint32_t vertexCount;
		uint32_t indexCount;
		float aabbMin[3];
		float aabbMax[3];
	};
#pragma pack(pop)

	static_assert(sizeof(TmeshHeaderV1) == 40, "TmeshHeaderV1 size");

	constexpr uint32_t kMagicTmh1 = 0x31484D54u; // "TMH1"

	inline void ExpandBounds(ModelBounds& b, const XMFLOAT3& p)
	{
		b.Min.x = (std::min)(b.Min.x, p.x);
		b.Min.y = (std::min)(b.Min.y, p.y);
		b.Min.z = (std::min)(b.Min.z, p.z);
		b.Max.x = (std::max)(b.Max.x, p.x);
		b.Max.y = (std::max)(b.Max.y, p.y);
		b.Max.z = (std::max)(b.Max.z, p.z);
	}

	ModelBounds BoundsFromVertices(const std::vector<Vertex>& v)
	{
		ModelBounds b{};
		if (v.empty())
			return b;
		b.Min = b.Max = v[0].Position;
		for (size_t i = 1; i < v.size(); ++i)
			ExpandBounds(b, v[i].Position);
		return b;
	}

	inline Vertex MakeVert(XMFLOAT3 pos, XMFLOAT3 n, XMFLOAT2 uv, XMFLOAT3 tan)
	{
		Vertex o{};
		o.Position = pos;
		o.Normal = n;
		o.UV = uv;
		o.Tangent = tan;
		o.Color = XMFLOAT4(1.f, 1.f, 1.f, 1.f);
		for (int k = 0; k < 4; ++k)
		{
			o.BoneIndex[k] = 0;
			o.BoneWeight[k] = 0.f;
		}
		return o;
	}

	void AddCylinder(std::vector<Vertex>& verts, std::vector<uint32_t>& idx, float y0, float y1, float r, int seg)
	{
		const int base = static_cast<int>(verts.size());
		const float twoPi = XM_2PI;
		for (int i = 0; i < seg; ++i)
		{
			const float t = (static_cast<float>(i) / static_cast<float>(seg)) * twoPi;
			const float c = cosf(t), s = sinf(t);
			const float x = r * c, z = r * s;
			XMFLOAT3 n{ c, 0.f, s };
			XMFLOAT3 tan{ -s, 0.f, c };
			verts.push_back(MakeVert({ x, y0, z }, n, { static_cast<float>(i) / seg, 0.f }, tan));
		}
		for (int i = 0; i < seg; ++i)
		{
			const float t = (static_cast<float>(i) / static_cast<float>(seg)) * twoPi;
			const float c = cosf(t), s = sinf(t);
			const float x = r * c, z = r * s;
			XMFLOAT3 n{ c, 0.f, s };
			XMFLOAT3 tan{ -s, 0.f, c };
			verts.push_back(MakeVert({ x, y1, z }, n, { static_cast<float>(i) / seg, 1.f }, tan));
		}
		for (int i = 0; i < seg; ++i)
		{
			const int i0 = base + i;
			const int i1 = base + ((i + 1) % seg);
			const int i2 = base + seg + i;
			const int i3 = base + seg + ((i + 1) % seg);
			idx.push_back(static_cast<uint32_t>(i0));
			idx.push_back(static_cast<uint32_t>(i2));
			idx.push_back(static_cast<uint32_t>(i1));
			idx.push_back(static_cast<uint32_t>(i1));
			idx.push_back(static_cast<uint32_t>(i2));
			idx.push_back(static_cast<uint32_t>(i3));
		}
	}

	void AddCone(std::vector<Vertex>& verts, std::vector<uint32_t>& idx, float yBase, float yTip, float rBase, int seg)
	{
		const int tipIx = static_cast<int>(verts.size());
		verts.push_back(MakeVert({ 0.f, yTip, 0.f }, { 0.f, 1.f, 0.f }, { 0.5f, 0.f }, { 1.f, 0.f, 0.f }));
		const int ringStart = static_cast<int>(verts.size());
		const float twoPi = XM_2PI;
		for (int i = 0; i < seg; ++i)
		{
			const float t = (static_cast<float>(i) / static_cast<float>(seg)) * twoPi;
			const float c = cosf(t), s = sinf(t);
			const float x = rBase * c, z = rBase * s;
			XMFLOAT3 n{ c * 0.7f, 0.5f, s * 0.7f };
			const float il = 1.f / sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
			n.x *= il;
			n.y *= il;
			n.z *= il;
			XMFLOAT3 tan{ -s, 0.f, c };
			verts.push_back(MakeVert({ x, yBase, z }, n, { static_cast<float>(i) / seg, 1.f }, tan));
		}
		for (int i = 0; i < seg; ++i)
		{
			const int b0 = ringStart + i;
			const int b1 = ringStart + ((i + 1) % seg);
			idx.push_back(static_cast<uint32_t>(tipIx));
			idx.push_back(static_cast<uint32_t>(b0));
			idx.push_back(static_cast<uint32_t>(b1));
		}
	}
}

namespace TreeMeshAsset
{

bool LoadTmeshV1(const wchar_t* resolvedPath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
	ModelBounds& outBounds)
{
	outVertices.clear();
	outIndices.clear();
	outBounds = {};

	if (!resolvedPath || !resolvedPath[0])
		return false;
	if (!fs::exists(resolvedPath))
		return false;

	FILE* f = nullptr;
	if (_wfopen_s(&f, resolvedPath, L"rb") != 0 || !f)
		return false;

	TmeshHeaderV1 hdr{};
	if (fread(&hdr, sizeof(hdr), 1, f) != 1)
	{
		fclose(f);
		return false;
	}
	if (hdr.magic != kMagicTmh1 || hdr.version != 1u || hdr.vertexCount == 0u || hdr.indexCount < 3u)
	{
		fclose(f);
		DebugLog("[TreeMeshAsset] invalid header: %ls\n", resolvedPath);
		return false;
	}
	const uint64_t vertBytes = static_cast<uint64_t>(hdr.vertexCount) * sizeof(Vertex);
	const uint64_t idxBytes = static_cast<uint64_t>(hdr.indexCount) * sizeof(uint32_t);
	if (vertBytes > 256ull * 1024 * 1024 || idxBytes > 256ull * 1024 * 1024)
	{
		fclose(f);
		return false;
	}

	outVertices.resize(hdr.vertexCount);
	outIndices.resize(hdr.indexCount);
	if (fread(outVertices.data(), sizeof(Vertex), hdr.vertexCount, f) != hdr.vertexCount ||
		fread(outIndices.data(), sizeof(uint32_t), hdr.indexCount, f) != hdr.indexCount)
	{
		outVertices.clear();
		outIndices.clear();
		fclose(f);
		return false;
	}
	fclose(f);

	outBounds.Min = { hdr.aabbMin[0], hdr.aabbMin[1], hdr.aabbMin[2] };
	outBounds.Max = { hdr.aabbMax[0], hdr.aabbMax[1], hdr.aabbMax[2] };
	if (!IsValidModelBounds(outBounds))
		outBounds = BoundsFromVertices(outVertices);

	DebugLog("[TreeMeshAsset] loaded TMH1: %ls verts=%u idx=%u\n", resolvedPath, hdr.vertexCount, hdr.indexCount);
	return true;
}

bool LoadFbxMerged(const wchar_t* resolvedPath, std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
	ModelBounds& outBounds)
{
	outVertices.clear();
	outIndices.clear();
	outBounds = {};

	if (!resolvedPath || !resolvedPath[0])
		return false;
	if (!fs::exists(resolvedPath))
	{
		DebugLog("[TreeMeshAsset] FBX not found: %ls\n", resolvedPath);
		return false;
	}

	std::vector<Mesh> meshes;
	ImportSettings settings(resolvedPath, meshes, false, true, 1.0f);
	settings.outClips = nullptr;

	AssimpLoader loader;
	if (!loader.Load(settings))
	{
		DebugLog("[TreeMeshAsset] AssimpLoader failed: %ls\n", resolvedPath);
		return false;
	}

	size_t totalVerts = 0, totalIdx = 0;
	for (const Mesh& m : meshes)
	{
		totalVerts += m.Vertices.size();
		totalIdx += m.Indices.size();
	}
	if (totalVerts == 0 || totalIdx == 0)
	{
		DebugLog("[TreeMeshAsset] FBX loaded but 0 verts/idx: %ls\n", resolvedPath);
		return false;
	}

	outVertices.reserve(totalVerts);
	outIndices.reserve(totalIdx);

	for (const Mesh& m : meshes)
	{
		const uint32_t baseVertex = static_cast<uint32_t>(outVertices.size());
		outVertices.insert(outVertices.end(), m.Vertices.begin(), m.Vertices.end());
		for (uint32_t idx : m.Indices)
			outIndices.push_back(baseVertex + idx);
	}

	// NOTE: outBaseTransform は通常パスで TransformComponent::BaseMatrix として適用される表示用変換
	// (Scale(-1,1,1)*RotY(180))。ツリーは GPU インスタンシングで独自のワールド行列を持つため、
	// 頂点データには焼き込まない。axisFix（Z-up→Y-up 変換）は AssimpLoader 内で既に適用済み。

	outBounds = BoundsFromVertices(outVertices);
	DebugLog("[TreeMeshAsset] FBX merged: %ls verts=%zu idx=%zu submeshes=%zu bounds=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)\n",
		resolvedPath, outVertices.size(), outIndices.size(), meshes.size(),
		outBounds.Min.x, outBounds.Min.y, outBounds.Min.z,
		outBounds.Max.x, outBounds.Max.y, outBounds.Max.z);
	return true;
}

bool LoadFbxParts(const wchar_t* resolvedPath, std::vector<PartData>& outParts, ModelBounds& outMergedBounds)
{
	outParts.clear();
	outMergedBounds = {};
	if (!resolvedPath || !resolvedPath[0] || !fs::exists(resolvedPath))
		return false;

	std::vector<Mesh> meshes;
	ImportSettings settings(resolvedPath, meshes, false, true, 1.0f);
	settings.outClips = nullptr;
	AssimpLoader loader;
	if (!loader.Load(settings))
		return false;

	if (meshes.empty())
		return false;

	const int partCount = (std::min)(static_cast<int>(meshes.size()), 3);
	outParts.resize(partCount);

	bool firstBounds = true;
	for (int p = 0; p < partCount; ++p)
	{
		const Mesh& m = meshes[p];
		outParts[p].vertices = m.Vertices;
		outParts[p].indices = m.Indices;
		outParts[p].bounds = BoundsFromVertices(m.Vertices);
		outParts[p].materialName = m.MaterialName;
		outParts[p].diffuseMap = m.DiffuseMap;
		outParts[p].normalMap = m.NormalMap;
		outParts[p].metallicMap = m.MetallicMap;
		outParts[p].roughnessMap = m.RoughnessMap;
		outParts[p].opacityMap = m.OpacityMap;
		DebugLog("[TreeMeshAsset] Part %d: mat='%s' verts=%zu idx=%zu diff=%ls opacity=%ls\n",
			p, m.MaterialName.c_str(), m.Vertices.size(), m.Indices.size(),
			m.DiffuseMap.c_str(), m.OpacityMap.c_str());
		if (firstBounds && !m.Vertices.empty())
		{
			outMergedBounds = outParts[p].bounds;
			firstBounds = false;
		}
		else if (!m.Vertices.empty())
		{
			outMergedBounds.Min.x = (std::min)(outMergedBounds.Min.x, outParts[p].bounds.Min.x);
			outMergedBounds.Min.y = (std::min)(outMergedBounds.Min.y, outParts[p].bounds.Min.y);
			outMergedBounds.Min.z = (std::min)(outMergedBounds.Min.z, outParts[p].bounds.Min.z);
			outMergedBounds.Max.x = (std::max)(outMergedBounds.Max.x, outParts[p].bounds.Max.x);
			outMergedBounds.Max.y = (std::max)(outMergedBounds.Max.y, outParts[p].bounds.Max.y);
			outMergedBounds.Max.z = (std::max)(outMergedBounds.Max.z, outParts[p].bounds.Max.z);
		}
	}
	return true;
}

void GenerateProceduralSpecies0Lod0(std::vector<Vertex>& outVertices, std::vector<uint32_t>& outIndices,
	ModelBounds& outBounds)
{
	outVertices.clear();
	outIndices.clear();
	// 幹 + 円錐（種0 プレースホルダ。本番は .tmesh を DCC から書き出し）
	AddCylinder(outVertices, outIndices, 0.f, 2.2f, 0.14f, 10);
	AddCone(outVertices, outIndices, 2.2f, 5.2f, 1.15f, 10);
	outBounds = BoundsFromVertices(outVertices);
	DebugLog("[TreeMeshAsset] procedural species0 LOD0: verts=%zu idx=%zu\n", outVertices.size(), outIndices.size());
}

bool LoadSpecies0Lod0Mesh(const std::wstring& resolvedTmeshPath, std::vector<Vertex>& outVertices,
	std::vector<uint32_t>& outIndices, ModelBounds& outBounds, std::wstring& outSourceLabel)
{
	outVertices.clear();
	outIndices.clear();
	outBounds = {};
	outSourceLabel.clear();

	// 1. Try FBX (tree2LOD0.fbx → tree2.fbx)
	const wchar_t* fbxCandidates[] = {
		L"assets\\tree\\tree1\\tree2LOD0.fbx",
		L"assets\\tree\\tree1\\tree2.fbx",
	};
	for (const wchar_t* candidate : fbxCandidates)
	{
		const fs::path p(candidate);
		if (fs::exists(p) && LoadFbxMerged(p.c_str(), outVertices, outIndices, outBounds) && !outVertices.empty())
		{
			outSourceLabel = p.wstring();
			return true;
		}
	}

	// 2. Try .tmesh
	if (LoadTmeshV1(resolvedTmeshPath.c_str(), outVertices, outIndices, outBounds) && !outVertices.empty())
	{
		outSourceLabel = resolvedTmeshPath;
		return true;
	}

	// 3. Procedural fallback
	GenerateProceduralSpecies0Lod0(outVertices, outIndices, outBounds);
	outSourceLabel = L"procedural";
	return true;
}

}
