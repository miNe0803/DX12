#include "Scene.h"
#include "Engine.h"
#include "App.h"
#include <d3dx12.h>
#ifdef ResourceBarrier
#undef ResourceBarrier
#endif
#include <SharedStruct.h>
#include <VertexBuffer.h>
#include <ConstantBuffer.h>
#include <RootSignature.h>
#include <PipelineState.h>
#include <IndexBuffer.h>
#include <assimpLoader.h>
#include "DescriptorHeap.h"
#include "Texture2D.h"
#include "keyboard.h"
#include "Camera.h"
#include "Graphics/IBLGenerator.h"
#include "Graphics/SkyboxRenderer.h"
#include "Graphics/PostProcessSystem.h"
#include "Graphics/PostProcessSettings.h"
#include "EngineBarrier.h"
#include "DebugLog.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/TransformSystem.h"
#include "Engine/ECS/Systems/LODSystem.h"
#include "Systems/PlayerSystem.h"
#include "Systems/CameraSystem.h"
#include "Engine/ECS/Systems/RenderSystem.h"
#include "Engine/ECS/Systems/TerrainSystem.h"
#include "Graphics/TerrainGenerator.h"
#include "Graphics/TerrainGpuCullSystem.h"
#include "Graphics/HiZSystem.h"
#include "Graphics/TreeGpuCullSystem.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/AtmosphereSystem.h"
#include "Graphics/TreeVegetation.h"
#include "Graphics/TreeImposterBake.h"
#include "ComPtr.h"
#include "Engine/ECS/Systems/TreeLodSystem.h"
#include "ModelBounds.h"
#include "Engine/Core/AsyncModelLoader.h"
#include "NprTuning.h"
#include "GpuDebugLabels.h"
#include "Engine/Profiling/Profiler.h"

#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <cstdio>

using namespace DirectX;
namespace fs = std::filesystem;

static HiZSystem* s_hiz = nullptr;
static TerrainGpuCullSystem* s_terrainGpuCull = nullptr;
static TreeGpuCullSystem* s_treeGpuCull = nullptr;
static ShadowSystem* s_shadow = nullptr;
static AtmosphereSystem* s_atmosphere = nullptr;
static AtmosphereParams s_atmosphereParams;

std::wstring ReplaceExtension(const std::wstring& origin, const char* ext)
{
	fs::path p = origin.c_str();
	return p.replace_extension(ext).c_str();
}

DescriptorHeap* descriptorHeap = nullptr;

Scene* g_Scene = nullptr;
ConstantBuffer* constantBuffer[Engine::FRAME_BUFFER_COUNT] = {};
ConstantBuffer* sceneConstantBuffer[Engine::FRAME_BUFFER_COUNT] = {};
ConstantBuffer* pbrPropertyBuffer[Engine::FRAME_BUFFER_COUNT] = {};

RootSignature* rootSignature = nullptr;
PipelineState* pipelineState = nullptr;
PipelineState* nprPipelineState = nullptr;
PipelineState* nprTransparentPipelineState = nullptr;
RootSignature* terrainRootSignature = nullptr;
PipelineState* terrainDepthPrepassPipelineState = nullptr;
PipelineState* terrainPipelineState = nullptr;
PipelineState* treeOpaquePipelineState = nullptr;
PipelineState* treeLod1PipelineState = nullptr;
PipelineState* treeLod2PipelineState = nullptr;
PipelineState* treeImposterPipelineState = nullptr;
ConstantBuffer* terrainConstantBuffer[Engine::FRAME_BUFFER_COUNT] = {};
Camera* g_Camera = nullptr;

static void WriteNprTuningToPbrConstants(PBRConstants* pbrConst)
{
	if (!pbrConst || !g_Camera)
		return;
	XMVECTOR camPos = g_Camera->GetPosition();
	XMStoreFloat4(&pbrConst->CameraPos, camPos);
	// RimParams.x は未使用（PBR/NPR 不透明は y=法線スケール z=リムべき w=リム強度のみ参照）
	pbrConst->RimParams = XMFLOAT4(1.0f, g_NprGpuTuning.normalScale,
		g_NprGpuTuning.rimPower, g_NprGpuTuning.rimStrength);
	// NprTuning.y は未使用（旧 transparent exposure）
	pbrConst->NprTuning = XMFLOAT4(g_NprGpuTuning.virtualLight, 0.0f,
		g_NprGpuTuning.opaqueAlphaClip, g_NprGpuTuning.ambientShadowStrength);
	pbrConst->NprTuning2 = XMFLOAT4(g_NprGpuTuning.celVertexNormalBlend, g_NprGpuTuning.celShadeSharpness,
		g_NprGpuTuning.rimVertexNormalBlend, static_cast<float>(g_NprGpuTuning.nprDebugRampView));
	pbrConst->NprDebugHdr = XMFLOAT4(0.f, 0.f, 0.f, 0.f);
}

namespace {
	constexpr float kTerrainCellSpacing = 8.0f;
	constexpr float kTerrainMaxHeight = 250.0f;
	// PIX デバッグ用: true で Terrain GPU カリング（ExecuteIndirect 経路）を無効化。
	constexpr bool kDisableTerrainGpuCullForDebug = false;
	// 木: GPU ExecuteIndirect をメインにする
	constexpr bool kDisableTreeGpuCullWork = false;
	// true: Hi-Z オクルージョンを木 CS で使用。誤遮蔽で全滅する環境があるため既定はオフ（視錐のみ）。
	constexpr bool kTreeCullUseHiZ = false;

	static_assert(sizeof(TreeVegetation::StreamedTreeInstance) == sizeof(TreeGpuCullSystem::TreeInstanceCpu));
	static_assert(alignof(TreeVegetation::StreamedTreeInstance) == alignof(TreeGpuCullSystem::TreeInstanceCpu));

	SkyboxRenderer* s_skyboxRenderer = nullptr;
	ComPtr<ID3D12Resource> skyboxCubemap;
	ComPtr<ID3D12Resource> skyboxEquirect;
	ComPtr<ID3D12Resource> s_irradianceCubemap;
	ComPtr<ID3D12Resource> s_prefilterCubemap;
	ComPtr<ID3D12Resource> s_brdfLut;
	PostProcessSystem* s_postProcess = nullptr;
	DescriptorHandle* s_hdrSrvHandle = nullptr;
	DescriptorHandle* s_envCubemapHandle = nullptr;
	DescriptorHandle* s_terrainMaskHandle = nullptr;
	DescriptorHandle* s_hizPyramidSrvHandle = nullptr;
	DescriptorHandle* s_treeImposterMatTableStart[3] = { nullptr, nullptr, nullptr };
	D3D12_GPU_DESCRIPTOR_HANDLE s_treeImposterMatGpu[3] = {};
	bool s_treeImposterBakeOk = false;
	VertexBuffer* s_treeImposterQuadVb = nullptr;
	IndexBuffer* s_treeImposterQuadIb = nullptr;

	// マスク木の GPU アップロード（ポストCLで UpdateInstances → DispatchCull と同じ static を共有）
	static bool s_treeGpuMaskUploaded = false;
	static size_t s_treeGpuMaskLastUploadedCount = 0;
	static uint64_t s_treeGpuMaskLastSerial = 0;

	// DispatchCull と DrawIndirectLods に渡す indexCount を同一にする。
	static void FillTreeIndexCountByPartByLod(uint32_t out[3][3])
	{
		const uint32_t idxMerged0 = TreeVegetation::GetMergedIndexCountLod(0);
		const uint32_t idxMerged1 = TreeVegetation::GetMergedIndexCountLod(1);
		const uint32_t idxMerged2 = TreeVegetation::GetMergedIndexCountLod(2);
		const uint32_t idxTrunk0 = TreeVegetation::GetPartIndexCount(0);
		const uint32_t idxLeaves0 = TreeVegetation::GetPartIndexCount(1);
		const uint32_t idxBranch0 = TreeVegetation::GetPartIndexCount(2);

		const bool noPartSplit = (idxTrunk0 == 0u && idxLeaves0 == 0u && idxBranch0 == 0u);
		if (noPartSplit)
		{
			// FBX マージメッシュは幹/葉/枝を分離していない。
			// part0(trunk) だけでフルメッシュを描き、part1/2 は 0 にして 3 倍描画を回避する。
			out[0][0] = idxMerged0;
			out[0][1] = idxMerged1;
			out[0][2] = idxMerged2;
			for (int lod = 0; lod < 3; ++lod)
			{
				out[1][lod] = 0u;
				out[2][lod] = 0u;
			}
		}
		else
		{
			const bool twoPartSplitNoBranchMesh =
				idxTrunk0 != 0u && idxLeaves0 != 0u && idxBranch0 == 0u;
			out[0][0] = idxTrunk0 ? idxTrunk0 : idxMerged0;
			out[0][1] = idxTrunk0 ? idxTrunk0 : idxMerged1;
			out[0][2] = idxTrunk0 ? idxTrunk0 : idxMerged2;
			out[1][0] = idxLeaves0 ? idxLeaves0 : idxMerged0;
			out[1][1] = idxLeaves0 ? idxLeaves0 : idxMerged1;
			out[1][2] = idxLeaves0 ? idxLeaves0 : idxMerged2;
			const uint32_t br0 = twoPartSplitNoBranchMesh ? 0u : (idxBranch0 ? idxBranch0 : idxMerged0);
			const uint32_t br1 = twoPartSplitNoBranchMesh ? 0u : (idxBranch0 ? idxBranch0 : idxMerged1);
			const uint32_t br2 = twoPartSplitNoBranchMesh ? 0u : (idxBranch0 ? idxBranch0 : idxMerged2);
			out[2][0] = br0;
			out[2][1] = br1;
			out[2][2] = br2;
		}

		const bool imposterReady = s_treeImposterBakeOk && s_treeImposterQuadVb && s_treeImposterQuadIb
			&& s_treeImposterMatGpu[0].ptr != 0 && s_treeImposterMatGpu[1].ptr != 0 && s_treeImposterMatGpu[2].ptr != 0;
		if (imposterReady)
		{
			out[0][1] = 6u;
			out[0][2] = 6u;
			out[1][1] = 0u;
			out[1][2] = 0u;
			out[2][1] = 0u;
			out[2][2] = 0u;
		}
		static int s_fillLog = 0;
		if (s_fillLog < 3)
		{
			DebugLog("[FillIdx] imposterReady=%d (bakeOk=%d quadVb=%p quadIb=%p mat0=%llu mat1=%llu mat2=%llu) "
				"idx[0]={%u,%u,%u} idx[1]={%u,%u,%u} idx[2]={%u,%u,%u}\n",
				imposterReady ? 1 : 0, s_treeImposterBakeOk ? 1 : 0,
				s_treeImposterQuadVb, s_treeImposterQuadIb,
				(unsigned long long)s_treeImposterMatGpu[0].ptr,
				(unsigned long long)s_treeImposterMatGpu[1].ptr,
				(unsigned long long)s_treeImposterMatGpu[2].ptr,
				out[0][0], out[0][1], out[0][2],
				out[1][0], out[1][1], out[1][2],
				out[2][0], out[2][1], out[2][2]);
			++s_fillLog;
		}
	}

	ComPtr<ID3D12Resource> s_treeImposterAtlas0;
	ComPtr<ID3D12Resource> s_treeImposterAtlas1;
	ComPtr<ID3D12Resource> s_treeImposterAtlas2;
	PostProcessSettings s_postProcessSettings;

	ComPtr<ID3D12Resource> s_pbrInstanceRingBuffer;
	InstanceData* s_pbrInstanceRingMapped = nullptr;

