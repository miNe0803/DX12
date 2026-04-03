#include "TreeVegetation.h"

#include "TreeMeshAsset.h"
#include "Core/ModelBounds.h"
#include "DebugLog.h"
#include "DescriptorHeap.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/TerrainSystem.h"
#include "IndexBuffer.h"
#include "Texture2D.h"
#include "VertexBuffer.h"

#include <DirectXTex.h>
#include <DirectXMath.h>
#include <entt/entt.hpp>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace DirectX;

namespace fs = std::filesystem;

namespace
{
	TreeSpeciesMaterials g_speciesMats[3]{};
	VertexBuffer* g_mergedVB[3] = { nullptr, nullptr, nullptr };
	IndexBuffer* g_mergedIB[3] = { nullptr, nullptr, nullptr };
	uint32_t g_mergedIndexCount[3] = { 0, 0, 0 };
	ModelBounds g_mergedBounds{};
	uint32_t g_spawnedTreeCount = 0;
	std::vector<uint8_t> g_treeMaskRgba;
	UINT g_treeMaskW = 0;
	UINT g_treeMaskH = 0;
	float g_speciesScale[3] = { 1.f, 1.f, 1.f };

	// Full-nature cache (all instances from mask).
	std::vector<TreeVegetation::StreamedTreeInstance> g_allMaskCached;
	uint64_t g_allMaskCachedKey = 0;
	uint64_t g_maskInstancesBuildSerial = 0;

	// LOD0 ソース（解決済み .tmesh / "procedural"）
	std::wstring g_lod0ResolvedSourcePath;

	static void ClearTreeLod0Diagnostics()
	{
		g_lod0ResolvedSourcePath.clear();
	}

	// GetPart* 互換（現状はマージメッシュのみ。将来 .tmesh のサブメッシュで再び埋める）
	inline constexpr int kTreePartCount = 3;
	VertexBuffer* g_partVB[kTreePartCount] = { nullptr, nullptr, nullptr };
	IndexBuffer* g_partIB[kTreePartCount] = { nullptr, nullptr, nullptr };
	uint32_t g_partIndexCount[kTreePartCount] = { 0, 0, 0 };
	DescriptorHandle* g_partMat[kTreePartCount] = { nullptr, nullptr, nullptr };

	std::wstring ResolveAsset(const wchar_t* path)
	{
		const fs::path p(path);
		if (fs::exists(p))
			return p.wstring();
		const fs::path p2 = fs::path(L"..\\..") / p;
		if (fs::exists(p2))
			return p2.wstring();
		const fs::path p3 = fs::path(L"..\\..\\..") / p;
		if (fs::exists(p3))
			return p3.wstring();
		return p.wstring();
	}

	std::wstring ReplaceExt(const std::wstring& origin, const wchar_t* ext)
	{
		const size_t dot = origin.find_last_of(L'.');
		if (dot == std::wstring::npos)
			return origin + ext;
		return origin.substr(0, dot + 1) + ext;
	}

	/// 種テクスチャ登録（パスだけ使用。ジオメトリは .tmesh / 手続きフォールバック）。
	DescriptorHandle* RegisterTreePartTextures(DescriptorHeap* heap, const Mesh& mesh, bool hasAlphaMask)
	{
		Texture2D* albedoTex = Texture2D::Get(mesh.DiffuseMap);
		if (!albedoTex && !mesh.DiffuseMap.empty())
			albedoTex = Texture2D::Get(ReplaceExt(mesh.DiffuseMap, L"tga"));
		if (!albedoTex)
			albedoTex = Texture2D::GetWhite();
		DescriptorHandle* first = heap->Register(albedoTex);
		Texture2D* normalTex = Texture2D::Get(mesh.NormalMap);
		if (!normalTex && !mesh.NormalMap.empty())
			normalTex = Texture2D::Get(ReplaceExt(mesh.NormalMap, L"tga"));
		if (!normalTex)
			normalTex = Texture2D::GetWhite();
		DescriptorHandle* nh = heap->Register(normalTex);
		Texture2D* metallicTex = nullptr;
		if (hasAlphaMask)
		{
			metallicTex = Texture2D::Get(mesh.MetallicMap);
			if (!metallicTex && !mesh.MetallicMap.empty())
				metallicTex = Texture2D::Get(ReplaceExt(mesh.MetallicMap, L"tga"));
		}
		if (!metallicTex)
			metallicTex = Texture2D::GetWhite();
		DescriptorHandle* mh = heap->Register(metallicTex);
		Texture2D* roughTex = Texture2D::Get(mesh.RoughnessMap);
		if (!roughTex && !mesh.RoughnessMap.empty())
			roughTex = Texture2D::Get(ReplaceExt(mesh.RoughnessMap, L"tga"));
		if (!roughTex)
			roughTex = Texture2D::GetDefaultRoughness();
		DescriptorHandle* rh = heap->Register(roughTex);
		Texture2D* rampTex = Texture2D::Get(L"assets\\npr\\default_ramp.png");
		if (!rampTex) rampTex = Texture2D::GetDefaultNprRamp();
		DescriptorHandle* rampH = heap->Register(rampTex);
		Texture2D* sphereTex = Texture2D::GetWhite();
		DescriptorHandle* sh = heap->Register(sphereTex);
		if (!first || !nh || !mh || !rh || !rampH || !sh)
			return nullptr;
		return first;
	}

