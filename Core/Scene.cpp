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
#include "ModelBounds.h"
#include "Engine/Core/AsyncModelLoader.h"

#include <filesystem>
#include <algorithm>
#include <vector>
#include <cstdio>

using namespace DirectX;
namespace fs = std::filesystem;

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
RootSignature* terrainRootSignature = nullptr;
PipelineState* terrainPipelineState = nullptr;
ConstantBuffer* terrainConstantBuffer[Engine::FRAME_BUFFER_COUNT] = {};
Camera* g_Camera = nullptr;

namespace {
	struct TreeSpeciesConfig
	{
		const wchar_t* ModelPath;
	};

	const TreeSpeciesConfig kTreeSpecies[3] = {
		{ L"assets\\sakura1\\sakura1.fbx" },
		{ L"assets\\sakura1\\sakura1.fbx" },
		{ L"assets\\sakura1\\sakura1.fbx" },
	};

	constexpr float kTerrainCellSpacing = 8.0f;
	constexpr float kTerrainMaxHeight = 250.0f;

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
		heap->Register(normalTex);

		Texture2D* metallicTex = Texture2D::Get(mesh.MetallicMap);
		if (!metallicTex && !mesh.MetallicMap.empty())
			metallicTex = Texture2D::Get(ReplaceExtension(mesh.MetallicMap, "tga"));
		if (!metallicTex) metallicTex = Texture2D::GetDefaultMetallic();
		heap->Register(metallicTex);

		Texture2D* roughnessTex = Texture2D::Get(mesh.RoughnessMap);
		if (!roughnessTex && !mesh.RoughnessMap.empty())
			roughnessTex = Texture2D::Get(ReplaceExtension(mesh.RoughnessMap, "tga"));
		if (!roughnessTex) roughnessTex = Texture2D::GetDefaultRoughness();
		heap->Register(roughnessTex);

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
		delete mr.pVB;
		mr.pVB = nullptr;
		delete mr.pIB;
		mr.pIB = nullptr;
	}
	m_ownedVertexBuffers.clear();
	m_ownedIndexBuffers.clear();
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

	s_hdrSrvHandle = nullptr;
	s_envCubemapHandle = nullptr;
	s_terrainMaskHandle = nullptr;

	skyboxCubemap.Reset();
	skyboxEquirect.Reset();
	s_irradianceCubemap.Reset();
	s_prefilterCubemap.Reset();
	s_brdfLut.Reset();

	delete rootSignature;
	rootSignature = nullptr;
	delete pipelineState;
	pipelineState = nullptr;
	delete terrainRootSignature;
	terrainRootSignature = nullptr;
	delete terrainPipelineState;
	terrainPipelineState = nullptr;

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
		m_registry.emplace<EditorHierarchyLabelComponent>(parentEnt,
			EditorHierarchyLabelComponent{ fs::path(path).filename().wstring() });
		m_registry.emplace<LODComponent>(parentEnt, 0, 0.0f);
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