	DescriptorHandle* RegisterPBRMaterial(DescriptorHeap* heap, const Mesh& mesh)
	{
		Texture2D* albedoTex = Texture2D::Get(mesh.DiffuseMap);
		if (!albedoTex && !mesh.DiffuseMap.empty())
			albedoTex = Texture2D::Get(ReplaceExtension(mesh.DiffuseMap, "tga"));
		if (!albedoTex) albedoTex = Texture2D::GetWhite();
		DescriptorHandle* firstHandle = heap->Register(albedoTex);

		Texture2D* normalTex = Texture2D::Get(mesh.NormalMap);
		if (!normalTex && !mesh.NormalMap.empty())
			normalTex = Texture2D::Get(ReplaceExtension(mesh.NormalMap, "tga"));
		if (!normalTex) normalTex = Texture2D::GetWhite();
		DescriptorHandle* normalHandle = heap->Register(normalTex);

		Texture2D* metallicTex = Texture2D::Get(mesh.MetallicMap);
		if (!metallicTex && !mesh.MetallicMap.empty())
			metallicTex = Texture2D::Get(ReplaceExtension(mesh.MetallicMap, "tga"));
		if (!metallicTex) metallicTex = Texture2D::GetDefaultMetallic();
		DescriptorHandle* metallicHandle = heap->Register(metallicTex);

		Texture2D* roughnessTex = Texture2D::Get(mesh.RoughnessMap);
		if (!roughnessTex && !mesh.RoughnessMap.empty())
			roughnessTex = Texture2D::Get(ReplaceExtension(mesh.RoughnessMap, "tga"));
		if (!roughnessTex) roughnessTex = Texture2D::GetDefaultRoughness();
		DescriptorHandle* roughnessHandle = heap->Register(roughnessTex);

		Texture2D* rampTex = nullptr;
		// exists のみだと相対パス・正規化差で弾かれ、白ランプに落ちることがある → Get に任せる
		if (!mesh.RampMap.empty())
			rampTex = Texture2D::Get(mesh.RampMap);
		if (!rampTex)
		{
			static const wchar_t kDefaultRampAsset[] = L"assets\\npr\\default_ramp.png";
			if (fs::exists(kDefaultRampAsset))
				rampTex = Texture2D::Get(kDefaultRampAsset);
		}
		if (!rampTex)
			rampTex = Texture2D::GetDefaultNprRamp();

		if (g_NprGpuTuning.logNprRampPathsOnRegister)
		{
			Texture2D* const white = Texture2D::GetWhite();
			const bool pathSet = !mesh.RampMap.empty();
			const bool ptrIsWhite = (rampTex == white);
			const bool existsFs = pathSet && fs::exists(fs::path(mesh.RampMap));
			DebugLog("[NPR][Ramp] mat=\"%s\" pathSet=%d fsExists=%d texPtr==GetWhite=%d\n",
				mesh.MaterialName.c_str(), pathSet ? 1 : 0, (pathSet && existsFs) ? 1 : 0, ptrIsWhite ? 1 : 0);
			if (pathSet)
			{
				OutputDebugStringW(L"  path: ");
				OutputDebugStringW(mesh.RampMap.c_str());
				OutputDebugStringA("\n");
			}
		}

		DescriptorHandle* rampHandle = heap->Register(rampTex);

		Texture2D* sphereTex = Texture2D::Get(mesh.SphereMap);
		if (!sphereTex && !mesh.SphereMap.empty())
			sphereTex = Texture2D::Get(ReplaceExtension(mesh.SphereMap, "tga"));
		if (!sphereTex)
			sphereTex = Texture2D::GetWhite();
		DescriptorHandle* sphereHandle = heap->Register(sphereTex);

		// t0..t5 を「先頭ハンドルから連番で期待」しているため、
		// Register が失敗した場合は不整合を避ける。
		if (!firstHandle || !normalHandle || !metallicHandle || !roughnessHandle || !rampHandle || !sphereHandle)
			return nullptr;

		return firstHandle;
	}
}

Scene::~Scene()
{
	if (g_Engine)
		g_Engine->WaitForGpuIdle();

	for (auto e : m_registry.view<MeshRendererComponent>())
	{
		auto& mr = m_registry.get<MeshRendererComponent>(e);
		if (mr.OwnsGpuBuffers)
		{
			delete mr.pVB;
			delete mr.pIB;
		}
		mr.pVB = nullptr;
		mr.pIB = nullptr;
	}
	m_ownedVertexBuffers.clear();
	m_ownedIndexBuffers.clear();
	delete m_terrainSharedVB;
	delete m_terrainSharedIB;
	m_terrainSharedVB = nullptr;
	m_terrainSharedIB = nullptr;
	m_registry.clear();

	if (s_pbrInstanceRingBuffer && s_pbrInstanceRingMapped)
	{
		s_pbrInstanceRingBuffer->Unmap(0, nullptr);
		s_pbrInstanceRingMapped = nullptr;
	}
	s_pbrInstanceRingBuffer.Reset();

	for (size_t i = 0; i < Engine::FRAME_BUFFER_COUNT; ++i)
	{
		delete constantBuffer[i];
		constantBuffer[i] = nullptr;
		delete sceneConstantBuffer[i];
		sceneConstantBuffer[i] = nullptr;
		delete pbrPropertyBuffer[i];
		pbrPropertyBuffer[i] = nullptr;
		delete terrainConstantBuffer[i];
		terrainConstantBuffer[i] = nullptr;
	}

	delete s_skyboxRenderer;
	s_skyboxRenderer = nullptr;
	delete s_postProcess;
	s_postProcess = nullptr;
	delete s_hiz;
	s_hiz = nullptr;
	delete s_terrainGpuCull;
	s_terrainGpuCull = nullptr;
	delete s_treeGpuCull;
	s_treeGpuCull = nullptr;
	if (s_shadow) { s_shadow->Shutdown(); delete s_shadow; s_shadow = nullptr; }
	if (s_atmosphere) { s_atmosphere->Shutdown(); delete s_atmosphere; s_atmosphere = nullptr; }

	s_hdrSrvHandle = nullptr;
	s_envCubemapHandle = nullptr;
	s_terrainMaskHandle = nullptr;
	s_hizPyramidSrvHandle = nullptr;

	skyboxCubemap.Reset();
	skyboxEquirect.Reset();
	s_irradianceCubemap.Reset();
	s_prefilterCubemap.Reset();
	s_brdfLut.Reset();

	delete rootSignature;
	rootSignature = nullptr;
	delete pipelineState;
	pipelineState = nullptr;
	delete nprPipelineState;
	nprPipelineState = nullptr;
	delete nprTransparentPipelineState;
	nprTransparentPipelineState = nullptr;
	delete terrainRootSignature;
	terrainRootSignature = nullptr;
	delete terrainDepthPrepassPipelineState;
	terrainDepthPrepassPipelineState = nullptr;
	delete terrainPipelineState;
	terrainPipelineState = nullptr;
	delete treeOpaquePipelineState;
	treeOpaquePipelineState = nullptr;
	delete treeLod1PipelineState;
	treeLod1PipelineState = nullptr;
	delete treeLod2PipelineState;
	treeLod2PipelineState = nullptr;
	delete treeImposterPipelineState;
	treeImposterPipelineState = nullptr;

	s_treeImposterAtlas0.Reset();
	s_treeImposterAtlas1.Reset();
	s_treeImposterAtlas2.Reset();
	s_treeImposterBakeOk = false;
	s_treeImposterQuadVb = nullptr;
	s_treeImposterQuadIb = nullptr;
	for (int i = 0; i < 3; ++i)
	{
		s_treeImposterMatTableStart[i] = nullptr;
		s_treeImposterMatGpu[i] = {};
	}

	delete descriptorHeap;
	descriptorHeap = nullptr;
	delete g_Camera;
	g_Camera = nullptr;
}

//const wchar_t* modelFile = L"assets\\hibana\\hibana.pmx";
const float modelScale = 1.0f;

bool Scene::SpawnLoadedMeshes(const wchar_t* path, std::vector<Mesh>&& loadedMeshes,
	const DirectX::XMFLOAT4X4& baseMatrix, const ModelSpawnOptions& opt,
	const ModelBounds* precomputedBounds, size_t* outEntitiesSpawned, XMFLOAT3* outFinalPosition)
{
	if (outEntitiesSpawned)
		*outEntitiesSpawned = 0;
	if (outFinalPosition)
		*outFinalPosition = { 0.f, 0.f, 0.f };
	if (!descriptorHeap || !descriptorHeap->GetHeap())
		return false;

	const ModelBounds bounds = precomputedBounds ? *precomputedBounds : ComputeModelBounds(loadedMeshes);
	const float sourceHeight = bounds.Max.y - bounds.Min.y;
	const float groundOffset = -bounds.Min.y * opt.uniformScale;

	XMFLOAT3 worldPos = opt.position;
	if (opt.foot == ModelSpawnOptions::FootPlacement::SnapFeetToTerrain)
	{
		const float gy = TerrainSystem::GetHeight(m_registry, worldPos.x, worldPos.z);
		worldPos.y = gy + groundOffset;
	}

	if (opt.addPlayerComponent)
	{
		DebugLog("[SpawnModel] %ls meshes=%zu player=Y pos=(%.2f,%.2f,%.2f) scale=%.3f groundOff=%.3f\n",
			path, loadedMeshes.size(), worldPos.x, worldPos.y, worldPos.z, opt.uniformScale, groundOffset);
	}

	size_t nToSpawn = 0;
	for (size_t i = 0; i < loadedMeshes.size(); ++i)
	{
		const Mesh& m = loadedMeshes[i];
		if (!m.Vertices.empty() && !m.Indices.empty())
			++nToSpawn;
	}
	if (nToSpawn == 0)
	{
		DebugLog("[SpawnLoadedMeshes] %ls: Assimp meshes=%zu -> spawned 0 (all empty or no triangles)\n",
			path, loadedMeshes.size());
		return false;
	}

	const entt::entity parentEnt = m_registry.create();
	{
		TransformComponent ptc{};
		ptc.BaseMatrix = baseMatrix;
		ptc.Position = worldPos;
		ptc.UniformScale = opt.uniformScale;
		ptc.RotationY = opt.rotationY;
		ptc.WorldMatrix = XMMatrixIdentity();
		m_registry.emplace<TransformComponent>(parentEnt, ptc);
		auto& root = m_registry.emplace<ModelGroupRootComponent>(parentEnt);
		root.children.reserve(nToSpawn);
		root.combinedModelBounds = bounds;
		root.hasCombinedModelBounds = IsValidModelBounds(bounds);
		m_registry.emplace<EditorHierarchyLabelComponent>(parentEnt,
			EditorHierarchyLabelComponent{ fs::path(path).filename().wstring() });
		m_registry.emplace<LODComponent>(parentEnt, 0, 0.0f);
		if (opt.addNprTag)
			m_registry.emplace<NPRTag>(parentEnt);
		if (opt.addPlayerComponent)
		{
			PlayerComponent pc = {};
			pc.Height = sourceHeight;
			pc.GroundOffset = groundOffset;
			pc.FollowCamera = false;
			m_registry.emplace<PlayerComponent>(parentEnt, pc);
		}
	}
	auto& rootChildren = m_registry.get<ModelGroupRootComponent>(parentEnt).children;

	auto destroyModelGroup = [&]() {
		for (const entt::entity c : rootChildren)
			m_registry.destroy(c);
		rootChildren.clear();
		m_registry.destroy(parentEnt);
	};

	size_t spawned = 0;
	for (size_t i = 0; i < loadedMeshes.size(); ++i)
	{
		Mesh& m = loadedMeshes[i];
		if (m.Vertices.empty() || m.Indices.empty())
		{
			DebugLog("[SpawnLoadedMeshes] %ls mesh[%zu] skip (empty vert/idx)\n", path, i);
			continue;
		}

		auto* pVB = new VertexBuffer(sizeof(Vertex) * m.Vertices.size(), sizeof(Vertex), m.Vertices.data());
		auto* pIB = new IndexBuffer(sizeof(uint32_t) * m.Indices.size(), m.Indices.data());
		if (!pVB->IsValid() || !pIB->IsValid())
		{
			DebugLog("[SpawnLoadedMeshes] %ls mesh[%zu] VB/IB create failed\n", path, i);
			destroyModelGroup();
			if (outEntitiesSpawned)
				*outEntitiesSpawned = 0;
			return false;
		}

		m_ownedVertexBuffers.push_back(pVB);
		m_ownedIndexBuffers.push_back(pIB);

		DescriptorHandle* matHandle = RegisterPBRMaterial(descriptorHeap, m);
		if (!matHandle)
		{
			DebugLog("[SpawnLoadedMeshes] %ls mesh[%zu] RegisterPBRMaterial failed (SRV heap full?)\n", path, i);
			destroyModelGroup();
			if (outEntitiesSpawned)
				*outEntitiesSpawned = 0;
			return false;
		}

		const auto entity = m_registry.create();
		TransformComponent tc = {};
		XMStoreFloat4x4(&tc.BaseMatrix, XMMatrixIdentity());
		tc.Position = { 0.0f, 0.0f, 0.0f };
		tc.UniformScale = 1.0f;
		tc.RotationY = 0.0f;
		tc.WorldMatrix = XMMatrixIdentity();
		m_registry.emplace<TransformComponent>(entity, tc);
		m_registry.emplace<ModelGroupChildComponent>(entity, ModelGroupChildComponent{ parentEnt });

		MeshRendererComponent mrc = {};
		mrc.pVB = pVB;
		mrc.pIB = pIB;
		mrc.IndexCount = static_cast<UINT>(m.Indices.size());
		mrc.MaterialHandle = matHandle;
		mrc.CastShadow = true;
		mrc.LocalBounds = ComputeMeshBounds(m);
		mrc.HasLocalBounds = IsValidModelBounds(mrc.LocalBounds);
		// スキンメッシュは頂点がバインド空間に近く、CPU AABB+親 World では枝先の花が視錐外扱いで消える
		mrc.SkipCpuFrustumCull = !m.Bones.empty();
		mrc.NprTransparent = opt.addNprTag && m.NprTransparentByRule;
		mrc.NprCelVertexBlendOverride = m.NprCelVertexBlendOverride;
		mrc.NprSphereMode = m.SphereMode;
		mrc.NprOpacity = m.Opacity;
		m_registry.emplace<MeshRendererComponent>(entity, mrc);
		m_registry.emplace<LODComponent>(entity, 0, 0.0f);
		m_registry.emplace<EditorHierarchyLabelComponent>(entity,
			EditorHierarchyLabelComponent{ L"Part [" + std::to_wstring(i) + L"]" });
		rootChildren.push_back(entity);
		++spawned;
	}

	if (spawned == 0)
		DebugLog("[SpawnLoadedMeshes] %ls: Assimp meshes=%zu -> spawned 0 (all empty or no triangles)\n",
			path, loadedMeshes.size());

	if (outEntitiesSpawned)
		*outEntitiesSpawned = spawned;
	if (spawned > 0 && outFinalPosition)
		*outFinalPosition = worldPos;
	return spawned > 0;
}