	DescriptorHandle* RegisterSixTextures(DescriptorHeap* heap, const Mesh& mesh)
	{
		Texture2D* albedoTex = Texture2D::Get(mesh.DiffuseMap);
		if (!albedoTex && !mesh.DiffuseMap.empty())
			albedoTex = Texture2D::Get(ReplaceExt(mesh.DiffuseMap, L"tga"));
		if (!albedoTex)
			albedoTex = Texture2D::GetWhite();
		DescriptorHandle* first = heap->Register(albedoTex);
		Texture2D* normalTex = Texture2D::Get(mesh.NormalMap);
		if (!normalTex && !mesh.NormalMap.empty())
			normalTex = Texture2D::Get(ReplaceExt(mesh.NormalMap, L"tga"));
		if (!normalTex)
			normalTex = Texture2D::GetWhite();
		DescriptorHandle* nh = heap->Register(normalTex);
		Texture2D* metallicTex = Texture2D::Get(mesh.MetallicMap);
		if (!metallicTex && !mesh.MetallicMap.empty())
			metallicTex = Texture2D::Get(ReplaceExt(mesh.MetallicMap, L"tga"));
		if (!metallicTex)
			metallicTex = Texture2D::GetDefaultMetallic();
		DescriptorHandle* mh = heap->Register(metallicTex);
		Texture2D* roughTex = Texture2D::Get(mesh.RoughnessMap);
		if (!roughTex && !mesh.RoughnessMap.empty())
			roughTex = Texture2D::Get(ReplaceExt(mesh.RoughnessMap, L"tga"));
		if (!roughTex)
			roughTex = Texture2D::GetDefaultRoughness();
		DescriptorHandle* rh = heap->Register(roughTex);
		Texture2D* rampTex = nullptr;
		if (!mesh.RampMap.empty())
			rampTex = Texture2D::Get(mesh.RampMap);
		if (!rampTex && fs::exists(L"assets\\npr\\default_ramp.png"))
			rampTex = Texture2D::Get(L"assets\\npr\\default_ramp.png");
		if (!rampTex)
			rampTex = Texture2D::GetDefaultNprRamp();
		DescriptorHandle* rampH = heap->Register(rampTex);
		Texture2D* sphereTex = Texture2D::Get(mesh.SphereMap);
		if (!sphereTex && !mesh.SphereMap.empty())
			sphereTex = Texture2D::Get(ReplaceExt(mesh.SphereMap, L"tga"));
		if (!sphereTex)
			sphereTex = Texture2D::GetWhite();
		DescriptorHandle* sh = heap->Register(sphereTex);
		if (!first || !nh || !mh || !rh || !rampH || !sh)
			return nullptr;
		return first;
	}

	bool MakeGpuBuffersForLod(int lod, const std::vector<Vertex>& verts, const std::vector<uint32_t>& indices,
		std::vector<VertexBuffer*>& outOwnedVB, std::vector<IndexBuffer*>& outOwnedIB)
	{
		g_mergedVB[lod] = new VertexBuffer(sizeof(Vertex) * verts.size(), sizeof(Vertex), verts.data());
		g_mergedIB[lod] = new IndexBuffer(sizeof(uint32_t) * indices.size(), indices.data());
		if (!g_mergedVB[lod]->IsValid() || !g_mergedIB[lod]->IsValid())
		{
			DebugLog("[TreeVegetation] VB/IB create failed (LOD=%d).\n", lod);
			delete g_mergedVB[lod];
			delete g_mergedIB[lod];
			g_mergedVB[lod] = nullptr;
			g_mergedIB[lod] = nullptr;
			return false;
		}
		outOwnedVB.push_back(g_mergedVB[lod]);
		outOwnedIB.push_back(g_mergedIB[lod]);
		g_mergedIndexCount[lod] = static_cast<uint32_t>(indices.size());
		return true;
	}

	/// CPU 頂点から LOD0 VB/IB を作成し、LOD1/2 用 FBX があればそちらも読む。
	bool CommitLod0GpuFromCpuMesh(std::vector<Vertex>&& verts, std::vector<uint32_t>&& indices,
		const std::wstring& resolvedSourcePath, DescriptorHeap* heap, std::vector<VertexBuffer*>& outOwnedVB,
		std::vector<IndexBuffer*>& outOwnedIB)
	{
		if (!heap || !heap->GetHeap())
			return false;
		if (verts.empty() || indices.empty())
		{
			DebugLog("[TreeVegetation] empty LOD0 mesh.\n");
			return false;
		}

		g_mergedBounds = ComputeBoundsFromVertices(verts);
		DebugLog("[TreeVegetation] LOD0 GPU: %ls | verts=%zu idx=%zu (~%u tris) bounds=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)\n",
			resolvedSourcePath.c_str(), verts.size(), indices.size(), static_cast<unsigned>(indices.size() / 3u),
			g_mergedBounds.Min.x, g_mergedBounds.Min.y, g_mergedBounds.Min.z,
			g_mergedBounds.Max.x, g_mergedBounds.Max.y, g_mergedBounds.Max.z);

		if (!MakeGpuBuffersForLod(0, verts, indices, outOwnedVB, outOwnedIB))
			return false;

		// LOD2
		{
			const std::wstring lod2Fbx = ResolveAsset(L"assets\\tree\\tree1\\tree2LOD2.fbx");
			std::vector<Vertex> lod2v;
			std::vector<uint32_t> lod2i;
			ModelBounds lod2b{};
			if (fs::exists(lod2Fbx) && TreeMeshAsset::LoadFbxMerged(lod2Fbx.c_str(), lod2v, lod2i, lod2b) && !lod2v.empty())
			{
				if (MakeGpuBuffersForLod(2, lod2v, lod2i, outOwnedVB, outOwnedIB))
					DebugLog("[TreeVegetation] LOD2 FBX: %ls verts=%zu idx=%zu\n", lod2Fbx.c_str(), lod2v.size(), lod2i.size());
				else
				{
					g_mergedVB[2] = g_mergedVB[0];
					g_mergedIB[2] = g_mergedIB[0];
					g_mergedIndexCount[2] = g_mergedIndexCount[0];
				}
			}
			else
			{
				g_mergedVB[2] = g_mergedVB[0];
				g_mergedIB[2] = g_mergedIB[0];
				g_mergedIndexCount[2] = g_mergedIndexCount[0];
			}
		}

		// LOD1
		{
			const std::wstring lod1Fbx = ResolveAsset(L"assets\\tree\\tree1\\tree2LOD1.fbx");
			std::vector<Vertex> lod1v;
			std::vector<uint32_t> lod1i;
			ModelBounds lod1b{};
			if (fs::exists(lod1Fbx) && TreeMeshAsset::LoadFbxMerged(lod1Fbx.c_str(), lod1v, lod1i, lod1b) && !lod1v.empty())
			{
				if (MakeGpuBuffersForLod(1, lod1v, lod1i, outOwnedVB, outOwnedIB))
					DebugLog("[TreeVegetation] LOD1 FBX: %ls verts=%zu idx=%zu\n", lod1Fbx.c_str(), lod1v.size(), lod1i.size());
				else
				{
					g_mergedVB[1] = g_mergedVB[2];
					g_mergedIB[1] = g_mergedIB[2];
					g_mergedIndexCount[1] = g_mergedIndexCount[2];
				}
			}
			else
			{
				g_mergedVB[1] = g_mergedVB[2];
				g_mergedIB[1] = g_mergedIB[2];
				g_mergedIndexCount[1] = g_mergedIndexCount[2];
				DebugLog("[TreeVegetation] LOD1: no dedicated FBX, using LOD2 mesh (%u indices)\n", g_mergedIndexCount[1]);
			}
		}

		// LOD0: use LOD2 mesh for rendering (tree2LOD0.fbx at 480K tris causes massive PS overdraw)
		// Bounds are kept from the original LOD0 for correct culling.
		if (g_mergedVB[2] && g_mergedIB[2] && g_mergedIndexCount[2] > 0)
		{
			g_mergedVB[0] = g_mergedVB[2];
			g_mergedIB[0] = g_mergedIB[2];
			g_mergedIndexCount[0] = g_mergedIndexCount[2];
			DebugLog("[TreeVegetation] LOD0: using LOD2 mesh for rendering (%u idx, ~%u tris)\n",
				g_mergedIndexCount[0], g_mergedIndexCount[0] / 3u);
		}

		g_lod0ResolvedSourcePath = resolvedSourcePath;
		return true;
	}