void Scene::ProcessAsyncModelLoads()
{
	if (!g_AsyncModelLoader)
		return;

	// 1フレームでの GPU リソース生成(VB/IB) + SRV 登録を分散してスパイクを抑える
	// (Assimp パース自体はワーカー側で完了している前提)
	constexpr size_t kSpawnBudgetPerFrame = 1;

	g_AsyncModelLoader->DrainCompleted(kSpawnBudgetPerFrame, [this](AsyncModelLoadResult&& r) {
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
		pbr->RimParams = XMFLOAT4(1.0f, 1.5f, 0.0f, 0.0f);
		pbr->CameraPos = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
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

	auto loadMask = [](const wchar_t* path) -> Texture2D*
	{
		if (fs::exists(path))
			return Texture2D::Get(std::wstring(path));
		return Texture2D::GetBlack();
	};

	(void)kTreeSpecies;
	static const wchar_t* terrainMaskPaths[] = {
		L"assets\\terrain\\tree_mask.png",
		L"assets\\terrain\\nature_mask.png",
	};
	s_terrainMaskHandle = nullptr;
	for (const wchar_t* path : terrainMaskPaths)
	{
		Texture2D* tex = loadMask(path);
		DescriptorHandle* h = descriptorHeap->Register(tex);
		if (!s_terrainMaskHandle)
			s_terrainMaskHandle = h;
	}

	terrainRootSignature = new RootSignature(true);
	terrainPipelineState = new PipelineState();
	terrainPipelineState->SetInputLayout(Vertex::InputLayout);
	terrainPipelineState->SetRootSignature(terrainRootSignature->Get());
	terrainPipelineState->SetVS(L"TerrainVS.cso");
	terrainPipelineState->SetPS(L"Terrain_PS.cso");
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
	}

	if (!terrainPipelineState->IsValid())
		return true;

	auto terrainEntity = m_registry.create();
	TransformComponent tcTerrain = {};
	XMStoreFloat4x4(&tcTerrain.BaseMatrix, XMMatrixIdentity());
	tcTerrain.Position = { 0.0f, 0.0f, 0.0f };
	tcTerrain.UniformScale = 1.0f;
	tcTerrain.RotationY = 0.0f;
	tcTerrain.WorldMatrix = XMMatrixIdentity();
	m_registry.emplace<TransformComponent>(terrainEntity, tcTerrain);

	MeshRendererComponent mrcTerrain = {};
	mrcTerrain.pVB = terrainResult.pVB;
	mrcTerrain.pIB = terrainResult.pIB;
	mrcTerrain.IndexCount = terrainResult.IndexCount;
	mrcTerrain.MaterialHandle = s_terrainMaskHandle;
	mrcTerrain.CastShadow = true;
	m_registry.emplace<MeshRendererComponent>(terrainEntity, mrcTerrain);

	TerrainComponent terrComp = {};
	terrComp.HeightData = std::move(terrainResult.HeightData);
	terrComp.GridWidth = terrainResult.GridWidth;
	terrComp.GridDepth = terrainResult.GridDepth;
	terrComp.CellSpacing = kTerrainCellSpacing;
	terrComp.MaxHeight = kTerrainMaxHeight;
	m_registry.emplace<TerrainComponent>(terrainEntity, terrComp);
	m_registry.emplace<LODComponent>(terrainEntity, 0, 0.0f);

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
	return pipelineState->IsValid();
}

bool Scene::InitSkyboxAndIBL()
{
	DebugLog("[Skybox] --- init begin ---\n");
	g_Engine->Allocator(0)->Reset();
	g_Engine->CommandList()->Reset(g_Engine->Allocator(0), nullptr);

	ID3D12Resource* cubemap = nullptr;
	ID3D12Resource* equirect = nullptr;
	IBLGenerator ibl;
	const auto doExecuteAndWait = []() { g_Engine->ExecuteAndWait(); };

	if (ibl.Generate(g_Engine->Device(), g_Engine->CommandList(), L"assets\\skybox.exr", 2560u, &cubemap, doExecuteAndWait, &equirect))
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
		if (IBLGenerator::CreateDefaultCubemap(g_Engine->Device(), g_Engine->CommandList(), 4, 0.25f, 0.45f, 0.85f, 1.0f, &defaultCubemap, defaultUpload.GetAddressOf()))
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
		g_Engine->CommandList()->Reset(g_Engine->Allocator(0), nullptr);
		ID3D12Resource* irradianceRaw = nullptr;
		if (ibl.GenerateIrradianceMap(g_Engine->Device(), g_Engine->CommandList(), skyboxCubemap.Get(), doExecuteAndWait, &irradianceRaw))
			s_irradianceCubemap = irradianceRaw;

		g_Engine->WaitForGpuIdle();
		g_Engine->Allocator(0)->Reset();
		g_Engine->CommandList()->Reset(g_Engine->Allocator(0), nullptr);
		ID3D12Resource* prefilterRaw = nullptr;
		if (ibl.GeneratePrefilteredEnvMap(g_Engine->Device(), g_Engine->CommandList(), skyboxCubemap.Get(), doExecuteAndWait, &prefilterRaw))
			s_prefilterCubemap = prefilterRaw;

		g_Engine->WaitForGpuIdle();
		g_Engine->Allocator(0)->Reset();
		g_Engine->CommandList()->Reset(g_Engine->Allocator(0), nullptr);
		ID3D12Resource* brdfLutRaw = nullptr;
		if (ibl.GenerateBrdfLut(g_Engine->Device(), g_Engine->CommandList(), doExecuteAndWait, &brdfLutRaw))
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

bool Scene::InitPostProcess()
{
	s_postProcessSettings.exposure = 1.0f;
	s_postProcessSettings.gamma = 2.2f;
	s_postProcessSettings.bloomIntensity = 0.6f;
	s_postProcessSettings.threshold = 0.8f;
	s_postProcessSettings.blurSize = 2.0f;
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
	if (s_postProcess && !s_postProcess->Init(g_Engine->Device(), descriptorHeap, w, h))
	{
		delete s_postProcess;
		s_postProcess = nullptr;
	}
	return true;
}

bool Scene::Init()
{
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

	constexpr float kPlayerScaleMultiplier = 1.0f;
	{
		ModelSpawnOptions player = {};
		player.position = { 0.0f, 0.0f, 0.0f };
		player.uniformScale = kPlayerScaleMultiplier;
		player.rotationY = 0.0f;
		player.foot = ModelSpawnOptions::FootPlacement::SnapFeetToTerrain;
		player.addPlayerComponent = true;
		if (!SpawnModelEntities(L"assets\\hibana\\hibana.pmx", player))
			return false;
	}

	if (!InitMainPipeline())
		return false;
	if (!InitSkyboxAndIBL())
		return false;
	if (!InitPostProcess())
		return false;

	return true;
}

void Scene::Update()
{
	ProcessAsyncModelLoads();

	float dt = 0.0016f;
	CameraSystem::Update(g_Camera, dt, m_registry);

	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto* sc = sceneConstantBuffer[currentIndex] ? sceneConstantBuffer[currentIndex]->GetPtr<SceneConstants>() : nullptr;
	if (!sc)
		return;

	float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
	sc->View = XMMatrixTranspose(g_Camera->GetViewMatrix());
	sc->Proj = XMMatrixTranspose(g_Camera->GetProjectionMatrix(aspect));

	// Terrain DrawMain reads SceneConstants; keep slot0 in sync for any legacy readers.
	auto currentTransform = constantBuffer[currentIndex]->GetPtr<Transform>();
	if (currentTransform)
	{
		currentTransform->View = sc->View;
		currentTransform->Proj = sc->Proj;
	}

	auto pbrConst = pbrPropertyBuffer[currentIndex]->GetPtr<PBRConstants>();
	if (pbrConst)
	{
		XMVECTOR camPos = g_Camera->GetPosition();
		XMStoreFloat4(&pbrConst->CameraPos, camPos);
	}
	if (terrainConstantBuffer[currentIndex])
	{
		auto* terrConst = terrainConstantBuffer[currentIndex]->GetPtr<TerrainConstants>();
		if (terrConst)
		{
			XMVECTOR camPos = g_Camera->GetPosition();
			XMStoreFloat4(&terrConst->CameraPos, camPos);
		}
	}

	XMFLOAT3 cameraPos;
	XMStoreFloat3(&cameraPos, g_Camera->GetPosition());
	PlayerSystem::Update(m_registry);
	TransformSystem::Update(m_registry);
	LODSystem::Update(m_registry, cameraPos);
}

void Scene::Draw()
{
	auto commandList = g_Engine->CommandList();

	EngineDoTransition(commandList, g_Engine->GetHdrColorResource(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);

	D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvHandle = g_Engine->GetHdrRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_Engine->GetDsvCpuHandle();
	commandList->OMSetRenderTargets(1, &hdrRtvHandle, FALSE, &dsvHandle);

	const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	commandList->ClearRenderTargetView(hdrRtvHandle, clearColor, 0, nullptr);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto materialHeap = descriptorHeap->GetHeap();
	commandList->SetDescriptorHeaps(1, &materialHeap);
	commandList->SetGraphicsRootSignature(rootSignature->Get());
	commandList->SetPipelineState(pipelineState->Get());

	D3D12_GPU_DESCRIPTOR_HANDLE envHandle = s_envCubemapHandle ? s_envCubemapHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };
	D3D12_GPU_DESCRIPTOR_HANDLE terrainMaskGPU = s_terrainMaskHandle ? s_terrainMaskHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };
	RenderSystem::DrawMain(m_registry, commandList,
		constantBuffer[currentIndex],
		sceneConstantBuffer[currentIndex],
		pbrPropertyBuffer[currentIndex],
		s_pbrInstanceRingBuffer.Get(),
		s_pbrInstanceRingMapped,
		currentIndex,
		rootSignature, pipelineState, descriptorHeap, envHandle,
		terrainRootSignature, terrainPipelineState, terrainConstantBuffer[currentIndex], terrainMaskGPU);

	if (s_skyboxRenderer && s_skyboxRenderer->IsValid())
	{
		float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
		s_skyboxRenderer->Draw(commandList, g_Camera->GetViewMatrix(), g_Camera->GetProjectionMatrix(aspect));
	}

	if (s_postProcess && s_postProcess->IsValid() && s_hdrSrvHandle)
	{
		ID3D12Resource* hdrRes = g_Engine->GetHdrColorResource();
		ID3D12Resource* backBufferRes = g_Engine->GetBackBufferResource();
		D3D12_RESOURCE_BARRIER barriers[2] = {};
		barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(hdrRes, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (backBufferRes)
			barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(backBufferRes, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		UINT barrierCount = backBufferRes ? 2 : 1;
		commandList->ResourceBarrier(barrierCount, barriers);

		commandList->SetDescriptorHeaps(1, &materialHeap);
		s_postProcess->Execute(commandList, materialHeap, s_hdrSrvHandle->HandleGPU, g_Engine->GetBackBufferRtvCpuHandle(), s_postProcessSettings);
	}
}