bool Scene::SpawnModelEntities(const wchar_t* path, const ModelSpawnOptions& opt)
{
	std::vector<Mesh> loadedMeshes;
	ImportSettings import(path, loadedMeshes, false, true, 1.0f);
	import.outClips = nullptr;

	AssimpLoader loader;
	if (!loader.Load(import))
		return false;

	return SpawnLoadedMeshes(path, std::move(loadedMeshes), import.outBaseTransform, opt, nullptr);
}

void Scene::RequestDestroyEntity(entt::entity root)
{
	if (m_registry.valid(root))
		m_pendingDestroy.push_back(root);
}

void Scene::ProcessAsyncModelLoads()
{
	if (!g_AsyncModelLoader)
		return;

	// 1フレームでの GPU リソース生成(VB/IB) + SRV 登録を分散してスパイクを抑える
	// (Assimp パース自体はワーカー側で完了している前提)
	const size_t spawnBudget = std::max<size_t>(1, m_asyncSpawnBudgetPerFrame);

	g_AsyncModelLoader->DrainCompleted(spawnBudget, [this](AsyncModelLoadResult&& r) {
		if (r.filePath.empty())
			return;
		if (!r.success)
		{
			DebugLog("[AsyncSpawn] %ls ABORT before spawn: Assimp/import failed (no meshes on main thread)\n",
				r.filePath.c_str());
			g_AsyncModelLoader->ReportSpawnResult(r.filePath, false, 0,
				L"Stage: Assimp/import failed (see console [AsyncLoad] FAIL). Path or format?",
				0.f, 0.f, 0.f);
			return;
		}
		size_t n = 0;
		XMFLOAT3 finalPos{};
		const bool spawned = SpawnLoadedMeshes(r.filePath.c_str(), std::move(r.meshes), r.baseTransform,
			r.options, &r.bounds, &n, &finalPos);
		if (spawned)
			DebugLog("[AsyncSpawn] %ls entities=%zu worldPos=(%.2f,%.2f,%.2f)\n",
				r.filePath.c_str(), n, finalPos.x, finalPos.y, finalPos.z);
		const wchar_t* detail = L"";
		if (!spawned)
		{
			if (!descriptorHeap || !descriptorHeap->GetHeap())
				detail = L"Stage: descriptorHeap invalid (Scene init).";
			else if (n == 0)
				detail = L"Stage: no drawable submeshes (empty vert/idx), or SRV Register returned null (heap cap).";
			else
				detail = L"Stage: stopped mid-model (VB/IB fail or SRV full). See console mesh index.";
		}
		g_AsyncModelLoader->ReportSpawnResult(r.filePath, spawned, n,
			spawned ? L"" : detail,
			finalPos.x, finalPos.y, finalPos.z);
	});
}

void Scene::TryEnsureTreeImposterBake()
{
	if (s_treeImposterBakeOk)
		return;
	if (!descriptorHeap || !rootSignature || !treeImposterPipelineState || !treeImposterPipelineState->IsValid()
		|| !pbrPropertyBuffer[0])
		return;
	if (TreeVegetation::GetMergedIndexCountLod(0) == 0)
		return;

	if (!s_treeImposterQuadVb || !s_treeImposterQuadIb)
	{
		VertexBuffer* qvb = nullptr;
		IndexBuffer* qib = nullptr;
		if (!TreeImposterBake::CreateQuadMeshes(&qvb, &qib) || !qvb || !qib)
			return;
		m_ownedVertexBuffers.push_back(qvb);
		m_ownedIndexBuffers.push_back(qib);
		s_treeImposterQuadVb = qvb;
		s_treeImposterQuadIb = qib;
	}

	const D3D12_GPU_DESCRIPTOR_HANDLE iblGpu = s_envCubemapHandle ? s_envCubemapHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{};
	DescriptorHandle* matStart[3] = {};
	const TreeSpeciesMaterials* sm0 = TreeVegetation::GetSpeciesMaterials(0);
	const TreeSpeciesMaterials* sm1 = TreeVegetation::GetSpeciesMaterials(1);
	const TreeSpeciesMaterials* sm2 = TreeVegetation::GetSpeciesMaterials(2);
	if (TreeImposterBake::BakeAtlases(
		descriptorHeap,
		rootSignature->Get(),
		pbrPropertyBuffer[0]->GetAddress(),
		iblGpu,
		TreeVegetation::GetMergedVertexBufferLod(0),
		TreeVegetation::GetMergedIndexBufferLod(0),
		TreeVegetation::GetMergedIndexCountLod(0),
		TreeVegetation::GetMergedLocalBounds(),
		sm0, sm1, sm2,
		s_treeImposterAtlas0, s_treeImposterAtlas1, s_treeImposterAtlas2,
		matStart))
	{
		s_treeImposterBakeOk = true;
		for (int i = 0; i < 3; ++i)
		{
			s_treeImposterMatTableStart[i] = matStart[i];
			if (matStart[i])
				s_treeImposterMatGpu[i] = matStart[i]->HandleGPU;
		}
	}
	else
		DebugLog("[Scene] Tree imposter bake failed.\n");
}

void Scene::TryEnsureTreeGpuCullInit()
{
	if (s_treeGpuCull)
		return;
	if (TreeVegetation::GetMergedIndexCountLod(0) == 0)
		return;
	if (!g_Engine || !descriptorHeap)
		return;
	s_treeGpuCull = new TreeGpuCullSystem();
	// マスク全セルは数十万本になり得る。256 のみだと初回はクランプされ、直後に再 Init が走る。
	const uint32_t maxInst = 262144u;
	if (!s_treeGpuCull->Init(g_Engine->Device(), descriptorHeap, rootSignature ? rootSignature->Get() : nullptr, maxInst))
	{
		delete s_treeGpuCull;
		s_treeGpuCull = nullptr;
	}
}

void Scene::SetAsyncSpawnBudgetPerFrame(size_t budget)
{
	m_asyncSpawnBudgetPerFrame = std::max<size_t>(1, budget);
}

size_t Scene::GetAsyncSpawnBudgetPerFrame() const
{
	return std::max<size_t>(1, m_asyncSpawnBudgetPerFrame);
}

bool Scene::InitDescriptorHeap()
{
	descriptorHeap = new DescriptorHeap();
	return descriptorHeap && descriptorHeap->GetHeap();
}

bool Scene::InitCameraAndFrameBuffers()
{
	g_Camera = new Camera();
	g_Camera->SetPosition(XMVectorSet(0.0f, 1.2f, 2.5f, 0.0f));

	for (size_t i = 0; i < Engine::FRAME_BUFFER_COUNT; i++)
	{
		constantBuffer[i] = new ConstantBuffer(kPerDrawTransformCBBytes);
		if (!constantBuffer[i]->IsValid())
			return false;

		sceneConstantBuffer[i] = new ConstantBuffer(sizeof(SceneConstants));
		if (!sceneConstantBuffer[i]->IsValid())
			return false;

		pbrPropertyBuffer[i] = new ConstantBuffer(sizeof(PBRConstants));
		if (!pbrPropertyBuffer[i]->IsValid())
			return false;
		auto pbr = pbrPropertyBuffer[i]->GetPtr<PBRConstants>();
		pbr->RimParams = XMFLOAT4(1.0f, g_NprGpuTuning.normalScale,
			g_NprGpuTuning.rimPower, g_NprGpuTuning.rimStrength);
		pbr->CameraPos = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		pbr->NprTuning = XMFLOAT4(g_NprGpuTuning.virtualLight, 0.0f,
			g_NprGpuTuning.opaqueAlphaClip, g_NprGpuTuning.ambientShadowStrength);
		pbr->NprTuning2 = XMFLOAT4(g_NprGpuTuning.celVertexNormalBlend, g_NprGpuTuning.celShadeSharpness,
			g_NprGpuTuning.rimVertexNormalBlend, static_cast<float>(g_NprGpuTuning.nprDebugRampView));
		pbr->NprDebugHdr = XMFLOAT4(0.f, 0.f, 0.f, 0.f);
	}
	return InitPbrInstanceRingBuffer();
}

bool Scene::InitPbrInstanceRingBuffer()
{
	static_assert(kPbrInstanceRingFrameCount == 2, "Ring frames must match Engine::FRAME_BUFFER_COUNT");
	const UINT64 byteSize = sizeof(InstanceData) * kMaxPbrInstancesPerFrame * kPbrInstanceRingFrameCount;
	const auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	const auto resDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

	HRESULT hr = g_Engine->Device()->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(s_pbrInstanceRingBuffer.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		printf("Scene::InitPbrInstanceRingBuffer: CreateCommittedResource failed (0x%08X)\n", static_cast<unsigned>(hr));
		return false;
	}

	void* mapped = nullptr;
	hr = s_pbrInstanceRingBuffer->Map(0, nullptr, &mapped);
	if (FAILED(hr) || !mapped)
	{
		printf("Scene::InitPbrInstanceRingBuffer: Map failed\n");
		s_pbrInstanceRingBuffer.Reset();
		return false;
	}
	s_pbrInstanceRingMapped = static_cast<InstanceData*>(mapped);
	return true;
}