	struct SpeciesTextureSet
	{
		std::wstring lod0Diff;
		std::wstring lod0Nor;
		std::wstring lod0Rough;
		std::wstring lod1Alpha;
		float scale = 1.f;
	};

	bool LoadMaskRgb(const wchar_t* path, std::vector<uint8_t>& outRgba, UINT& outW, UINT& outH)
	{
		const std::wstring resolved = ResolveAsset(path);
		if (!fs::exists(resolved))
			return false;
		DirectX::TexMetadata meta{};
		DirectX::ScratchImage scratch{};
		if (FAILED(LoadFromWICFile(resolved.c_str(), WIC_FLAGS_NONE, &meta, scratch)))
			return false;
		const DirectX::Image* im = scratch.GetImage(0, 0, 0);
		if (!im || !im->pixels || meta.width == 0 || meta.height == 0)
			return false;
		outW = static_cast<UINT>(meta.width);
		outH = static_cast<UINT>(meta.height);
		outRgba.resize(static_cast<size_t>(outW) * outH * 4);
		const size_t srcPitch = im->rowPitch;
		for (UINT y = 0; y < outH; ++y)
		{
			const uint8_t* row = im->pixels + y * srcPitch;
			for (UINT x = 0; x < outW; ++x)
			{
				size_t di = (static_cast<size_t>(y) * outW + x) * 4;
				if (meta.format == DXGI_FORMAT_R8G8B8A8_UNORM || meta.format == DXGI_FORMAT_B8G8R8A8_UNORM)
				{
					const uint8_t* px = row + x * 4;
					outRgba[di + 0] = px[0];
					outRgba[di + 1] = px[1];
					outRgba[di + 2] = px[2];
					outRgba[di + 3] = px[3];
				}
				else
				{
					outRgba[di + 0] = row[x * 4];
					outRgba[di + 1] = row[x * 4 + 1];
					outRgba[di + 2] = row[x * 4 + 2];
					outRgba[di + 3] = 255;
				}
			}
		}
		return true;
	}

	inline uint32_t Hash32(uint32_t x)
	{
		// xorshift* like
		x ^= x >> 16;
		x *= 0x7feb352d;
		x ^= x >> 15;
		x *= 0x846ca68b;
		x ^= x >> 16;
		return x;
	}

	inline float Hash01(uint32_t a, uint32_t b, uint32_t seed)
	{
		uint32_t h = Hash32(a * 0x9e3779b9u ^ b * 0x85ebca6bu ^ seed);
		return (h & 0x00ffffffu) / 16777215.0f;
	}

	inline float Saturate(float v) { return (v < 0.f) ? 0.f : (v > 1.f) ? 1.f : v; }
}