bool Scene::InitTerrain()
{
	TerrainGenerateResult terrainResult = {};
	if (!TerrainGenerator_GenerateFromExr("assets/terrain/Weathering_Out.exr", kTerrainCellSpacing, kTerrainMaxHeight, terrainResult))
	{
		if (!TerrainGenerator_GenerateFromFile(L"assets\\terrain_heightmap.png", kTerrainCellSpacing, kTerrainMaxHeight, terrainResult))
			terrainResult = {};
	}
	if (!terrainResult.pVB || !terrainResult.pIB)
		return true;

	auto resolvePath = [](const wchar_t* path) -> std::wstring
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
	};
	auto loadTextureOrFallback = [&resolvePath](const wchar_t* path, Texture2D* fallback) -> Texture2D*
	{
		const std::wstring resolved = resolvePath(path);
		if (fs::exists(resolved))
			return Texture2D::Get(resolved);
		DebugLog("[TerrainTex] missing: %ls\n", path);
		return fallback;
	};

	static const wchar_t* terrainTexturePaths[] = {
		L"assets\\terrain\\tree_mask.png",
		L"assets\\terrain\\nature_mask.png",
		L"assets\\terrain\\Ground\\textures\\coast_sand_rocks_02_diff_4k.jpg",
		L"assets\\terrain\\Ground\\textures\\coast_sand_rocks_02_disp_4k.png",
	};
	// t0: tree_mask, t1: nature_mask, t2: ground_diff, t3: ground_disp
	Texture2D* terrainTex[4] = {
		loadTextureOrFallback(terrainTexturePaths[0], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainTexturePaths[1], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainTexturePaths[2], Texture2D::GetWhite()),
		loadTextureOrFallback(terrainTexturePaths[3], Texture2D::GetBlack())
	};
	s_terrainMaskHandle = nullptr;
	for (Texture2D* tex : terrainTex)
	{
		DescriptorHandle* h = descriptorHeap->Register(tex);
		if (!s_terrainMaskHandle)
			s_terrainMaskHandle = h;
	}

	terrainRootSignature = new RootSignature(true);
	terrainDepthPrepassPipelineState = new PipelineState();
	terrainDepthPrepassPipelineState->SetInputLayout(Vertex::InputLayout);
	terrainDepthPrepassPipelineState->SetRootSignature(terrainRootSignature->Get());
	terrainDepthPrepassPipelineState->SetVS(L"TerrainVS.cso");
	terrainDepthPrepassPipelineState->SetCullMode(D3D12_CULL_MODE_NONE);
	terrainDepthPrepassPipelineState->SetNumRenderTargets(0);
	terrainDepthPrepassPipelineState->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ALL);
	terrainDepthPrepassPipelineState->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
	terrainDepthPrepassPipelineState->Create();

	terrainPipelineState = new PipelineState();
	terrainPipelineState->SetInputLayout(Vertex::InputLayout);
	terrainPipelineState->SetRootSignature(terrainRootSignature->Get());
	terrainPipelineState->SetVS(L"TerrainVS.cso");
	terrainPipelineState->SetPS(L"Terrain_PS.cso");
	terrainPipelineState->SetCullMode(D3D12_CULL_MODE_NONE);
	if (terrainDepthPrepassPipelineState && terrainDepthPrepassPipelineState->IsValid())
	{
		terrainPipelineState->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ZERO);
		terrainPipelineState->SetDepthFunc(D3D12_COMPARISON_FUNC_EQUAL);
	}
	terrainPipelineState->Create();

	for (size_t i = 0; i < Engine::FRAME_BUFFER_COUNT; i++)
	{
		terrainConstantBuffer[i] = new ConstantBuffer(sizeof(TerrainConstants));
		if (!terrainConstantBuffer[i]->IsValid())
			continue;
		auto* tc = terrainConstantBuffer[i]->GetPtr<TerrainConstants>();
		tc->LayerColor[0] = XMFLOAT4(0.35f, 0.28f, 0.2f, 1.0f);
		tc->LayerColor[1] = XMFLOAT4(0.10f, 0.28f, 0.08f, 1.0f);
		tc->LayerColor[2] = XMFLOAT4(0.18f, 0.33f, 0.12f, 1.0f);
		tc->LayerColor[3] = XMFLOAT4(0.24f, 0.40f, 0.18f, 1.0f);
		tc->LayerColor[4] = XMFLOAT4(0.95f, 0.95f, 1.0f, 1.0f);
		tc->LayerColor[5] = XMFLOAT4(0.20f, 0.40f, 0.70f, 1.0f);
		tc->CameraPos = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
		tc->DebugParams = XMFLOAT4(static_cast<float>(m_terrainPsDebugMode), 0.0f, 0.0f, 0.0f);
	}

	if (!terrainPipelineState->IsValid())
		return true;

	m_terrainSharedVB = terrainResult.pVB;
	m_terrainSharedIB = terrainResult.pIB;

	const auto terrainRoot = m_registry.create();
	TransformComponent tcTerrain = {};
	XMStoreFloat4x4(&tcTerrain.BaseMatrix, XMMatrixIdentity());
	tcTerrain.Position = { 0.0f, 0.0f, 0.0f };
	tcTerrain.UniformScale = 1.0f;
	tcTerrain.RotationY = 0.0f;
	tcTerrain.WorldMatrix = XMMatrixIdentity();
	m_registry.emplace<TransformComponent>(terrainRoot, tcTerrain);

	TerrainComponent terrComp = {};
	terrComp.HeightData = std::move(terrainResult.HeightData);
	terrComp.GridWidth = terrainResult.GridWidth;
	terrComp.GridDepth = terrainResult.GridDepth;
	terrComp.CellSpacing = kTerrainCellSpacing;
	terrComp.MaxHeight = kTerrainMaxHeight;
	m_registry.emplace<TerrainComponent>(terrainRoot, terrComp);
	m_registry.emplace<EditorHierarchyLabelComponent>(terrainRoot, EditorHierarchyLabelComponent{ L"Terrain (root)" });

	for (const TerrainChunkDesc& chunk : terrainResult.Chunks)
	{
		const auto chunkEnt = m_registry.create();
		m_registry.emplace<TransformComponent>(chunkEnt, tcTerrain);
		MeshRendererComponent mrc = {};
		mrc.pVB = m_terrainSharedVB;
		mrc.pIB = m_terrainSharedIB;
		mrc.IndexCount = chunk.IndexCount[0];
		mrc.StartIndexLocation = chunk.StartIndex[0];
		mrc.OwnsGpuBuffers = false;
		mrc.MaterialHandle = s_terrainMaskHandle;
		mrc.CastShadow = true;
		mrc.HasLocalBounds = true;
		mrc.LocalBounds = chunk.LocalBounds;
		m_registry.emplace<MeshRendererComponent>(chunkEnt, mrc);
		m_registry.emplace<TerrainMeshTag>(chunkEnt);
		m_registry.emplace<LODComponent>(chunkEnt, 0, 0.0f);
	}

	if (!terrainResult.Chunks.empty() && g_Engine)
	{
		s_terrainGpuCull = new TerrainGpuCullSystem();
		if (!s_terrainGpuCull->Init(g_Engine->Device(), descriptorHeap, terrainResult.Chunks))
		{
			delete s_terrainGpuCull;
			s_terrainGpuCull = nullptr;
		}
	}

	return true;
}

bool Scene::InitMainPipeline()
{
	rootSignature = new RootSignature();
	pipelineState = new PipelineState();
	pipelineState->SetInputLayout(Vertex::InputLayout);
	pipelineState->SetRootSignature(rootSignature->Get());
	pipelineState->SetVS(L"SimpleVS.cso");
	pipelineState->SetPS(L"StandardPBR_PS.cso");
	pipelineState->Create();
	if (!pipelineState->IsValid())
		return false;

	nprPipelineState = new PipelineState();
	nprPipelineState->SetInputLayout(Vertex::InputLayout);
	nprPipelineState->SetRootSignature(rootSignature->Get());
	nprPipelineState->SetVS(L"SimpleVS.cso");
	nprPipelineState->SetPS(L"NPR_PS.cso");
	nprPipelineState->SetCullMode(D3D12_CULL_MODE_BACK);
	nprPipelineState->Create();
	if (!nprPipelineState->IsValid())
		DebugLog("[Scene] NPR opaque PSO invalid (NPR_PS.cso?). Toon path disabled; model falls back to PBR.\n");

	nprTransparentPipelineState = new PipelineState();
	nprTransparentPipelineState->SetInputLayout(Vertex::InputLayout);
	nprTransparentPipelineState->SetRootSignature(rootSignature->Get());
	nprTransparentPipelineState->SetVS(L"SimpleVS.cso");
	nprTransparentPipelineState->SetPS(L"NPR_PS_Transparent.cso");
	// 板ポリの目・ハイライト等が欠けないよう半透明パスは両面
	nprTransparentPipelineState->SetCullMode(D3D12_CULL_MODE_NONE);
	nprTransparentPipelineState->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ZERO);
	nprTransparentPipelineState->SetAlphaBlendPremultiplied();
	nprTransparentPipelineState->Create();

	// Trees: use a dedicated VS that reads visible-index indirection.
	treeOpaquePipelineState = new PipelineState();
	treeOpaquePipelineState->SetInputLayout(Vertex::InputLayout);
	treeOpaquePipelineState->SetRootSignature(rootSignature->Get());
	treeOpaquePipelineState->SetVS(L"TreeIndirectVS.cso");
	treeOpaquePipelineState->SetPS(L"StandardPBR_PS.cso");
	treeOpaquePipelineState->SetCullMode(D3D12_CULL_MODE_NONE);
	treeOpaquePipelineState->Create();
	if (!treeOpaquePipelineState->IsValid())
		DebugLog("[Scene] Tree opaque PSO invalid.\n");


	treeLod1PipelineState = new PipelineState();
	treeLod1PipelineState->SetInputLayout(Vertex::InputLayout);
	treeLod1PipelineState->SetRootSignature(rootSignature->Get());
	treeLod1PipelineState->SetVS(L"TreeIndirectVS.cso");
	treeLod1PipelineState->SetPS(L"TreeLOD1_PS.cso");
	treeLod1PipelineState->Create();
	if (!treeLod1PipelineState->IsValid())
		DebugLog("[Scene] Tree LOD1 PSO invalid.\n");

	treeLod2PipelineState = new PipelineState();
	treeLod2PipelineState->SetInputLayout(Vertex::InputLayout);
	treeLod2PipelineState->SetRootSignature(rootSignature->Get());
	treeLod2PipelineState->SetVS(L"TreeIndirectVS.cso");
	treeLod2PipelineState->SetPS(L"TreeLOD2_PS.cso");
	treeLod2PipelineState->Create();
	if (!treeLod2PipelineState->IsValid())
		DebugLog("[Scene] Tree LOD2 PSO invalid.\n");

	// LOD1 インポスター（クワッド + 焼きアトラス）。未生成だとベイク条件も DrawIndirect の imposter 経路も常に無効になる。
	treeImposterPipelineState = new PipelineState();
	treeImposterPipelineState->SetInputLayout(Vertex::InputLayout);
	treeImposterPipelineState->SetRootSignature(rootSignature->Get());
	treeImposterPipelineState->SetVS(L"TreeImposterVS.cso");
	treeImposterPipelineState->SetPS(L"TreeImposterPS.cso");
	treeImposterPipelineState->SetCullMode(D3D12_CULL_MODE_NONE);
	treeImposterPipelineState->Create();
	if (!treeImposterPipelineState->IsValid())
		DebugLog("[Scene] Tree imposter PSO invalid (TreeImposterVS/PS.cso?). LOD1 falls back to full mesh.\n");

	return true;
}

bool Scene::InitSkyboxAndIBL()
{
	DebugLog("[Skybox] --- init begin ---\n");
	g_Engine->Allocator(0)->Reset();
	g_Engine->MainGraphicsCmdList()->Reset(g_Engine->Allocator(0), nullptr);

	ID3D12Resource* cubemap = nullptr;
	ID3D12Resource* equirect = nullptr;
	IBLGenerator ibl;
	const auto doExecuteAndWait = []() { g_Engine->ExecuteAndWait(); };

	// NOTE: 2560 cubemap は環境によっては初期化時に TDR を踏みやすい。まず 512 で安定化（必要なら後で戻す）。
	if (ibl.Generate(g_Engine->Device(), g_Engine->MainGraphicsCmdList(), L"assets\\skybox.exr", 512u, &cubemap, doExecuteAndWait, &equirect))
	{
		skyboxCubemap = cubemap;
		skyboxEquirect = equirect;
		DebugLog("[Skybox] Generate OK (cubemap + equirect).\n");
	}
	else
	{
		DebugLog("[Skybox] Generate failed. Creating fallback...\n");
		ID3D12Resource* defaultCubemap = nullptr;
		ComPtr<ID3D12Resource> defaultUpload;
		if (IBLGenerator::CreateDefaultCubemap(g_Engine->Device(), g_Engine->MainGraphicsCmdList(), 4, 0.25f, 0.45f, 0.85f, 1.0f, &defaultCubemap, defaultUpload.GetAddressOf()))
		{
			skyboxCubemap = defaultCubemap;
			g_Engine->ExecuteAndWait();
		}
	}

	// IBL/Compute の実行が完全に終わったことを保証してから allocator を Reset する
	// (ここは初期化フェーズ限定で、毎フレーム直列化はしない)
	g_Engine->WaitForGpuIdle();

	if (skyboxCubemap.Get())
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDescCube = {};
		srvDescCube.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		srvDescCube.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDescCube.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		srvDescCube.TextureCube.MipLevels = 1;

		g_Engine->WaitForGpuIdle();
		g_Engine->Allocator(0)->Reset();
		g_Engine->MainGraphicsCmdList()->Reset(g_Engine->Allocator(0), nullptr);
		ID3D12Resource* irradianceRaw = nullptr;
		if (ibl.GenerateIrradianceMap(g_Engine->Device(), g_Engine->MainGraphicsCmdList(), skyboxCubemap.Get(), doExecuteAndWait, &irradianceRaw))
			s_irradianceCubemap = irradianceRaw;

		g_Engine->WaitForGpuIdle();
		g_Engine->Allocator(0)->Reset();
		g_Engine->MainGraphicsCmdList()->Reset(g_Engine->Allocator(0), nullptr);
		ID3D12Resource* prefilterRaw = nullptr;
		if (ibl.GeneratePrefilteredEnvMap(g_Engine->Device(), g_Engine->MainGraphicsCmdList(), skyboxCubemap.Get(), doExecuteAndWait, &prefilterRaw))
			s_prefilterCubemap = prefilterRaw;

		g_Engine->WaitForGpuIdle();
		g_Engine->Allocator(0)->Reset();
		g_Engine->MainGraphicsCmdList()->Reset(g_Engine->Allocator(0), nullptr);
		ID3D12Resource* brdfLutRaw = nullptr;
		if (ibl.GenerateBrdfLut(g_Engine->Device(), g_Engine->MainGraphicsCmdList(), doExecuteAndWait, &brdfLutRaw))
			s_brdfLut = brdfLutRaw;

		srvDescCube.TextureCube.MipLevels = 5;
		if (s_prefilterCubemap.Get())
			s_envCubemapHandle = descriptorHeap->RegisterResource(s_prefilterCubemap.Get(), srvDescCube);
		else
			s_envCubemapHandle = descriptorHeap->RegisterResource(skyboxCubemap.Get(), srvDescCube);
		srvDescCube.TextureCube.MipLevels = 1;
		if (s_irradianceCubemap.Get())
			descriptorHeap->RegisterResource(s_irradianceCubemap.Get(), srvDescCube);
		else
			descriptorHeap->RegisterResource(skyboxCubemap.Get(), srvDescCube);
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2D = {};
		srvDesc2D.Format = DXGI_FORMAT_R16G16_FLOAT;
		srvDesc2D.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc2D.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc2D.Texture2D.MipLevels = 1;
		if (s_brdfLut.Get())
			descriptorHeap->RegisterResource(s_brdfLut.Get(), srvDesc2D);
		else
		{
			Texture2D* fallback = Texture2D::GetWhite();
			if (fallback)
			{
				srvDesc2D.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				descriptorHeap->RegisterResource(fallback->Resource(), srvDesc2D);
			}
		}
	}

	if (skyboxEquirect.Get())
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		DescriptorHandle* skyboxHandle = descriptorHeap->RegisterResource(skyboxEquirect.Get(), srvDesc);
		if (skyboxHandle)
		{
			s_skyboxRenderer = new SkyboxRenderer();
			if (s_skyboxRenderer->Init(g_Engine->Device(), skyboxEquirect.Get(), skyboxHandle->HandleGPU))
				DebugLog("[Skybox] SkyboxRenderer::Init OK (equirect 2D).\n");
			else
			{
				delete s_skyboxRenderer;
				s_skyboxRenderer = nullptr;
			}
		}
	}
	// 初期化フェーズの最後に全 GPU 作業完了を保証しておく
	// （これ以降のフレーム先頭で command allocator を Reset するため）
	g_Engine->WaitForGpuIdle();
	return true;
}

bool Scene::InitShadowSystem()
{
	if (!g_Engine || !descriptorHeap) return true;
	s_shadow = new ShadowSystem();
	if (!s_shadow->Init(g_Engine->Device(), descriptorHeap))
	{
		delete s_shadow;
		s_shadow = nullptr;
		DebugLog("[Shadow] ShadowSystem::Init failed. Shadows disabled.\n");
	}
	else
		DebugLog("[Shadow] ShadowSystem::Init OK (%ux%u, %u cascades).\n",
			ShadowSystem::kShadowMapSize, ShadowSystem::kShadowMapSize, ShadowSystem::kCascadeCount);
	return true;
}

bool Scene::InitAtmosphereSystem()
{
	if (!g_Engine || !descriptorHeap) return true;
	s_atmosphere = new AtmosphereSystem();
	if (!s_atmosphere->Init(g_Engine->Device(), descriptorHeap,
		g_Engine->GetFrameBufferWidth(), g_Engine->GetFrameBufferHeight(),
		g_Engine->GetDepthStencilResource()))
	{
		delete s_atmosphere;
		s_atmosphere = nullptr;
		DebugLog("[Atmosphere] AtmosphereSystem::Init failed. Fog/volumetric disabled.\n");
	}
	else
		DebugLog("[Atmosphere] AtmosphereSystem::Init OK.\n");
	return true;
}

bool Scene::InitPostProcess()
{
	s_postProcessSettings.gamma = 2.2f;
	// NPR+PBR 同一 HDR: Bloom は「閾値超え差分」のみ＋ソフトニー。露出はやや抑え気味。
	s_postProcessSettings.bloomIntensity = 0.38f;
	s_postProcessSettings.threshold = 1.15f;
	s_postProcessSettings.bloomKnee = 0.55f;
	s_postProcessSettings.exposure = 0.88f;
	s_postProcessSettings.blurSize = 2.0f;
	s_postProcessSettings.nprPostExposure = 1.0f;
	s_postProcessSettings.nprPostGamma = 2.2f;
	ID3D12Resource* hdrRes = g_Engine->GetHdrColorResource();
	if (hdrRes)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		s_hdrSrvHandle = descriptorHeap->RegisterResource(hdrRes, srvDesc);
	}
	s_postProcess = new PostProcessSystem();
	UINT w = g_Engine->GetFrameBufferWidth();
	UINT h = g_Engine->GetFrameBufferHeight();
	if (s_postProcess && !s_postProcess->Init(g_Engine->Device(), descriptorHeap, w, h, g_Engine->GetNprHdrColorResource()))
	{
		delete s_postProcess;
		s_postProcess = nullptr;
	}
	return true;
}

bool Scene::Init()
{
	// Warm-up: load frequently used shader bytecode early to reduce first-use stutter.
	PipelineState::WarmupShaderBytecode({
		L"TerrainVS.cso",
		L"Terrain_PS.cso",
		L"SimpleVS.cso",
		L"StandardPBR_PS.cso",
		L"NPR_PS.cso",
		L"NPR_PS_Transparent.cso",
		L"BakeTreeLOD0_PS.cso",
		L"TreeImposterVS.cso",
		L"TreeImposterPS.cso"
	});

	if (!InitDescriptorHeap())
		return false;
	if (!InitCameraAndFrameBuffers())
		return false;

	//{
	//	ModelSpawnOptions showcase = {};
	//	showcase.position = { 0.0f, 0.0f, 0.0f };
	//	showcase.uniformScale = modelScale;
	//	showcase.rotationY = 0.0f;
	//	showcase.foot = ModelSpawnOptions::FootPlacement::None;
	//	showcase.addPlayerComponent = false;
	//	if (!SpawnModelEntities(modelFile, showcase))
	//		return false;
	//}

	if (!InitTerrain())
		return false;
	if (g_Camera)
	{
		const float groundY = TerrainSystem::GetHeight(m_registry, 0.0f, 0.0f);
		g_Camera->SetPosition(XMVectorSet(0.0f, groundY + 2.0f, 0.0f, 0.0f));
	}

	constexpr float kPlayerScaleMultiplier = 0.1f;
	{
		ModelSpawnOptions player = {};
		player.position = { 0.0f, 0.0f, 0.0f };
		player.uniformScale = kPlayerScaleMultiplier;
		player.rotationY = 0.0f;
		player.foot = ModelSpawnOptions::FootPlacement::SnapFeetToTerrain;
		player.addPlayerComponent = true;
		player.addNprTag = true;
		if (!SpawnModelEntities(L"assets\\hibana\\hibana.pmx", player))
			return false;
	}

	if (!InitMainPipeline())
		return false;
	TreeVegetation::Initialize(m_registry, descriptorHeap, m_ownedVertexBuffers, m_ownedIndexBuffers);
	if (!InitSkyboxAndIBL())
		return false;

	s_treeImposterBakeOk = false;
	for (int i = 0; i < 3; ++i)
	{
		s_treeImposterMatGpu[i] = {};
		s_treeImposterMatTableStart[i] = nullptr;
	}
	s_treeImposterAtlas0.Reset();
	s_treeImposterAtlas1.Reset();
	s_treeImposterAtlas2.Reset();
	s_treeImposterQuadVb = nullptr;
	s_treeImposterQuadIb = nullptr;

	TryEnsureTreeImposterBake();

	if (!InitShadowSystem())
		return false;

	if (!InitAtmosphereSystem())
		return false;

	if (!InitPostProcess())
		return false;

	{
		UINT w = g_Engine->GetFrameBufferWidth();
		UINT h = g_Engine->GetFrameBufferHeight();
		s_hiz = new HiZSystem();
		if (!s_hiz->Init(g_Engine->Device(), w, h, g_Engine->GetDepthStencilResource()))
		{
			delete s_hiz;
			s_hiz = nullptr;
		}
		else if (descriptorHeap)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC hizSrvDesc = {};
			hizSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			hizSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			hizSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			hizSrvDesc.Texture2D.MostDetailedMip = 0;
			hizSrvDesc.Texture2D.MipLevels = s_hiz->GetMipCount();
			s_hizPyramidSrvHandle = descriptorHeap->RegisterResource(s_hiz->GetPyramidResource(), hizSrvDesc);
		}
	}

	// Tree GPU cull: LOD0 は非同期のため、メッシュ確定後に TryEnsureTreeGpuCullInit() で確保。
	TryEnsureTreeGpuCullInit();

	return true;
}

HiZSystem* Scene::GetHiZSystem() const
{
	return s_hiz;
}

TerrainGpuCullSystem* Scene::GetTerrainGpuCullSystem() const
{
	return s_terrainGpuCull;
}

TreeGpuCullSystem* Scene::GetTreeGpuCullSystem() const
{
	return s_treeGpuCull;
}

ShadowSystem* Scene::GetShadowSystem() const
{
	return s_shadow;
}

AtmosphereParams& Scene::GetAtmosphereParams()
{
	return s_atmosphereParams;
}

const AtmosphereParams& Scene::GetAtmosphereParams() const
{
	return s_atmosphereParams;
}

void Scene::GetDebugTreeDirectLodCounts(uint32_t& outLod0, uint32_t& outLod1, uint32_t& outLod2) const
{
	outLod0 = m_debugTreeDirectLodCount[0];
	outLod1 = m_debugTreeDirectLodCount[1];
	outLod2 = m_debugTreeDirectLodCount[2];
}

void Scene::SetTerrainPsDebugMode(int mode)
{
	if (mode < 0) mode = 0;
	if (mode > 3) mode = 3;
	m_terrainPsDebugMode = mode;
}