namespace TreeVegetation
{

bool Initialize(::entt::registry& registry, DescriptorHeap* heap,
	std::vector<VertexBuffer*>& outOwnedVB, std::vector<IndexBuffer*>& outOwnedIB)
{
	for (auto& s : g_speciesMats)
		s = {};
	for (int i = 0; i < 3; ++i)
	{
		g_mergedVB[i] = nullptr;
		g_mergedIB[i] = nullptr;
		g_mergedIndexCount[i] = 0;
	}
	for (int p = 0; p < kTreePartCount; ++p)
	{
		g_partVB[p] = nullptr;
		g_partIB[p] = nullptr;
		g_partIndexCount[p] = 0;
		g_partMat[p] = nullptr;
	}
	g_mergedBounds = {};
	g_spawnedTreeCount = 0;
	ClearTreeLod0Diagnostics();

	if (!heap || !heap->GetHeap())
		return true;

	// LOD0: FBX は読まない。.tmesh / procedural のみ。
	{
		std::vector<Vertex> lv;
		std::vector<uint32_t> li;
		std::wstring srcLabel;
		const std::wstring tmeshPath = ResolveAsset(L"assets\\tree\\tree1\\species0_lod0.tmesh");
		TreeMeshAsset::LoadSpecies0Lod0Mesh(tmeshPath, lv, li, g_mergedBounds, srcLabel);
		if (!CommitLod0GpuFromCpuMesh(std::move(lv), std::move(li), srcLabel, heap, outOwnedVB, outOwnedIB))
			DebugLog("[TreeVegetation] LOD0 .tmesh commit failed.\n");
	}

	// Load FBX parts to get per-submesh textures AND geometry
	std::vector<TreeMeshAsset::PartData> fbxParts;
	ModelBounds fbxPartsBounds{};
	{
		const wchar_t* partCandidates[] = {
			L"assets\\tree\\tree1\\tree2LOD2.fbx",
			L"assets\\tree\\tree1\\tree2LOD0.fbx",
			L"assets\\tree\\tree1\\tree2.fbx",
		};
		for (const wchar_t* c : partCandidates)
		{
			const std::wstring p = ResolveAsset(c);
			if (fs::exists(p) && TreeMeshAsset::LoadFbxParts(p.c_str(), fbxParts, fbxPartsBounds) && !fbxParts.empty())
			{
				DebugLog("[TreeVegetation] Per-part FBX: %ls (%d parts)\n", p.c_str(), (int)fbxParts.size());
				break;
			}
		}
	}
	// Classify and reorder FBX parts to match the fixed slot order:
	//   slot 0 = trunk, slot 1 = leaves, slot 2 = branches
	// (CS and DrawIndirectLods assume this order.)
	enum PartType { PT_TRUNK, PT_BRANCHES, PT_LEAVES };
	struct MatGroup { std::wstring diff, nor, rough, alpha; };
	const MatGroup kTrunkMat = {
		L"assets\\tree\\tree1\\textures\\island_tree_01_diff_4k.jpg",
		L"assets\\tree\\tree1\\textures\\island_tree_01_nor_gl_4k.exr",
		L"assets\\tree\\tree1\\textures\\island_tree_01_rough_4k.exr",
		L"",
	};
	const MatGroup kBranchesMat = {
		L"assets\\tree\\tree1\\textures\\island_tree_01_branches_diff_4k.png",
		L"assets\\tree\\tree1\\textures\\island_tree_01_branches_nor_gl_4k.png",
		L"assets\\tree\\tree1\\textures\\island_tree_01_branches_rough_4k.png",
		L"",
	};
	const MatGroup kLeavesMat = {
		L"assets\\tree\\tree1\\textures\\island_tree_01_leaves_diff_4k.png",
		L"assets\\tree\\tree1\\textures\\island_tree_01_leaves_nor_gl_4k.png",
		L"assets\\tree\\tree1\\textures\\island_tree_01_leaves_rough_4k.png",
		L"assets\\tree\\tree1\\textures\\island_tree_01_leaves_alpha_4k.png",
	};

	auto ClassifyPart = [](const TreeMeshAsset::PartData& pd) -> PartType {
		auto toLower = [](const std::string& s) {
			std::string r = s;
			for (auto& c : r) if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
			return r;
		};
		auto toLowerW = [](const std::wstring& s) {
			std::wstring r = s;
			for (auto& c : r) if (c >= L'A' && c <= L'Z') c += (L'a' - L'A');
			return r;
		};
		std::string matLow = toLower(pd.materialName);
		std::wstring diffLow = toLowerW(pd.diffuseMap);
		std::wstring opacLow = toLowerW(pd.opacityMap);
		if (matLow.find("leaf") != std::string::npos || matLow.find("leaves") != std::string::npos
			|| diffLow.find(L"leaf") != std::wstring::npos || diffLow.find(L"leaves") != std::wstring::npos
			|| opacLow.find(L"alpha") != std::wstring::npos)
			return PT_LEAVES;
		if (matLow.find("branch") != std::string::npos
			|| diffLow.find(L"branch") != std::wstring::npos)
			return PT_BRANCHES;
		return PT_TRUNK;
	};

	// Reorder FBX submeshes into fixed slot order: [0]=trunk, [1]=leaves, [2]=branches
	// This matches the CS (IndexCountsTrunk=slot0, IndexCountsLeaves=slot1, IndexCountsBranches=slot2)
	// and DrawIndirectLods (part==1 → alpha-cut PSO for leaves).
	const MatGroup* slotMat[3] = { &kTrunkMat, &kLeavesMat, &kBranchesMat };
	int slotSrcIdx[3] = { -1, -1, -1 }; // which FBX submesh goes into which slot

	for (int i = 0; i < (int)fbxParts.size(); ++i)
	{
		PartType pt = ClassifyPart(fbxParts[i]);
		int slot = (pt == PT_TRUNK) ? 0 : (pt == PT_LEAVES) ? 1 : 2;
		if (slotSrcIdx[slot] < 0)
			slotSrcIdx[slot] = i;
		DebugLog("[TreeVegetation] FBX submesh %d → '%s' (mat='%s') → slot %d\n", i,
			pt == PT_LEAVES ? "leaves" : pt == PT_BRANCHES ? "branches" : "trunk",
			fbxParts[i].materialName.c_str(), slot);
	}

	g_speciesScale[0] = 0.045f;
	for (int slot = 0; slot < kTreePartCount; ++slot)
	{
		const int srcIdx = slotSrcIdx[slot];
		const MatGroup* chosen = slotMat[slot];

		// Create VB/IB from the classified FBX submesh
		if (srcIdx >= 0)
		{
			auto& part = fbxParts[srcIdx];
			if (!part.vertices.empty() && !part.indices.empty())
			{
				g_partVB[slot] = new VertexBuffer(sizeof(Vertex) * part.vertices.size(), sizeof(Vertex), part.vertices.data());
				g_partIB[slot] = new IndexBuffer(sizeof(uint32_t) * part.indices.size(), part.indices.data());
				if (g_partVB[slot]->IsValid() && g_partIB[slot]->IsValid())
				{
					g_partIndexCount[slot] = static_cast<uint32_t>(part.indices.size());
					outOwnedVB.push_back(g_partVB[slot]);
					outOwnedIB.push_back(g_partIB[slot]);
				}
				else
				{
					delete g_partVB[slot]; g_partVB[slot] = nullptr;
					delete g_partIB[slot]; g_partIB[slot] = nullptr;
					g_partIndexCount[slot] = 0;
				}
			}
		}

		Mesh m0{};
		m0.DiffuseMap = ResolveAsset(chosen->diff.c_str());
		m0.NormalMap = ResolveAsset(chosen->nor.c_str());
		if (!chosen->alpha.empty())
			m0.MetallicMap = ResolveAsset(chosen->alpha.c_str());
		m0.RoughnessMap = ResolveAsset(chosen->rough.c_str());
		m0.RampMap = L"assets\\npr\\default_ramp.png";
		m0.SphereMap = L"";
		g_partMat[slot] = RegisterTreePartTextures(heap, m0, slot == 1);
		DebugLog("[TreeVegetation] Slot %d (%s): src=%d idx=%u mat=%p\n", slot,
			slot == 0 ? "trunk" : slot == 1 ? "leaves" : "branches",
			srcIdx, g_partIndexCount[slot], (void*)g_partMat[slot]);
	}

	g_speciesMats[0].matLod0 = g_partMat[0] ? g_partMat[0] : RegisterSixTextures(heap, Mesh{});
	g_speciesMats[0].matLod1 = (kTreePartCount > 1 && g_partMat[1]) ? g_partMat[1] : g_speciesMats[0].matLod0;
	g_speciesMats[0].matLod2 = g_speciesMats[0].matLod0;

	for (int si = 1; si < 3; ++si)
	{
		g_speciesScale[si] = 0.045f;
		g_speciesMats[si].matLod0 = g_speciesMats[0].matLod0;
		g_speciesMats[si].matLod1 = g_speciesMats[0].matLod1;
		g_speciesMats[si].matLod2 = g_speciesMats[0].matLod2;
	}

	std::vector<uint8_t> rgba;
	UINT mw = 0, mh = 0;
	const wchar_t* maskPath = L"assets\\terrain\\tree_mask.png";
	if (!LoadMaskRgb(maskPath, rgba, mw, mh) || mw == 0 || mh == 0)
	{
		DebugLog("[TreeVegetation] tree_mask load fail — geometry only, no instances.\n");
		return true;
	}
	g_treeMaskRgba = rgba;
	g_treeMaskW = mw;
	g_treeMaskH = mh;

	auto terrainView = registry.view<TerrainComponent>();
	if (terrainView.begin() == terrainView.end())
		return true;
	const entt::entity terrainEntity = *terrainView.begin();
	const TerrainComponent& terr = registry.get<TerrainComponent>(terrainEntity);
	const float halfW = static_cast<float>(terr.GridWidth) * terr.CellSpacing * 0.5f;
	const float halfD = static_cast<float>(terr.GridDepth) * terr.CellSpacing * 0.5f;

	const int stride = 32;
	const float thr = 0.11f;
	// GPU ExecuteIndirect をメインにするため、ECS への木スポーン（デバッグ用）はデフォルト無効
	const uint32_t maxTrees = 0;
	uint32_t spawned = 0;

	const float footY = g_mergedBounds.Min.y;
	// NOTE:
	// 以前は (0,0) から走査して最初にヒットしたピクセルにスポーンしていたため、
	// fu/fv が 0 付近だと wx/wz が -halfW/-halfD になり「初期位置が -2024 付近」になりやすい。
	// 見た目確認をしやすくするため、マスク中心に近いセルから優先して走査する。
	const int startX = static_cast<int>(mw / 2);
	const int startY = static_cast<int>(mh / 2);
	const int step = std::max(1, stride);
	const int maxR = std::max(static_cast<int>(mw), static_cast<int>(mh));

	auto trySpawnAt = [&](UINT px, UINT py) -> void
	{
		if (spawned >= maxTrees)
			return;
		const size_t i = (static_cast<size_t>(py) * mw + px) * 4;
		if (i + 2 >= rgba.size())
			return;
		const float rf = rgba[i] / 255.f;
		const float gf = rgba[i + 1] / 255.f;
		const float bf = rgba[i + 2] / 255.f;
		if (rf < thr && gf < thr && bf < thr)
			return;

		uint8_t species = 0;
		if (rf >= gf && rf >= bf && rf >= thr)
			species = 0;
		else if (gf >= bf && gf >= thr)
			species = 1;
		else if (bf >= thr)
			species = 2;
		else
			return;

		const float jx = static_cast<float>((px * 73 + py * 31) % 13) / 13.f;
		const float jz = static_cast<float>((px * 17 + py * 97) % 11) / 11.f;
		const float fu = (px + jx) / static_cast<float>(mw);
		const float fv = (py + jz) / static_cast<float>(mh);
		const float wx = fu * (2.f * halfW) - halfW;
		const float wz = fv * (2.f * halfD) - halfD;
		const float ground = TerrainSystem::GetHeight(registry, wx, wz);
		const float sc = g_speciesScale[species];
		const float gy = ground - footY * sc;
		const float rot = static_cast<float>((px * 127 + py + species * 13) % 6283) / 1000.f;

		if (g_mergedVB[0] && g_mergedIB[0] && g_mergedIndexCount[0] != 0)
		{
			const entt::entity e = registry.create();
			TransformComponent tc{};
			XMStoreFloat4x4(&tc.BaseMatrix, XMMatrixIdentity());
			tc.Position = XMFLOAT3(wx, gy, wz);
			tc.UniformScale = sc;
			tc.RotationY = rot;
			tc.WorldMatrix = XMMatrixIdentity();
			registry.emplace<TransformComponent>(e, tc);

			MeshRendererComponent mrc{};
			mrc.pVB = g_mergedVB[0];
			mrc.pIB = g_mergedIB[0];
			mrc.IndexCount = static_cast<UINT>(g_mergedIndexCount[0]);
			mrc.OwnsGpuBuffers = false;
			const TreeSpeciesMaterials* mats = GetSpeciesMaterials(species);
			mrc.MaterialHandle = (mats && mats->matLod0) ? mats->matLod0 : nullptr;
			mrc.LocalBounds = g_mergedBounds;
			mrc.HasLocalBounds = IsValidModelBounds(g_mergedBounds);
			mrc.SkipCpuFrustumCull = false;
			registry.emplace<MeshRendererComponent>(e, mrc);
			registry.emplace<TreeInstanceTag>(e, TreeInstanceTag{ species });
			registry.emplace<LODComponent>(e, LODComponent{ 0, 0.f });
			registry.emplace<EditorHierarchyLabelComponent>(e, EditorHierarchyLabelComponent{ L"Tree (LOD0 mesh)" });
		}

		++spawned;
	};

	for (int r = 0; r <= maxR && spawned < maxTrees; r += step)
	{
		const int y0 = startY - r;
		const int y1 = startY + r;
		const int x0 = startX - r;
		const int x1 = startX + r;

		for (int x = x0; x <= x1 && spawned < maxTrees; x += step)
		{
			if (y0 >= 0 && y0 < static_cast<int>(mh) && x >= 0 && x < static_cast<int>(mw))
				trySpawnAt(static_cast<UINT>(x), static_cast<UINT>(y0));
		}
		for (int y = y0 + step; y <= y1 && spawned < maxTrees; y += step)
		{
			if (x1 >= 0 && x1 < static_cast<int>(mw) && y >= 0 && y < static_cast<int>(mh))
				trySpawnAt(static_cast<UINT>(x1), static_cast<UINT>(y));
		}
		for (int x = x1 - step; x >= x0 && spawned < maxTrees; x -= step)
		{
			if (y1 >= 0 && y1 < static_cast<int>(mh) && x >= 0 && x < static_cast<int>(mw))
				trySpawnAt(static_cast<UINT>(x), static_cast<UINT>(y1));
		}
		for (int y = y1 - step; y >= y0 + step && spawned < maxTrees; y -= step)
		{
			if (x0 >= 0 && x0 < static_cast<int>(mw) && y >= 0 && y < static_cast<int>(mh))
				trySpawnAt(static_cast<UINT>(x0), static_cast<UINT>(y));
		}
	}

	g_spawnedTreeCount = spawned;
	DebugLog("[TreeVegetation] instances=%u (mask %ux%u stride=%d)\n", spawned, mw, mh, stride);
	return true;
}

bool BuildStreamedInstances(
	::entt::registry& registry,
	const DirectX::XMFLOAT3& cameraPos,
	std::vector<StreamedTreeInstance>& outInstances,
	uint32_t maxInstances,
	float nearRadius,
	float farRadius,
	float cellSize)
{
	outInstances.clear();
	if (!g_mergedVB[0] || !g_mergedIB[0] || g_mergedIndexCount[0] == 0)
		return false;
	if (g_treeMaskRgba.empty() || g_treeMaskW == 0 || g_treeMaskH == 0)
		return false;
	if (maxInstances == 0 || cellSize <= 0.f || farRadius <= 1.f)
		return false;

	// Terrain extents for world<->mask mapping
	auto terrainView = registry.view<TerrainComponent>();
	if (terrainView.empty())
		return false;
	const entt::entity terrainEntity = *terrainView.begin();
	const TerrainComponent& terr = registry.get<TerrainComponent>(terrainEntity);
	const float halfW = static_cast<float>(terr.GridWidth) * terr.CellSpacing * 0.5f;
	const float halfD = static_cast<float>(terr.GridDepth) * terr.CellSpacing * 0.5f;
	if (halfW <= 1e-3f || halfD <= 1e-3f)
		return false;

	const float thr = 0.11f;
	const float footY = g_mergedBounds.Min.y;
	const uint32_t seed = 0x1234abcd;

	const float camX = cameraPos.x;
	const float camZ = cameraPos.z;

	const int minCx = static_cast<int>(floorf((camX - farRadius) / cellSize));
	const int maxCx = static_cast<int>(floorf((camX + farRadius) / cellSize));
	const int minCz = static_cast<int>(floorf((camZ - farRadius) / cellSize));
	const int maxCz = static_cast<int>(floorf((camZ + farRadius) / cellSize));

	outInstances.reserve(maxInstances);

	// Near-first: scan a growing ring so we fill close cells first.
	const int spanX = maxCx - minCx + 1;
	const int spanZ = maxCz - minCz + 1;
	const int centerCx = static_cast<int>(floorf(camX / cellSize));
	const int centerCz = static_cast<int>(floorf(camZ / cellSize));
	const int maxR = std::max(std::max(abs(maxCx - centerCx), abs(centerCx - minCx)),
		std::max(abs(maxCz - centerCz), abs(centerCz - minCz)));

	auto tryCell = [&](int cx, int cz)
	{
		if (outInstances.size() >= maxInstances)
			return;

		const float baseX = (static_cast<float>(cx) + 0.5f) * cellSize;
		const float baseZ = (static_cast<float>(cz) + 0.5f) * cellSize;
		const float dx = baseX - camX;
		const float dz = baseZ - camZ;
		const float dist = sqrtf(dx * dx + dz * dz);
		if (dist > farRadius)
			return;

		// distance thinning: keep all nearRadius, then fade to a minimum density
		const float t = (dist <= nearRadius) ? 1.f : Saturate((farRadius - dist) / std::max(1.f, (farRadius - nearRadius)));
		const float density = 0.05f + 0.95f * (t * t); // far keeps 5%
		const float r = Hash01(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz), seed);
		if (r > density)
			return;

		// jitter inside cell
		const float jx = (Hash01(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz), seed ^ 0x1111u) - 0.5f) * cellSize;
		const float jz = (Hash01(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz), seed ^ 0x2222u) - 0.5f) * cellSize;
		const float wx = baseX + jx;
		const float wz = baseZ + jz;

		// Map world to mask uv/pixel
		const float fu = (wx + halfW) / (2.f * halfW);
		const float fv = (wz + halfD) / (2.f * halfD);
		if (fu < 0.f || fu > 1.f || fv < 0.f || fv > 1.f)
			return;
		const UINT px = static_cast<UINT>(std::min<float>(g_treeMaskW - 1.f, std::max<float>(0.f, fu * (g_treeMaskW - 1.f))));
		const UINT py = static_cast<UINT>(std::min<float>(g_treeMaskH - 1.f, std::max<float>(0.f, fv * (g_treeMaskH - 1.f))));
		const size_t i = (static_cast<size_t>(py) * g_treeMaskW + px) * 4;
		if (i + 2 >= g_treeMaskRgba.size())
			return;
		const float rf = g_treeMaskRgba[i] / 255.f;
		const float gf = g_treeMaskRgba[i + 1] / 255.f;
		const float bf = g_treeMaskRgba[i + 2] / 255.f;
		if (rf < thr && gf < thr && bf < thr)
			return;

		uint8_t species = 0;
		if (rf >= gf && rf >= bf && rf >= thr)
			species = 0;
		else if (gf >= bf && gf >= thr)
			species = 1;
		else if (bf >= thr)
			species = 2;
		else
			return;

		const float ground = TerrainSystem::GetHeight(registry, wx, wz);
		// Debug: many FBX exports are already in world units; tiny scale makes trees effectively invisible.
		// Once visibility is confirmed, wire per-species scale back in.
		const float sc = 1.0f;
		const float gy = ground - footY * sc;
		const float rot = Hash01(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz), seed ^ (0x3333u + species * 17u)) * XM_2PI;

		const XMMATRIX T = XMMatrixTranslation(wx, gy, wz);
		const XMMATRIX S = XMMatrixScaling(sc, sc, sc);
		const XMMATRIX R = XMMatrixRotationY(rot);
		const XMMATRIX world = S * R * T;
		StreamedTreeInstance inst{};
		inst.worldGpuT = XMMatrixTranspose(world); // TransformComponent convention
		inst.speciesIndex = species;
		outInstances.push_back(inst);
	};

	for (int r = 0; r <= maxR && outInstances.size() < maxInstances; ++r)
	{
		const int cz0 = centerCz - r;
		const int cz1 = centerCz + r;
		const int cx0 = centerCx - r;
		const int cx1 = centerCx + r;

		for (int cx = cx0; cx <= cx1; ++cx) { tryCell(cx, cz0); }
		for (int cz = cz0 + 1; cz <= cz1 - 1; ++cz) { tryCell(cx1, cz); }
		if (cz1 != cz0)
			for (int cx = cx1; cx >= cx0; --cx) { tryCell(cx, cz1); }
		if (cx1 != cx0)
			for (int cz = cz1 - 1; cz >= cz0 + 1; --cz) { tryCell(cx0, cz); }
	}

	return !outInstances.empty();
}

bool BuildAllMaskInstances(
	::entt::registry& registry,
	std::vector<StreamedTreeInstance>& outInstances,
	float cellSize)
{
	outInstances.clear();
	// マスク植生の CPU リストは LOD0 メッシュより先に構築可能（描画には VB が必要）。
	if (g_treeMaskRgba.empty() || g_treeMaskW == 0 || g_treeMaskH == 0)
		return false;
	if (cellSize <= 0.0f)
		return false;

	// Terrain extents for world<->mask mapping
	auto terrainView = registry.view<TerrainComponent>();
	if (terrainView.empty())
		return false;
	const entt::entity terrainEntity = *terrainView.begin();
	const TerrainComponent& terr = registry.get<TerrainComponent>(terrainEntity);
	const float halfW = static_cast<float>(terr.GridWidth) * terr.CellSpacing * 0.5f;
	const float halfD = static_cast<float>(terr.GridDepth) * terr.CellSpacing * 0.5f;
	if (halfW <= 1e-3f || halfD <= 1e-3f)
		return false;

	// Cache key: mask + terrain + cell + 足元 Y（LOD0 非同期完了で変わるため再構築する）
	const bool lod0Ready = g_mergedVB[0] && g_mergedIB[0] && g_mergedIndexCount[0] > 0u;
	const float footYForKey = lod0Ready ? g_mergedBounds.Min.y : 0.f;
	uint64_t key = 0;
	key ^= (static_cast<uint64_t>(g_treeMaskW) << 0);
	key ^= (static_cast<uint64_t>(g_treeMaskH) << 16);
	key ^= (static_cast<uint64_t>(terr.GridWidth) << 32);
	key ^= (static_cast<uint64_t>(terr.GridDepth) << 40);
	key ^= static_cast<uint64_t>(static_cast<uint32_t>(cellSize * 1000.0f)) * 0x9E3779B185EBCA87ull;
	key ^= static_cast<uint64_t>(static_cast<int32_t>(footYForKey * 100000.f)) << 48;

	if (g_allMaskCachedKey == key && !g_allMaskCached.empty())
	{
		// キャッシュヒット時は out に 10 万本超を毎フレームコピーしない（Scene は GetAllMaskInstancesCached() のみ参照）。
		return true;
	}

	g_allMaskCached.clear();

	const float thr = 0.11f;
	const float footY = lod0Ready ? g_mergedBounds.Min.y : 0.f;
	const uint32_t seed = 0x1234abcd;

	// Iterate world cells across the terrain in a deterministic grid.
	const int minCx = static_cast<int>(floorf((-halfW) / cellSize));
	const int maxCx = static_cast<int>(floorf((+halfW) / cellSize));
	const int minCz = static_cast<int>(floorf((-halfD) / cellSize));
	const int maxCz = static_cast<int>(floorf((+halfD) / cellSize));

	g_allMaskCached.reserve(static_cast<size_t>(std::max(0, (maxCx - minCx + 1) * (maxCz - minCz + 1))));

	for (int cz = minCz; cz <= maxCz; ++cz)
	{
		for (int cx = minCx; cx <= maxCx; ++cx)
		{
			const float baseX = (static_cast<float>(cx) + 0.5f) * cellSize;
			const float baseZ = (static_cast<float>(cz) + 0.5f) * cellSize;

			// jitter inside cell (deterministic)
			const float jx = (Hash01(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz), seed ^ 0x1111u) - 0.5f) * cellSize;
			const float jz = (Hash01(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz), seed ^ 0x2222u) - 0.5f) * cellSize;
			const float wx = baseX + jx;
			const float wz = baseZ + jz;

			// Map world to mask uv/pixel
			const float fu = (wx + halfW) / (2.f * halfW);
			const float fv = (wz + halfD) / (2.f * halfD);
			if (fu < 0.f || fu > 1.f || fv < 0.f || fv > 1.f)
				continue;
			const UINT px = static_cast<UINT>(std::min<float>(g_treeMaskW - 1.f, std::max<float>(0.f, fu * (g_treeMaskW - 1.f))));
			const UINT py = static_cast<UINT>(std::min<float>(g_treeMaskH - 1.f, std::max<float>(0.f, fv * (g_treeMaskH - 1.f))));
			const size_t i = (static_cast<size_t>(py) * g_treeMaskW + px) * 4;
			if (i + 2 >= g_treeMaskRgba.size())
				continue;

			const float rf = g_treeMaskRgba[i] / 255.f;
			const float gf = g_treeMaskRgba[i + 1] / 255.f;
			const float bf = g_treeMaskRgba[i + 2] / 255.f;
			if (rf < thr && gf < thr && bf < thr)
				continue;

			uint8_t species = 0;
			if (rf >= gf && rf >= bf && rf >= thr)
				species = 0;
			else if (gf >= bf && gf >= thr)
				species = 1;
			else if (bf >= thr)
				species = 2;
			else
				continue;

			const float ground = TerrainSystem::GetHeight(registry, wx, wz);
			const float sc = 1.0f;
			const float gy = ground - footY * sc;
			const float rot = Hash01(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz), seed ^ (0x3333u + species * 17u)) * XM_2PI;

			const XMMATRIX T = XMMatrixTranslation(wx, gy, wz);
			const XMMATRIX S = XMMatrixScaling(sc, sc, sc);
			const XMMATRIX R = XMMatrixRotationY(rot);
			const XMMATRIX world = S * R * T;

			StreamedTreeInstance inst{};
			inst.worldGpuT = XMMatrixTranspose(world);
			inst.speciesIndex = species;
			g_allMaskCached.push_back(inst);
		}
	}

	g_allMaskCachedKey = key;
	++g_maskInstancesBuildSerial;
	outInstances = g_allMaskCached;
	DebugLog("[Trees][AllMask] built=%zu (cell=%.2fm mask=%ux%u)\n", outInstances.size(), cellSize, g_treeMaskW, g_treeMaskH);
	return !outInstances.empty();
}