void Scene::Update()
{
	// Deferred entity destruction (requested from EditorUI etc. during previous frame)
	if (!m_pendingDestroy.empty())
	{
		// GPU may still reference VB/IB from in-flight frames; wait for all commands to finish.
		g_Engine->WaitForGpuIdle();

		auto removeOwned = [](auto& vec, auto* ptr) {
			auto it = std::find(vec.begin(), vec.end(), ptr);
			if (it != vec.end()) { delete *it; vec.erase(it); }
		};

		for (const entt::entity ent : m_pendingDestroy)
		{
			if (!m_registry.valid(ent))
				continue;

			std::wstring label;
			if (auto* lab = m_registry.try_get<EditorHierarchyLabelComponent>(ent))
				label = lab->displayName;

			auto* rootComp = m_registry.try_get<ModelGroupRootComponent>(ent);
			if (rootComp)
			{
				std::vector<entt::entity> children = rootComp->children;
				size_t freed = 0;
				for (const entt::entity child : children)
				{
					if (!m_registry.valid(child))
						continue;
					if (auto* mr = m_registry.try_get<MeshRendererComponent>(child); mr && mr->OwnsGpuBuffers)
					{
						removeOwned(m_ownedVertexBuffers, mr->pVB);
						removeOwned(m_ownedIndexBuffers, mr->pIB);
						++freed;
					}
					m_registry.destroy(child);
				}
				m_registry.destroy(ent);
				DebugLog("[Scene] Destroyed model group '%ls' (%zu meshes, %zu GPU buffers freed)\n",
					label.c_str(), children.size(), freed);
			}
			else
			{
				if (auto* mr = m_registry.try_get<MeshRendererComponent>(ent); mr && mr->OwnsGpuBuffers)
				{
					removeOwned(m_ownedVertexBuffers, mr->pVB);
					removeOwned(m_ownedIndexBuffers, mr->pIB);
				}
				m_registry.destroy(ent);
				DebugLog("[Scene] Destroyed entity '%ls'\n", label.c_str());
			}
		}
		m_pendingDestroy.clear();
	}

	ProcessAsyncModelLoads();
	TryEnsureTreeImposterBake();
	TryEnsureTreeGpuCullInit();

	float dt = 0.016f;
	// PlayerSystem が毎フレーム Y を地形に合わせる → その後に CameraSystem が追従するのが一貫。
	// CameraSystem を先にすると TPS 追従が1フレームずれ、地形の上で「引き戻される／遅延」に見えやすい。
	PlayerSystem::Update(m_registry);
	CameraSystem::Update(g_Camera, dt, m_registry);

	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto* sc = sceneConstantBuffer[currentIndex] ? sceneConstantBuffer[currentIndex]->GetPtr<SceneConstants>() : nullptr;
	if (!sc)
		return;

	float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
	XMMATRIX viewMat = g_Camera->GetViewMatrix();
	XMMATRIX projMat = g_Camera->GetProjectionMatrix(aspect);
	sc->View = XMMatrixTranspose(viewMat);
	sc->Proj = XMMatrixTranspose(projMat);
	XMStoreFloat4(&sc->CameraWorld, g_Camera->GetPosition());
	sc->CameraWorld.w = 1.f;

	XMVECTOR sunDir = XMVector3Normalize(XMVectorSet(0.5f, 0.7f, -1.0f, 0.0f));
	XMStoreFloat4(&sc->SunDirection, sunDir);
	sc->SunDirection.w = 1.0f;
	sc->SunColor = XMFLOAT4(1.2f, 1.2f, 1.2f, 1.0f);
	sc->InvViewProj = XMMatrixTranspose(XMMatrixInverse(nullptr, viewMat * projMat));

	if (s_shadow && s_shadow->IsValid())
	{
		XMFLOAT3 sunDirF3;
		XMStoreFloat3(&sunDirF3, sunDir);
		s_shadow->UpdateCascades(viewMat, projMat, sunDirF3, 0.1f, 500.0f);
	}

	// Terrain DrawMain reads SceneConstants; keep slot0 in sync for any legacy readers.
	auto currentTransform = constantBuffer[currentIndex]->GetPtr<Transform>();
	if (currentTransform)
	{
		currentTransform->View = sc->View;
		currentTransform->Proj = sc->Proj;
	}

	WriteNprTuningToPbrConstants(pbrPropertyBuffer[currentIndex]->GetPtr<PBRConstants>());
	if (terrainConstantBuffer[currentIndex])
	{
		auto* terrConst = terrainConstantBuffer[currentIndex]->GetPtr<TerrainConstants>();
		if (terrConst)
		{
			XMVECTOR camPos = g_Camera->GetPosition();
			XMStoreFloat4(&terrConst->CameraPos, camPos);
			terrConst->DebugParams = XMFLOAT4(
				static_cast<float>(m_terrainPsDebugMode),
				m_terrainCheapPathEnabled ? 1.0f : 0.0f,
				m_terrainCheapGrazingThresh,
				m_terrainCheapNearPreserveMeters);
		}
	}

	XMFLOAT3 cameraPos;
	XMStoreFloat3(&cameraPos, g_Camera->GetPosition());
	TransformSystem::Update(m_registry);
	LODSystem::Update(m_registry, cameraPos);
	TreeLodSystem::Update(m_registry);
}

void Scene::SyncNprGpuTuningToMaterialCB()
{
	if (!g_Engine)
		return;
	const UINT currentIndex = g_Engine->CurrentBackBufferIndex();
	if (currentIndex >= Engine::FRAME_BUFFER_COUNT || !pbrPropertyBuffer[currentIndex])
		return;
	WriteNprTuningToPbrConstants(pbrPropertyBuffer[currentIndex]->GetPtr<PBRConstants>());
}

PostProcessSettings& Scene::GetPostProcessSettings()
{
	return s_postProcessSettings;
}

const PostProcessSettings& Scene::GetPostProcessSettings() const
{
	return s_postProcessSettings;
}

void Scene::GetNprPathDiagnostics(size_t& outNprTagEntityCount, bool& outNprOpaquePsoValid, bool& outWillUseNprDrawPass) const
{
	outNprTagEntityCount = 0;
	for ([[maybe_unused]] entt::entity e : m_registry.view<NPRTag>())
		++outNprTagEntityCount;
	outNprOpaquePsoValid = (nprPipelineState != nullptr && nprPipelineState->IsValid());
	const bool nprTransOk = (nprTransparentPipelineState != nullptr && nprTransparentPipelineState->IsValid());
	outWillUseNprDrawPass = (outNprTagEntityCount > 0) && (outNprOpaquePsoValid || nprTransOk);
}