const std::vector<StreamedTreeInstance>& GetAllMaskInstancesCached()
{
	return g_allMaskCached;
}

const TreeSpeciesMaterials* GetSpeciesMaterials(size_t speciesIndex)
{
	if (speciesIndex >= 3)
		return nullptr;
	return &g_speciesMats[speciesIndex];
}

VertexBuffer* GetPartVertexBuffer(int part)
{
	if (part < 0 || part >= kTreePartCount) return nullptr;
	return g_partVB[part];
}
IndexBuffer* GetPartIndexBuffer(int part)
{
	if (part < 0 || part >= kTreePartCount) return nullptr;
	return g_partIB[part];
}
uint32_t GetPartIndexCount(int part)
{
	if (part < 0 || part >= kTreePartCount) return 0;
	return g_partIndexCount[part];
}
DescriptorHandle* GetPartMaterialHandle(int part)
{
	if (part < 0 || part >= kTreePartCount) return nullptr;
	return g_partMat[part];
}

VertexBuffer* GetMergedVertexBuffer() { return g_mergedVB[0]; }
IndexBuffer* GetMergedIndexBuffer() { return g_mergedIB[0]; }
uint32_t GetMergedIndexCount() { return g_mergedIndexCount[0]; }
VertexBuffer* GetMergedVertexBufferLod(int lod)
{
	if (lod < 0 || lod > 2) return nullptr;
	return g_mergedVB[lod];
}
IndexBuffer* GetMergedIndexBufferLod(int lod)
{
	if (lod < 0 || lod > 2) return nullptr;
	return g_mergedIB[lod];
}
uint32_t GetMergedIndexCountLod(int lod)
{
	if (lod < 0 || lod > 2) return 0;
	return g_mergedIndexCount[lod];
}
const ModelBounds& GetMergedLocalBounds() { return g_mergedBounds; }

bool GetImposterBillboardParams(ImposterBillboardParams* out)
{
	if (!out)
		return false;
	if (!IsValidModelBounds(g_mergedBounds))
	{
		out->FootLocal = { 0.f, 0.f, 0.f };
		out->HalfWidth = 1.f;
		out->Height = 1.f;
		return false;
	}
	const ModelBounds& b = g_mergedBounds;
	out->FootLocal.x = 0.5f * (b.Min.x + b.Max.x);
	out->FootLocal.y = b.Min.y;
	out->FootLocal.z = 0.5f * (b.Min.z + b.Max.z);
	const float dx = b.Max.x - b.Min.x;
	const float dz = b.Max.z - b.Min.z;
	out->HalfWidth = 0.5f * (std::max)(dx, dz) + 0.25f;
	out->Height = b.Max.y - b.Min.y;
	return true;
}

uint32_t GetSpawnedTreeCount() { return g_spawnedTreeCount; }

bool IsLod0MeshReady()
{
	return g_mergedVB[0] != nullptr && g_mergedIB[0] != nullptr && g_mergedIndexCount[0] > 0u;
}

const wchar_t* GetLod0SourcePath()
{
	return g_lod0ResolvedSourcePath.empty() ? L"" : g_lod0ResolvedSourcePath.c_str();
}

}

uint64_t TreeVegetation::GetMaskInstancesBuildSerial()
{
	return g_maskInstancesBuildSerial;
}