void Scene::Draw()
{
	auto commandList = g_Engine->MainGraphicsCmdList();
	auto postCommandList = g_Engine->PostGraphicsCmdList();
	// Update() 後に非同期で LOD0 が載ったフレームでも、Draw 冒頭で一度確保してマスク経路に乗せる。
	TryEnsureTreeGpuCullInit();

	bool hasNprTag = false;
	for ([[maybe_unused]] entt::entity e : m_registry.view<NPRTag>())
	{
		(void)e;
		hasNprTag = true;
		break;
	}
	const bool nprOpaquePsoOk = nprPipelineState && nprPipelineState->IsValid();
	const bool nprTransPsoOk = nprTransparentPipelineState && nprTransparentPipelineState->IsValid();
	const bool useNprDrawPath = hasNprTag && (nprOpaquePsoOk || nprTransPsoOk);

	EngineDoTransition(commandList, g_Engine->GetHdrColorResource(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (useNprDrawPath)
	{
		EngineDoTransition(commandList, g_Engine->GetNprHdrColorResource(),
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_RENDER_TARGET);
	}

	// ---- Shadow Pass: render depth from light viewpoint ----
	if (s_shadow && s_shadow->IsValid())
	{
		GPU_CMD_BEGIN_EVENT(commandList, 60, 60, 60, L"Shadow Pass (CSM)");
		s_shadow->ResetPerDrawRing();

		for (UINT cascade = 0; cascade < ShadowSystem::kCascadeCount; ++cascade)
		{
			s_shadow->BeginShadowPass(commandList, cascade);
			commandList->SetPipelineState(s_shadow->GetShadowPSO());
			commandList->SetGraphicsRootSignature(s_shadow->GetShadowRootSignature());
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			XMMATRIX lightVP = s_shadow->GetLightVPTransposed(cascade);
			lightVP = XMMatrixTranspose(lightVP);

			auto view = m_registry.view<TransformComponent, MeshRendererComponent>();
			for (auto entity : view)
			{
				auto& mr = view.get<MeshRendererComponent>(entity);
				if (!mr.pVB || !mr.pIB || mr.IndexCount == 0) continue;
				if (m_registry.any_of<TerrainMeshTag>(entity)) continue;

				auto& tc = view.get<TransformComponent>(entity);
				XMMATRIX world = tc.WorldMatrix;
				XMMATRIX wlvp = world * lightVP;

				D3D12_GPU_VIRTUAL_ADDRESS cbAddr = s_shadow->WritePerDrawCB(wlvp);
				if (cbAddr == 0) break;

				commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
				auto vbView = mr.pVB->View();
				auto ibView = mr.pIB->View();
				commandList->IASetVertexBuffers(0, 1, &vbView);
				commandList->IASetIndexBuffer(&ibView);
				commandList->DrawIndexedInstanced(mr.IndexCount, 1, mr.StartIndexLocation, 0, 0);
			}

			// Tree shadows: limited cascade count + capped instance count
			if (s_treeGpuCull && s_treeGpuCull->IsValid() && s_shadow->HasTreeShadowPipeline()
				&& s_treeGpuCull->GetInstanceCount() > 0
				&& s_atmosphereParams.enableTreeShadows
				&& cascade < static_cast<UINT>(s_atmosphereParams.treeShadowCascades))
			{
				VertexBuffer* treeVb = TreeVegetation::GetMergedVertexBufferLod(0);
				IndexBuffer* treeIb = TreeVegetation::GetMergedIndexBufferLod(0);
				uint32_t treeIdxCount = TreeVegetation::GetMergedIndexCountLod(0);
				UINT drawCount = std::min(s_treeGpuCull->GetInstanceCount(),
					static_cast<uint32_t>(s_atmosphereParams.treeShadowMaxInstances));
				s_shadow->DrawTreeShadows(
					commandList, cascade,
					s_treeGpuCull->GetInstanceDataResource(),
					drawCount,
					treeVb, treeIb, treeIdxCount);
			}

			s_shadow->EndShadowPass(commandList, cascade);
		}

		s_shadow->TransitionToSRV(commandList);
		GPU_CMD_END_EVENT(commandList);

		// Restore main viewport/scissor after shadow pass
		commandList->RSSetViewports(1, &g_Engine->GetViewport());
		commandList->RSSetScissorRects(1, &g_Engine->GetScissorRect());
	}

	// ---- Main forward pass ----
	D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvHandle = g_Engine->GetHdrRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE nprHdrRtvHandle = g_Engine->GetNprHdrRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_Engine->GetDsvCpuHandle();
	commandList->OMSetRenderTargets(1, &hdrRtvHandle, FALSE, &dsvHandle);

	const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	commandList->ClearRenderTargetView(hdrRtvHandle, clearColor, 0, nullptr);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
	GPU_CMD_BEGIN_EVENT(commandList, 40, 40, 40, L"After HDR+DSV clear (inspect here = this frame, not previous)");
	GPU_CMD_END_EVENT(commandList);

	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto materialHeap = descriptorHeap->GetHeap();
	bool treeIndirectPrepared = false;
	D3D12_GPU_DESCRIPTOR_HANDLE treeIndirectMats[3][3][3]{};
	VertexBuffer* treeIndirectVb[3][3]{};
	IndexBuffer* treeIndirectIb[3][3]{};
	uint32_t treeIndirectIdx[3][3]{};
	commandList->SetDescriptorHeaps(1, &materialHeap);
	commandList->SetGraphicsRootSignature(rootSignature->Get());
	commandList->SetPipelineState(pipelineState->Get());

	// Bind shadow resources on PBR root signature slots 7, 8 (space2)
	if (s_shadow && s_shadow->IsValid())
	{
		commandList->SetGraphicsRootDescriptorTable(7, s_shadow->GetShadowMapSrvGpu());
		commandList->SetGraphicsRootConstantBufferView(8, s_shadow->GetShadowCBAddress());
	}

	D3D12_GPU_DESCRIPTOR_HANDLE envHandle = s_envCubemapHandle ? s_envCubemapHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };
	D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskGPU = s_terrainMaskHandle ? s_terrainMaskHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };
	TerrainGpuCullSystem* terrainCullForDraw = kDisableTerrainGpuCullForDebug ? nullptr : s_terrainGpuCull;
	if (terrainCullForDraw)
	{
		const UINT w = g_Engine->GetFrameBufferWidth();
		const UINT h = g_Engine->GetFrameBufferHeight();
		const UINT mips = s_hiz ? s_hiz->GetMipCount() : 1u;
		const bool hizReadyForTerrainCull = (s_hiz && s_hiz->IsValid() && s_hiz->GetEnabled());
		terrainCullForDraw->SetHiZResources(s_hizPyramidSrvHandle, w, h, mips, hizReadyForTerrainCull);
	}

	bool hasStreamedTreesThisFrame = false;
	const TreeGpuCullSystem::TreeInstanceCpu* treeCpuDataForThisFrame = nullptr;
	uint32_t treeCpuCountForThisFrame = 0;
	DirectX::XMFLOAT3 treeCamPosForThisFrame{};
	// マスク上の全インスタンス（CPU）。実体は GetAllMaskInstancesCached。毎フレーム vector を再確保しない。
	{
		static std::vector<TreeVegetation::StreamedTreeInstance> s_maskBuildTmp;
		TreeVegetation::BuildAllMaskInstances(m_registry, s_maskBuildTmp, 8.0f);
	}
	const auto& treeInstCached = TreeVegetation::GetAllMaskInstancesCached();
	// Tree GPU cull uses previous frame Hi-Z (same as terrain)
	if (!kDisableTreeGpuCullWork && s_treeGpuCull && s_treeGpuCull->IsValid())
	{
		const UINT w = g_Engine->GetFrameBufferWidth();
		const UINT h = g_Engine->GetFrameBufferHeight();
		const UINT mips = s_hiz ? s_hiz->GetMipCount() : 1u;
		// Tree Hi-Z: use previous frame Hi-Z when available.
		// (CS has a near-disable distance so near trees won't be occlusion-tested)
		const bool hizReady = (s_hiz && s_hiz->IsValid() && s_hiz->GetEnabled());
		s_treeGpuCull->SetHiZResources(s_hizPyramidSrvHandle, w, h, mips, hizReady && kTreeCullUseHiZ);

		// Full-nature mode: build ALL instances from the mask (no thinning).
		DirectX::XMFLOAT3 camPos{};
		DirectX::XMStoreFloat3(&camPos, g_Camera->GetPosition());
		treeCamPosForThisFrame = camPos;
		if (treeInstCached.empty())
		{
			// Fallback: use existing spawned TreeInstance entities so we always have something to render/debug.
			// (unchanged)
		}
		if (!treeInstCached.empty())
		{
			hasStreamedTreesThisFrame = true;
			// StreamedTreeInstance は TreeInstanceCpu と同一レイアウト。二重 vector を持たずマスクキャッシュを直接渡す。
			treeCpuDataForThisFrame = reinterpret_cast<const TreeGpuCullSystem::TreeInstanceCpu*>(treeInstCached.data());
			treeCpuCountForThisFrame = static_cast<uint32_t>(treeInstCached.size());
			// Ensure TreeGpuCull has enough capacity for the full mask.
			// If not, recreate it once with a larger maxInstances.
			if (s_treeGpuCull && s_treeGpuCull->IsValid() && s_treeGpuCull->GetMaxInstances() < static_cast<uint32_t>(treeInstCached.size()))
			{
				const uint32_t want = static_cast<uint32_t>(std::min(treeInstCached.size(), static_cast<size_t>(kTreeGpuMaxMaskInstances)));
				DebugLog("[Trees][GPUCull] reinit for maxInst=%u (was %u)\n", want, s_treeGpuCull->GetMaxInstances());
				TreeGpuCullSystem* old = s_treeGpuCull;
				s_treeGpuCull = new TreeGpuCullSystem();
				if (!s_treeGpuCull->Init(g_Engine->Device(), descriptorHeap, rootSignature ? rootSignature->Get() : nullptr, want))
				{
					delete s_treeGpuCull;
					s_treeGpuCull = old; // keep old (will clamp instance count)
				}
				else
				{
					// old resources may still be referenced by in-flight GPU work from previous frames.
					// wait once before releasing to avoid OBJECT_DELETED_WHILE_STILL_IN_USE.
					if (g_Engine)
						g_Engine->WaitForGpuIdle();
					delete old;
					s_treeGpuMaskUploaded = false;
				}
			}

			const uint32_t idxMerged0 = TreeVegetation::GetMergedIndexCountLod(0);
			const uint32_t idxMerged1 = TreeVegetation::GetMergedIndexCountLod(1);
			const uint32_t idxMerged2 = TreeVegetation::GetMergedIndexCountLod(2);
			const uint32_t idxTrunk0 = TreeVegetation::GetPartIndexCount(0);
			const uint32_t idxLeaves0 = TreeVegetation::GetPartIndexCount(1);
			const uint32_t idxBranch0 = TreeVegetation::GetPartIndexCount(2);
			DebugLog("[Trees][Mesh] merged lod0=%u lod1=%u lod2=%u | part trunk=%u leaves=%u branches=%u\n",
				idxMerged0, idxMerged1, idxMerged2, idxTrunk0, idxLeaves0, idxBranch0);
			// index 表は DrawMain 後の FillTreeIndexCountByPartByLod(treeIndirectIdx) のみとする。
			// メインで先に Fill して Dispatch に渡すと、インポスター bake 前後で [0][1] 等がズレ、CS の IndexCount=0・Draw 側 idx>0 になり RenderDoc で <0, N> になる。
		}
	}
	// CPU parallelization: enable worker command-list recording for PBR batches.
	// This reduces main-thread recording load when many draws are present.
	const bool disableParallelPbr = false;
	// 森の木は TreeGpuCullSystem（ポストCL: Upload+Dispatch+ExecuteIndirect 同一リスト）RenderSystem 内 kSkipEcsTreeMeshesInMainPbrPass。
	Profiler::GpuMarkDrawMainBegin(commandList);
	GPU_CMD_BEGIN_EVENT(commandList, 80, 180, 255, L"Scene: DrawMain (terrain + ECS PBR; trees = post CL GPU path)");
	RenderSystem::DrawMain(m_registry, commandList,
		constantBuffer[currentIndex],
		sceneConstantBuffer[currentIndex],
		pbrPropertyBuffer[currentIndex],
		s_pbrInstanceRingBuffer.Get(),
		s_pbrInstanceRingMapped,
		currentIndex,
		rootSignature, pipelineState, descriptorHeap, envHandle,
		terrainRootSignature, terrainDepthPrepassPipelineState, terrainPipelineState, terrainConstantBuffer[currentIndex], terrainMaskGPU,
		disableParallelPbr ? nullptr : g_Engine->PbrRecordCmdList(0),
		disableParallelPbr ? nullptr : g_Engine->PbrRecordCmdList(1),
		materialHeap,
		hdrRtvHandle,
		dsvHandle,
		nprOpaquePsoOk,
		nprTransPsoOk,
		terrainCullForDraw,
		m_terrainSharedVB,
		m_terrainSharedIB,
		treeLod1PipelineState,
		treeLod2PipelineState);
	GPU_CMD_END_EVENT(commandList);
	Profiler::GpuMarkDrawMainEnd(commandList);

	// Tree ExecuteIndirect draw (species×LOD×part) — マテ・VB 束ね（Dispatch はポストCLで Draw と連結）
	// NOTE: streamed instances が 0 のフレームで ExecuteIndirect を呼ぶと、未初期化の counter/args を参照して暴走し得る
	if (!kDisableTreeGpuCullWork && s_treeGpuCull && s_treeGpuCull->IsValid()
		&& hasStreamedTreesThisFrame)
	{
		// 案A: ExecuteIndirect（GPU駆動）をメイン経路にする
		const bool kEnableTreeExecuteIndirect = true;
		if (!kEnableTreeExecuteIndirect)
		{
			// Stable fallback path: direct instancing (no ExecuteIndirect)
			// CPU LOD split to match ExecuteIndirect LOD distances:
			// - LOD0: <= 10m
			// - LOD1: <= 50m
			// - LOD2:  > 50m
			std::vector<TreeGpuCullSystem::TreeInstanceCpu> lodCpu[3];
			const size_t cpuCount = treeCpuDataForThisFrame ? static_cast<size_t>(treeCpuCountForThisFrame) : 0u;
			lodCpu[0].reserve(cpuCount);
			lodCpu[1].reserve(cpuCount);
			lodCpu[2].reserve(cpuCount);
			// Hysteresis to prevent LOD flicker near thresholds (esp. 2m).
			// Keyed by quantized XZ + species so classification is stable frame-to-frame.
			static std::unordered_map<uint64_t, uint8_t> s_treeLodHistory;
			s_treeLodHistory.reserve(4096);
			const float lod0In = 2.0f;
			const float lod0Out = 2.4f;   // leave LOD0 only when clearly farther
			const float lod1In = 50.0f;
			const float lod1Out = 55.0f;  // leave LOD1 only when clearly farther
			const float lod0In2 = lod0In * lod0In;
			const float lod0Out2 = lod0Out * lod0Out;
			const float lod1In2 = lod1In * lod1In;
			const float lod1Out2 = lod1Out * lod1Out;

			if (treeCpuDataForThisFrame && treeCpuCountForThisFrame > 0)
			for (uint32_t ci = 0; ci < treeCpuCountForThisFrame; ++ci)
			{
				const TreeGpuCullSystem::TreeInstanceCpu& inst = treeCpuDataForThisFrame[ci];
				DirectX::XMFLOAT4X4 w{};
				DirectX::XMStoreFloat4x4(&w, inst.worldGpuT);

				// NOTE: TreeInstanceCpu::worldGpuT is "GPU transposed" (see struct comment).
				// Therefore translation lives in (m14,m24,m34) after transpose.
				const float tx = w._14;
				const float tz = w._34;
				// LOD distance should be ground distance (XZ) not 3D distance (Y),
				// because the camera often sits far above the ground.
				const float dx = tx - treeCamPosForThisFrame.x;
				const float dz = tz - treeCamPosForThisFrame.z;
				const float d2 = dx * dx + dz * dz;

				// Quantize XZ position to build a stable key (1m grid).
				const int qx = static_cast<int>(floorf(tx));
				const int qz = static_cast<int>(floorf(tz));
				uint64_t key = 0;
				key |= (static_cast<uint64_t>(static_cast<uint32_t>(qx)) & 0xFFFFFFFFull) << 32;
				key |= (static_cast<uint64_t>(static_cast<uint32_t>(qz)) & 0xFFFFFFFFull);
				key ^= static_cast<uint64_t>(inst.speciesIndex) * 0x9E3779B185EBCA87ull;

				uint8_t prev = 2;
				if (auto it = s_treeLodHistory.find(key); it != s_treeLodHistory.end())
					prev = it->second;

				uint8_t lod = prev;
				// Transition rules with hysteresis bands
				if (prev == 0)
				{
					if (d2 > lod0Out2)
						lod = (d2 <= lod1In2) ? 1 : 2;
				}
				else if (prev == 1)
				{
					if (d2 <= lod0In2) lod = 0;
					else if (d2 > lod1Out2) lod = 2;
				}
				else
				{
					if (d2 <= lod0In2) lod = 0;
					else if (d2 <= lod1In2) lod = 1;
					else lod = 2;
				}

				s_treeLodHistory[key] = lod;
				lodCpu[lod].push_back(inst);
			}

			// Debug UI: record actual direct-instancing LOD split counts.
			m_debugTreeDirectLodCount[0] = static_cast<uint32_t>(lodCpu[0].size());
			m_debugTreeDirectLodCount[1] = static_cast<uint32_t>(lodCpu[1].size());
			m_debugTreeDirectLodCount[2] = static_cast<uint32_t>(lodCpu[2].size());

			for (int lod = 0; lod < 3; ++lod)
			{
				if (lodCpu[lod].empty())
					continue;

				s_treeGpuCull->UpdateInstances(commandList, lodCpu[lod].data(), static_cast<uint32_t>(lodCpu[lod].size()));

				for (int part = 0; part < 3; ++part)
				{
					// Direct-instancing "visual LOD":
					// - Only draw leaves in LOD0 (near). LOD1/2 skip leaves to reduce alpha-cut cost and avoid "always LOD0" look.
					// - For LOD2, also skip branches (keep trunk only).
					if (part == 1 && lod > 0)
						continue;
					if (part == 2 && lod > 1)
						continue;

					VertexBuffer* tvb = TreeVegetation::GetPartVertexBuffer(part);
					IndexBuffer* tib = TreeVegetation::GetPartIndexBuffer(part);
					const uint32_t tic = TreeVegetation::GetPartIndexCount(part);
					if (!tvb || !tib || tic == 0)
						continue;

					PipelineState* drawPso = pipelineState;
					if (part == 1 && treeLod1PipelineState && treeLod1PipelineState->IsValid())
						drawPso = treeLod1PipelineState;

					D3D12_GPU_DESCRIPTOR_HANDLE tmat = {};
					if (part == 1)
					{
						if (const TreeSpeciesMaterials* sm0 = TreeVegetation::GetSpeciesMaterials(0); sm0 && sm0->matLod1)
							tmat = sm0->matLod1->HandleGPU;
					}
					if (tmat.ptr == 0)
					{
						if (DescriptorHandle* pm = TreeVegetation::GetPartMaterialHandle(part))
							tmat = pm->HandleGPU;
					}

					s_treeGpuCull->DrawDirectInstancedDebug(
						commandList, rootSignature, drawPso,
						sceneConstantBuffer[currentIndex]->GetAddress(),
						pbrPropertyBuffer[currentIndex]->GetAddress(),
						tmat, envHandle, tvb, tib, tic);
				}
			}
		}
		if (kEnableTreeExecuteIndirect)
		{
		for (int si = 0; si < 3; ++si)
		{
			const TreeSpeciesMaterials* sm = TreeVegetation::GetSpeciesMaterials(static_cast<size_t>(si));
			for (int part = 0; part < 3; ++part)
			{
				// trunk/branches: species の LOD0 マテリアルを流用（後で species別の幹/枝マテに拡張）
				D3D12_GPU_DESCRIPTOR_HANDLE m0 = (sm && sm->matLod0) ? sm->matLod0->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{};
				D3D12_GPU_DESCRIPTOR_HANDLE m1 = (sm && sm->matLod1) ? sm->matLod1->HandleGPU : m0;
				D3D12_GPU_DESCRIPTOR_HANDLE m2 = (sm && sm->matLod2) ? sm->matLod2->HandleGPU : m0;

				// Safety fallback: species material missing -> use part-generic material.
				if (m0.ptr == 0)
				{
					if (DescriptorHandle* pm = TreeVegetation::GetPartMaterialHandle(part))
						m0 = pm->HandleGPU;
				}
				if (m1.ptr == 0)
					m1 = m0;
				if (m2.ptr == 0)
					m2 = m1.ptr ? m1 : m0;

				if (part == 1)
				{
					// leaves: alpha-cut 用に matLod1（t3=alpha）を優先
					treeIndirectMats[si][part][0] = m1;
					treeIndirectMats[si][part][1] = m1;
					treeIndirectMats[si][part][2] = m2;
				}
				else
				{
					treeIndirectMats[si][part][0] = m0;
					treeIndirectMats[si][part][1] = m0;
					treeIndirectMats[si][part][2] = m0;
				}
			}
		}
		// species0(tree1): per-part materials from FBX submeshes
		for (int lod = 0; lod < 3; ++lod)
		{
			for (int part = 0; part < 3; ++part)
			{
				if (DescriptorHandle* pm = TreeVegetation::GetPartMaterialHandle(part))
					treeIndirectMats[0][part][lod] = pm->HandleGPU;
			}
		}

		FillTreeIndexCountByPartByLod(treeIndirectIdx);
		for (int part = 0; part < 3; ++part)
		{
			VertexBuffer* pvb = TreeVegetation::GetPartVertexBuffer(part);
			IndexBuffer* pib = TreeVegetation::GetPartIndexBuffer(part);
			for (int lod = 0; lod < 3; ++lod)
			{
				treeIndirectVb[part][lod] = pvb ? pvb : TreeVegetation::GetMergedVertexBufferLod(lod);
				treeIndirectIb[part][lod] = pib ? pib : TreeVegetation::GetMergedIndexBufferLod(lod);
			}
		}
		// vb[0][1] は常に LOD1 メッシュ。クワッドは DrawIndirectLods が vbImposterQuad を渡すときだけ使う。

			treeIndirectPrepared = true;

			// NOTE: Do not auto-fallback to direct instancing here.
			// The debug visible counter is based on previous-frame readback and can be 0 on warm-up,
			// and drawing even a sample can still distort perf measurements. Use explicit debug paths instead.
		}
	}
	else if (!kDisableTreeGpuCullWork && s_treeGpuCull && s_treeGpuCull->IsValid())
	{
		Profiler::GpuMarkTreeDrawBegin(commandList);
		Profiler::GpuMarkTreeDrawEnd(commandList);
	}

	if (useNprDrawPath)
	{
		const float clearNpr[] = { 0.0f, 0.0f, 0.0f, 0.0f };
		commandList->OMSetRenderTargets(1, &nprHdrRtvHandle, FALSE, &dsvHandle);
		commandList->ClearRenderTargetView(nprHdrRtvHandle, clearNpr, 0, nullptr);

		GPU_CMD_BEGIN_EVENT(commandList, 255, 120, 200, L"Scene: NPR (opaque + transparent)");
		XMFLOAT3 camPos{};
		XMStoreFloat3(&camPos, g_Camera->GetPosition());
		RenderSystem::DrawNprPasses(m_registry, commandList,
			sceneConstantBuffer[currentIndex],
			pbrPropertyBuffer[currentIndex],
			s_pbrInstanceRingBuffer.Get(),
			s_pbrInstanceRingMapped,
			currentIndex,
			rootSignature,
			nprPipelineState,
			nprTransparentPipelineState,
			descriptorHeap,
			envHandle,
			materialHeap,
			nprHdrRtvHandle,
			dsvHandle,
			camPos);
		GPU_CMD_END_EVENT(commandList);
	}

	// 木: Post CL で UpdateInstances → DispatchCull → ExecuteIndirect を同一リストに記録（メインCLと分離しない）
	if (treeIndirectPrepared && s_treeGpuCull && s_treeGpuCull->IsValid())
	{
		postCommandList->SetDescriptorHeaps(1, &materialHeap);
		Profiler::GpuMarkTreeUploadCullBegin(postCommandList);
		bool didTreeDispatchCullThisFrame = false;
		const auto& treeInstForGpu = TreeVegetation::GetAllMaskInstancesCached();
		if (hasStreamedTreesThisFrame && !treeInstForGpu.empty())
		{
			const uint64_t maskSerial = TreeVegetation::GetMaskInstancesBuildSerial();
			if (!s_treeGpuMaskUploaded || s_treeGpuMaskLastUploadedCount != treeInstForGpu.size()
				|| s_treeGpuMaskLastSerial != maskSerial)
			{
				s_treeGpuCull->UpdateInstances(
					postCommandList,
					reinterpret_cast<const TreeGpuCullSystem::TreeInstanceCpu*>(treeInstForGpu.data()),
					static_cast<uint32_t>(treeInstForGpu.size()));
				s_treeGpuMaskUploaded = true;
				s_treeGpuMaskLastUploadedCount = treeInstForGpu.size();
				s_treeGpuMaskLastSerial = maskSerial;
			}
			if (const SceneConstants* scGpu = sceneConstantBuffer[currentIndex] ? sceneConstantBuffer[currentIndex]->GetPtr<SceneConstants>() : nullptr)
			{
				// TreeFrustumHiZCull_CS の IndexCounts と DrawIndirectLods の idx は treeIndirectIdx と同一必須（上で Fill 済み）。
				if (s_treeGpuCull->DispatchCull(
					postCommandList,
					scGpu,
					treeIndirectIdx,
					treeCamPosForThisFrame))
					didTreeDispatchCullThisFrame = true;
			}
		}
		Profiler::GpuMarkTreeUploadCullEnd(postCommandList);
		if (didTreeDispatchCullThisFrame)
		{
			RenderSystem::DrawPostScenePbrTreesExecuteIndirect(
				postCommandList,
				materialHeap,
				hdrRtvHandle,
				dsvHandle,
				s_treeGpuCull,
				rootSignature,
				treeOpaquePipelineState ? treeOpaquePipelineState : pipelineState,
				treeLod1PipelineState,
				(treeImposterPipelineState && treeImposterPipelineState->IsValid()) ? treeImposterPipelineState : nullptr,
				nullptr,
				sceneConstantBuffer[currentIndex]->GetAddress(),
				pbrPropertyBuffer[currentIndex]->GetAddress(),
				treeIndirectMats,
				s_treeImposterMatGpu,
				envHandle,
				treeIndirectVb,
				treeIndirectIb,
				treeIndirectIdx,
				s_treeImposterQuadVb,
				s_treeImposterQuadIb);
		}
	}

	if (s_skyboxRenderer && s_skyboxRenderer->IsValid())
	{
		// Post CL でも RTV/DSV は継承されない（スカイボックス PSO は D32 DSV 前提）
		postCommandList->SetDescriptorHeaps(1, &materialHeap);
		postCommandList->OMSetRenderTargets(1, &hdrRtvHandle, FALSE, &dsvHandle);
		float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
		s_skyboxRenderer->Draw(postCommandList, g_Camera->GetViewMatrix(), g_Camera->GetProjectionMatrix(aspect));
	}

	if (s_hiz && s_hiz->IsValid())
	{
		Profiler::GpuMarkHiZBuildBegin(postCommandList);
		s_hiz->Build(postCommandList, g_Engine->GetDepthStencilResource());
		Profiler::GpuMarkHiZBuildEnd(postCommandList);
	}

	// ---- Atmosphere: fog + volumetric light (before bloom/tonemap) ----
	if (s_atmosphere && s_atmosphere->IsValid() &&
		(s_atmosphereParams.enableFog || s_atmosphereParams.enableVolumetric))
	{
		ID3D12Resource* hdrRes = g_Engine->GetHdrColorResource();
		ID3D12Resource* depthRes = g_Engine->GetDepthStencilResource();

		XMMATRIX invVP = XMMatrixInverse(nullptr, g_Camera->GetViewMatrix()
			* g_Camera->GetProjectionMatrix(static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT)));
		XMFLOAT3 camPos;
		XMStoreFloat3(&camPos, g_Camera->GetPosition());
		XMFLOAT3 sunDirF;
		{
			auto* sc = sceneConstantBuffer[currentIndex] ? sceneConstantBuffer[currentIndex]->GetPtr<SceneConstants>() : nullptr;
			sunDirF = sc ? XMFLOAT3(sc->SunDirection.x, sc->SunDirection.y, sc->SunDirection.z) : XMFLOAT3(0.5f, 0.7f, -1.0f);
		}

		s_atmosphere->Execute(postCommandList, materialHeap,
			s_hdrSrvHandle ? s_hdrSrvHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{0},
			g_Engine->GetHdrRtvCpuHandle(),
			hdrRes, depthRes,
			s_shadow,
			invVP, camPos, sunDirF,
			XMFLOAT4(1.2f, 1.2f, 1.2f, 1.0f),
			g_Engine->CurrentBackBufferIndex(),
			s_atmosphereParams);
	}

	Profiler::GpuMarkPostProcessBegin(postCommandList);
	if (s_postProcess && s_postProcess->IsValid() && s_hdrSrvHandle)
	{
		ID3D12Resource* hdrRes = g_Engine->GetHdrColorResource();
		ID3D12Resource* nprHdrRes = g_Engine->GetNprHdrColorResource();
		ID3D12Resource* backBufferRes = g_Engine->GetBackBufferResource();
		const bool splitNprPost = useNprDrawPath && s_postProcess->HasRegisteredNprHdrSrv();
		D3D12_RESOURCE_BARRIER barriers[3] = {};
		UINT bi = 0;
		barriers[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(hdrRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (splitNprPost && nprHdrRes)
			barriers[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(nprHdrRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (backBufferRes)
			barriers[bi++] = CD3DX12_RESOURCE_BARRIER::Transition(backBufferRes, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		GPU_CMD_BEGIN_EVENT(postCommandList, 180, 200, 100, L"Scene: PostProcess prep (HDR/NPR RT→SRV, backbuffer RTV)");
		postCommandList->ResourceBarrier(bi, barriers);
		GPU_CMD_END_EVENT(postCommandList);

		postCommandList->SetDescriptorHeaps(1, &materialHeap);
		GPU_CMD_BEGIN_EVENT(postCommandList, 200, 255, 120, L"Scene: PostProcess (bloom + tonemap + composite)");
		s_postProcess->Execute(postCommandList, materialHeap, s_hdrSrvHandle->HandleGPU, g_Engine->GetBackBufferRtvCpuHandle(), s_postProcessSettings,
			splitNprPost);
		GPU_CMD_END_EVENT(postCommandList);
	}
	// 木 GPU（TreeGpuCull）未初期化のフレームでは 10..13 に EndQuery が無く、ResolveQueryData が失敗する。
	Profiler::EnsureTreeGpuStampPlaceholders(postCommandList);
	Profiler::GpuMarkPostProcessEndAndResolve(postCommandList);
}
