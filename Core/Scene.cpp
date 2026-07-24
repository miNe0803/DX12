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
#include "Town/TownScene.h"
#include "Graphics/TerrainGenerator.h"
#include "Graphics/TerrainGpuCullSystem.h"
#include "Graphics/HiZSystem.h"
#include "Graphics/TreeGpuCullSystem.h"
#include "Graphics/ShadowSystem.h"
#include "Graphics/AtmosphereSystem.h"
#include "Graphics/SsrSystem.h"
#include "Graphics/GtaoSystem.h"
#include "Graphics/RtaoSystem.h"
#include "Graphics/DdgiSystem.h"
#include "Graphics/RtReflectionSystem.h"
#include "Graphics/VsmSystem.h"
#include "Graphics/RayTracingManager.h"   // DXR-GI F1: TLAS基盤
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
static SsrSystem* s_ssr = nullptr;
static GtaoSystem* s_gtao = nullptr;
static VsmSystem* s_vsm = nullptr;   // VSM本体（V1: 土台のみ。V4でCSM影を置換予定）
static RayTracingManager* s_rtManager = nullptr;  // DXR-GI: 静的町の BLAS/TLAS（F1で構築, R/G で RTAO/DDGI が利用）
static bool s_giEnabled = false;     // DXR-GI 有効（DX12_GI で初期化。F1は既定OFF＝TLAS構築せず無コスト・無変更）
static bool s_giDebugView = false;   // F1 検証ビュー表示（ImGui/DX12_GI_DEBUG）。TLAS構築(s_giEnabled)が前提
static RtaoSystem* s_rtao = nullptr; // Phase R: レイトレースAO（TLAS存在時のみ生成）。GTAOと排他
static bool s_rtaoEnabled = false;   // RTAO 有効（DX12_RTAO で初期化。ONでGTAOを置換。既定OFF＝GTAO経路不変）
static DdgiSystem* s_ddgi = nullptr; // Phase G: DDGI拡散GI（TLAS存在時のみ生成）
static bool s_ddgiEnabled = false;   // DDGI 有効（DX12_DDGI で初期化。ONで町の偽ambientを実GIに置換。既定OFF＝不変）
static RtReflectionSystem* s_rtr = nullptr; // 仕上げ: レイトレース反射（TLAS存在時のみ生成）
static bool s_rtrEnabled = false;    // RTR 有効（DX12_RTR で初期化。ONで濡れ地面が本物のRT反射に。既定OFF）
static TownScene* s_town = nullptr;   // [TOWN] Unreal T3D 町シーン
static std::vector<VsmSystem::RenderBatch> s_vsmRenderBatches;   // V3c-m3: 静的 submesh 描画バッチ（init時1回構築）
static bool s_vsmAtlasReady = false;   // V4: 最初のVSM描画+EndRenderStates後にtrue。町がサンプル可になる（フレーム1のガード）
// VSM ランタイムトグル（ImGui / Debug UI で操作。既定は環境変数 DX12_VSM / DX12_VSM_ATLAS / DX12_VSM_SHADOW）
static bool s_vsmEnabled = false;      // 太陽影を VSM でサンプル（OFF=従来CSM）
static bool s_vsmAtlasDebug = false;   // 物理アトラスをフルスクリーン表示（検証）
static bool s_vsmShadowDebug = false;  // VSM 影係数をフルスクリーン表示（検証）
static bool s_vsmForceRender = false;  // 診断: V5aキャッシュ無効化（毎フレーム再描画→ログ更新）DX12_VSM_NOCACHE
static bool s_vsmCache = false;        // V5b 永続キャッシュ（移動時は新規ページのみ描画）DX12_VSM_CACHE
static bool s_vsmGateInit = false;     // 環境変数からの初期化を1回だけ行う

std::wstring ReplaceExtension(const std::wstring& origin, const char* ext)
{
	fs::path p = origin.c_str();
	return p.replace_extension(ext).c_str();
}

DescriptorHeap* descriptorHeap = nullptr;

Scene* g_Scene = nullptr;
ConstantBuffer* constantBuffer[Engine::FRAME_BUFFER_COUNT] = {};
ConstantBuffer* sceneConstantBuffer[Engine::FRAME_BUFFER_COUNT] = {};
ConstantBuffer* reflectionConstantBuffer[Engine::FRAME_BUFFER_COUNT] = {}; // 平面反射: ミラーカメラCB
ConstantBuffer* pbrPropertyBuffer[Engine::FRAME_BUFFER_COUNT] = {};

RootSignature* rootSignature = nullptr;
PipelineState* pipelineState = nullptr;
PipelineState* nprPipelineState = nullptr;
PipelineState* nprTransparentPipelineState = nullptr;
RootSignature* terrainRootSignature = nullptr;
PipelineState* terrainDepthPrepassPipelineState = nullptr;
PipelineState* terrainPipelineState = nullptr;
PipelineState* waterPipelineState = nullptr;
PipelineState* oceanPipelineState = nullptr;
VertexBuffer*  s_oceanVB = nullptr;
IndexBuffer*   s_oceanIB = nullptr;
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
	constexpr bool kTreeCullUseHiZ = true; // HiZ オクルージョンカリングを有効化して非表示の木をスキップ

	static_assert(sizeof(TreeVegetation::StreamedTreeInstance) == sizeof(TreeGpuCullSystem::TreeInstanceCpu));
	static_assert(alignof(TreeVegetation::StreamedTreeInstance) == alignof(TreeGpuCullSystem::TreeInstanceCpu));

	SkyboxRenderer* s_skyboxRenderer = nullptr;
	ComPtr<ID3D12Resource> skyboxCubemap;
	ComPtr<ID3D12Resource> skyboxEquirect;
	ComPtr<ID3D12Resource> s_irradianceCubemap;
	ComPtr<ID3D12Resource> s_prefilterCubemap;
	ComPtr<ID3D12Resource> s_brdfLut;

	// ---- 平面反射（水たまり）: 半解像度の反射カラー+深度 + 専用 RTV/DSV ヒープ ----
	ComPtr<ID3D12Resource> s_reflColor;              // R16G16B16A16_FLOAT (rgb=町, a=被覆)
	ComPtr<ID3D12Resource> s_reflDepth;              // R32_TYPELESS
	ComPtr<ID3D12DescriptorHeap> s_reflRtvHeap;      // 1 slot
	ComPtr<ID3D12DescriptorHeap> s_reflDsvHeap;      // 1 slot
	UINT  s_reflW = 0, s_reflH = 0;
	float s_reflPlaneY = 0.0f;                       // 水面（ミラー）平面 Y（毎フレーム更新）
	bool  s_reflValid = false;                       // ターゲット作成済み

	// 半解像度の反射カラー/深度と RTV/DSV ヒープを作成（Engine のヒープは満杯なので専用ヒープ）。
	bool CreatePlanarReflectionTargets(ID3D12Device* dev, UINT w, UINT h)
	{
		if (!dev || w == 0 || h == 0) return false;
		s_reflW = w; s_reflH = h;
		// カラー（a=0 でクリア → 町PS が a=1 出力 → 被覆マスクになる）
		{
			auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 1, 1, 1, 0,
				D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
			D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			cv.Color[0] = cv.Color[1] = cv.Color[2] = cv.Color[3] = 0.0f;
			if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&s_reflColor)))) return false;
		}
		// 深度（R32_TYPELESS → D32_FLOAT DSV）
		{
			auto hp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
			auto rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, w, h, 1, 1, 1, 0,
				D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
			D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
			if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
				D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&s_reflDepth)))) return false;
		}
		// RTV ヒープ + RTV
		{
			D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
			if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_reflRtvHeap)))) return false;
			D3D12_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			dev->CreateRenderTargetView(s_reflColor.Get(), &rv, s_reflRtvHeap->GetCPUDescriptorHandleForHeapStart());
		}
		// DSV ヒープ + DSV
		{
			D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
			if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_reflDsvHeap)))) return false;
			D3D12_DEPTH_STENCIL_VIEW_DESC dv = {}; dv.Format = DXGI_FORMAT_D32_FLOAT;
			dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			dev->CreateDepthStencilView(s_reflDepth.Get(), &dv, s_reflDsvHeap->GetCPUDescriptorHandleForHeapStart());
		}
		return true;
	}

	PostProcessSystem* s_postProcess = nullptr;
	DescriptorHandle* s_hdrSrvHandle = nullptr;
	DescriptorHandle* s_envCubemapHandle = nullptr;
	DescriptorHandle* s_terrainMaskHandle = nullptr;
	// 拡張テレインテクスチャ (t9-t12): Rivers_Direction, WaterColor_Color, Trees2_FreshWater, INHIBITORS_Out
	DescriptorHandle* s_terrainExtraMaskHandle = nullptr;
	// 公開アクセサ用 GPU ハンドル (RenderSystem から参照)
	D3D12_GPU_DESCRIPTOR_HANDLE s_terrainExtraMaskGpuPub = {};
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
		//static int s_fillLog = 0;
		//if (s_fillLog < 3)
		//{
		//	DebugLog("[FillIdx] imposterReady=%d (bakeOk=%d quadVb=%p quadIb=%p mat0=%llu mat1=%llu mat2=%llu) "
		//		"idx[0]={%u,%u,%u} idx[1]={%u,%u,%u} idx[2]={%u,%u,%u}\n",
		//		imposterReady ? 1 : 0, s_treeImposterBakeOk ? 1 : 0,
		//		s_treeImposterQuadVb, s_treeImposterQuadIb,
		//		(unsigned long long)s_treeImposterMatGpu[0].ptr,
		//		(unsigned long long)s_treeImposterMatGpu[1].ptr,
		//		(unsigned long long)s_treeImposterMatGpu[2].ptr,
		//		out[0][0], out[0][1], out[0][2],
		//		out[1][0], out[1][1], out[1][2],
		//		out[2][0], out[2][1], out[2][2]);
		//	++s_fillLog;
		//}
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

	// [TOWN] 町シーン ( descriptorHeap を参照するため、その解放より前に )
	delete s_town;
	s_town = nullptr;

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
		delete reflectionConstantBuffer[i];
		reflectionConstantBuffer[i] = nullptr;
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
	if (s_ssr) { s_ssr->Shutdown(); delete s_ssr; s_ssr = nullptr; }
	if (s_gtao) { s_gtao->Shutdown(); delete s_gtao; s_gtao = nullptr; }
	if (s_rtao) { s_rtao->Shutdown(); delete s_rtao; s_rtao = nullptr; }
	if (s_ddgi) { s_ddgi->Shutdown(); delete s_ddgi; s_ddgi = nullptr; }
	if (s_rtr) { s_rtr->Shutdown(); delete s_rtr; s_rtr = nullptr; }
	if (s_vsm) { s_vsm->Shutdown(); delete s_vsm; s_vsm = nullptr; }
	if (s_rtManager) { s_rtManager->Shutdown(); delete s_rtManager; s_rtManager = nullptr; }
	s_reflValid = false;
	s_reflColor.Reset(); s_reflDepth.Reset(); s_reflRtvHeap.Reset(); s_reflDsvHeap.Reset();

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

		reflectionConstantBuffer[i] = new ConstantBuffer(sizeof(SceneConstants));
		if (!reflectionConstantBuffer[i]->IsValid())
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
		// Gaea: 主要河川マスク (WaterColor_Mɑsk.png をASCII名でコピー)。Rivers_Depth は全画素0 で使用不可、
		// Rivers_Rivers は毛細血管まで含み密すぎるため、この主要河川マスクを採用。シェーダ内で多点ぼかして滑らか化。
		L"assets\\terrain\\WaterColor_Mask_main.png",
		L"assets\\terrain\\Snow_Snow.png",
	};
	// t0-t5: tree_mask, nature_mask, ground_diff, ground_disp, rivers, snow
	Texture2D* terrainTex[6] = {
		loadTextureOrFallback(terrainTexturePaths[0], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainTexturePaths[1], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainTexturePaths[2], Texture2D::GetWhite()),
		loadTextureOrFallback(terrainTexturePaths[3], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainTexturePaths[4], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainTexturePaths[5], Texture2D::GetBlack()),
	};
	s_terrainMaskHandle = nullptr;
	for (Texture2D* tex : terrainTex)
	{
		DescriptorHandle* h = descriptorHeap->Register(tex);
		if (!s_terrainMaskHandle)
			s_terrainMaskHandle = h;
	}

	// ---- 拡張テレインマスク (t9-t12) ----
	// Gaea が出力した残りの有効テクスチャを総動員してリッチな見た目に。
	static const wchar_t* terrainExtraTexturePaths[] = {
		L"assets\\terrain\\Rivers_Direction.png",   // RG: 川の流れ方向 (B=128 中立)
		L"assets\\terrain\\WaterColor_Color.png",   // RGB: 川/湿地の真の水色
		L"assets\\terrain\\Trees2_FreshWater.png",  // 湿地マスク (川岸の濡れた土)
		L"assets\\terrain\\INHIBITORS_Out.png",     // 植生抑制マスク (岩肌・痩せ地)
	};
	Texture2D* terrainExtraTex[4] = {
		loadTextureOrFallback(terrainExtraTexturePaths[0], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainExtraTexturePaths[1], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainExtraTexturePaths[2], Texture2D::GetBlack()),
		loadTextureOrFallback(terrainExtraTexturePaths[3], Texture2D::GetBlack()),
	};
	s_terrainExtraMaskHandle = nullptr;
	for (Texture2D* tex : terrainExtraTex)
	{
		DescriptorHandle* h = descriptorHeap->Register(tex);
		if (!s_terrainExtraMaskHandle)
			s_terrainExtraMaskHandle = h;
	}
	s_terrainExtraMaskGpuPub = s_terrainExtraMaskHandle ? s_terrainExtraMaskHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 };

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

	// ---- Water PSO (半透明水面プレーン用) ----
	waterPipelineState = new PipelineState();
	waterPipelineState->SetInputLayout(Vertex::InputLayout);
	waterPipelineState->SetRootSignature(terrainRootSignature->Get());
	waterPipelineState->SetVS(L"Water_VS.cso");
	waterPipelineState->SetPS(L"Water_PS.cso");
	waterPipelineState->SetCullMode(D3D12_CULL_MODE_NONE);
	waterPipelineState->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ZERO); // 水面は深度読み取り専用
	waterPipelineState->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL); // 川底と同深度でも通す
	waterPipelineState->SetAlphaBlendPremultiplied();
	waterPipelineState->Create();
	DebugLog("[Water] PSO create: ptr=%p valid=%d\n",
		(void*)waterPipelineState, waterPipelineState ? (int)waterPipelineState->IsValid() : -1);

	// ---- Ocean PSO (地形メッシュ外側まで水を伸ばす巨大プレーン用) ----
	oceanPipelineState = new PipelineState();
	oceanPipelineState->SetInputLayout(Vertex::InputLayout);
	oceanPipelineState->SetRootSignature(terrainRootSignature->Get());
	oceanPipelineState->SetVS(L"Ocean_VS.cso");
	oceanPipelineState->SetPS(L"Ocean_PS.cso");
	oceanPipelineState->SetCullMode(D3D12_CULL_MODE_NONE);
	oceanPipelineState->SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK_ZERO);
	oceanPipelineState->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS_EQUAL);
	oceanPipelineState->SetAlphaBlendPremultiplied();
	oceanPipelineState->Create();
	DebugLog("[Ocean] PSO create: ptr=%p valid=%d\n",
		(void*)oceanPipelineState, oceanPipelineState ? (int)oceanPipelineState->IsValid() : -1);

	// ---- Ocean VB/IB: 4 頂点の超巨大クワッド (XZ ± 50km) ----
	{
		const float K = 50000.0f; // ±50km
		Vertex oceanVerts[4] = {};
		oceanVerts[0].Position = DirectX::XMFLOAT3(-K, 0.0f, -K); oceanVerts[0].UV = DirectX::XMFLOAT2(0, 0); oceanVerts[0].Normal = DirectX::XMFLOAT3(0,1,0);
		oceanVerts[1].Position = DirectX::XMFLOAT3(+K, 0.0f, -K); oceanVerts[1].UV = DirectX::XMFLOAT2(1, 0); oceanVerts[1].Normal = DirectX::XMFLOAT3(0,1,0);
		oceanVerts[2].Position = DirectX::XMFLOAT3(-K, 0.0f, +K); oceanVerts[2].UV = DirectX::XMFLOAT2(0, 1); oceanVerts[2].Normal = DirectX::XMFLOAT3(0,1,0);
		oceanVerts[3].Position = DirectX::XMFLOAT3(+K, 0.0f, +K); oceanVerts[3].UV = DirectX::XMFLOAT2(1, 1); oceanVerts[3].Normal = DirectX::XMFLOAT3(0,1,0);
		uint32_t oceanIdx[6] = { 0, 2, 1, 1, 2, 3 };
		s_oceanVB = new VertexBuffer(sizeof(oceanVerts), sizeof(Vertex), oceanVerts);
		s_oceanIB = new IndexBuffer(sizeof(oceanIdx), oceanIdx);
		DebugLog("[Ocean] VB=%p IB=%p\n", (void*)s_oceanVB, (void*)s_oceanIB);
	}

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
	s_postProcessSettings.exposure = 1.0f;   // 0.88→1.0: 全体の軽い暗さを解消
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

	// [TOWN] 地形・水・海・木・プレイヤー(hibana)は町シーンへ置き換えるため無効化。
	//        地形インフラ(root sig / PSO / 共有VB/IB / マスク)は生成されず null のまま。
	//        DrawMain は useTerrain=false、水/海パスは terrain バッファ null で no-op になる。
	// if (!InitTerrain())
	// 	return false;
	if (g_Camera)
	{
		// 町原点上空・通り方向を見る自由飛行の初期位置（Phase 1 で実際の範囲に合わせ調整）。
		g_Camera->SetPosition(XMVectorSet(0.0f, 30.0f, -60.0f, 0.0f));
	}

	// [TOWN] プレイヤーモデル(hibana.pmx)の生成は無効化。
	//constexpr float kPlayerScaleMultiplier = 0.1f;
	//{
	//	ModelSpawnOptions player = {};
	//	player.position = { 0.0f, 0.0f, 0.0f };
	//	player.uniformScale = kPlayerScaleMultiplier;
	//	player.rotationY = 0.0f;
	//	player.foot = ModelSpawnOptions::FootPlacement::SnapFeetToTerrain;
	//	player.addPlayerComponent = true;
	//	player.addNprTag = true;
	//	if (!SpawnModelEntities(L"assets\\hibana\\hibana.pmx", player))
	//		return false;
	//}

	if (!InitMainPipeline())
		return false;
	// [TOWN] 植生(木)の初期化は無効化。
	// TreeVegetation::Initialize(m_registry, descriptorHeap, m_ownedVertexBuffers, m_ownedIndexBuffers);
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

		// SSR（濡れた水たまり反射）
		s_ssr = new SsrSystem();
		if (!s_ssr->Init(g_Engine->Device(), w, h))
		{
			delete s_ssr;
			s_ssr = nullptr;
		}

		// GI G0: GTAO（スクリーン空間AO）
		s_gtao = new GtaoSystem();
		if (!s_gtao->Init(g_Engine->Device(), w, h))
		{
			delete s_gtao;
			s_gtao = nullptr;
		}

		// VSM本体 V1: 物理プール＋ページテーブル＋アドレッシング（土台。描画/サンプルは V3/V4）
		s_vsm = new VsmSystem();
		if (!s_vsm->Init(g_Engine->Device(), descriptorHeap, w, h))
		{
			delete s_vsm;
			s_vsm = nullptr;
		}

		// 平面反射ターゲット（フル解像度）: 水たまりが町の鏡像をサンプルする（後段でぼかす）
		s_reflValid = CreatePlanarReflectionTargets(g_Engine->Device(), w, h);
	}

	// Tree GPU cull: LOD0 は非同期のため、メッシュ確定後に TryEnsureTreeGpuCullInit() で確保。
	TryEnsureTreeGpuCullInit();

	// [TOWN] Unreal T3D 町シーンを初期化 ( IBL/影の後 = s_envCubemapHandle / s_shadow が有効 )
	{
		s_town = new TownScene();
		TownConfig townCfg; // Phase 1 既定 ( maxActors=150 )
		if (!s_town->Init(g_Engine->Device(), descriptorHeap, townCfg))
		{
			DebugLog("[Town] Init failed — 町シーンは表示されません\n");
			delete s_town;
			s_town = nullptr;
		}
		else if (g_Camera)
		{
			// 町の境界中心を見る初期カメラ（上空やや手前から俯瞰）。LookAt で yaw/pitch を設定。
			XMFLOAT3 mn = s_town->BoundsMin(), mx = s_town->BoundsMax();
			XMFLOAT3 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
			// 建物が実際に集まっている重心（≈原点）に注視。なければ全体の中心。
			XMFLOAT3 t = s_town->HasBuildingCenter() ? s_town->BuildingCenter() : c;
			// Camera::LookAt は垂直方向が反転している（forward.y = -y/dist、mouse の上下反転仕様）ため、
			// ターゲット Y をカメラ面で鏡像化して補正 → 実際に注視点を見下ろす。
			// 注視点の近く・低高度（街路レベル）に配置。WASD/矢印で自由移動可。
			// 既定は横断歩道のある街路を俯瞰（デカールが平らに見える良い初期ビュー）。
			// 環境変数 DX12_TOWN_OVERVIEW=1 で町の重心を遠望する俯瞰に切替。
			XMFLOAT3 cw; char ovEv[8]; char drEv[8]; char pdEv[8];
			bool wantOverview = (GetEnvironmentVariableA("DX12_TOWN_OVERVIEW", ovEv, sizeof(ovEv)) > 0);
			XMFLOAT3 dr;
			char eyeEv[8];
			// VSM アイレベル崩壊の厳密再現（診断用）: ユーザ報告の視点を正確に再構築。
			// Pos(-42.35,1.84,2.23), Forward(-0.409,-0.233,-0.882)。GetViewMatrix は Y のみ反転
			// (実forward=(td.x,-td.y,td.z)/dist) なので LookAt の目標方向は (Fx,-Fy,Fz)。
			// DX12_LOOK_EYE=1 でこの草地すれすれ視点に固定 → 要求ページ数を計測する。
			if (GetEnvironmentVariableA("DX12_LOOK_EYE", eyeEv, sizeof(eyeEv)) > 0)
			{
				const XMVECTOR eyePos = XMVectorSet(-42.35f, 1.84f, 2.23f, 0.0f);
				g_Camera->SetPosition(eyePos);
				g_Camera->LookAt(XMVectorAdd(eyePos, XMVectorSet(-0.409f, 0.233f, -0.882f, 0.0f)));
			}
			else if (GetEnvironmentVariableA("DX12_LOOK_PUDDLE", pdEv, sizeof(pdEv)) > 0 && s_town->FirstCrosswalkWorld(cw))
			{
				// 水たまりを浅い視線角で見る（SSRが建物/木を映す検証用）。低く・遠くから見下ろし。
				// LookAt は垂直反転仕様のため注視点 Y を鏡像化（2*camY - targetY）。
				const float camY = cw.y + 3.0f;
				g_Camera->SetPosition(XMVectorSet(cw.x, camY, cw.z - 22.0f, 0.0f));
				g_Camera->LookAt(XMVectorSet(cw.x, 2.0f * camY - cw.y, cw.z, 0.0f));
			}
			else if (GetEnvironmentVariableA("DX12_LOOK_DRIPS", drEv, sizeof(drEv)) > 0 && s_town->FirstDripWorld(dr))
			{
				// 壁/awning 水滴デカールを間近で確認（検証用）。少し離れて水平に見る。
				g_Camera->SetPosition(XMVectorSet(dr.x, dr.y + 1.0f, dr.z - 12.0f, 0.0f));
				g_Camera->LookAt(XMVectorSet(dr.x, dr.y, dr.z, 0.0f));
			}
			else if (!wantOverview && s_town->FirstCrosswalkWorld(cw))
			{
				// 横断歩道デカールを間近で俯瞰（街路レベル）。高さ/手前距離はここで調整可。
				const float camY = cw.y + 12.0f;   // 注視点からの高さ (m)
				const float camBack = 14.0f;       // 注視点から手前への距離 (m)
				// 検証用: DX12_CAM_DX でカメラと注視点を X 方向へ平行移動（WSAD相当）。
				// 影のワールド固定性チェック: dx を変えた2枚で影が地面に対しズレなければ安定。
				float dx = 0.0f; char dxEv[16];
				if (GetEnvironmentVariableA("DX12_CAM_DX", dxEv, sizeof(dxEv)) > 0) dx = (float)atof(dxEv);
				g_Camera->SetPosition(XMVectorSet(cw.x + dx, camY, cw.z - camBack, 0.0f));
				g_Camera->LookAt(XMVectorSet(cw.x + dx, 2.0f * camY - cw.y, cw.z, 0.0f));
			}
			else
			{
				// 町の重心付近を遠望する俯瞰。高さ/距離はここで調整可。
				const float camY = t.y + 35.0f;    // 注視点からの高さ (m)
				const float camBack = 75.0f;       // 注視点から手前への距離 (m)
				g_Camera->SetPosition(XMVectorSet(t.x, camY, t.z - camBack, 0.0f));
				g_Camera->LookAt(XMVectorSet(t.x, 2.0f * camY - t.y, t.z, 0.0f));
			}
		}
	}

	// VSM V3c-m2/m3: 町の静的キャスタレコード + submesh バッチを VSM へ登録（init 時1回）。
	if (s_town && s_vsm && s_vsm->IsValid())
	{
		s_vsm->SetCasterSource(s_town->CasterRecordsVA(), s_town->CasterCount(), s_town->CasterModelCount(),
			s_town->SubmeshTableVA(), s_town->VsmBatchCount());
		s_vsmRenderBatches.clear();
		s_vsmRenderBatches.reserve(s_town->VsmBatches().size());
		for (const auto& b : s_town->VsmBatches())
			s_vsmRenderBatches.push_back(VsmSystem::RenderBatch{ b.vbv, b.ibv });
	}

	// [DXR-GI F1] 静的町の BLAS/TLAS を一度だけ構築（DX12_GI 有効時のみ）。町の VB/IB は
	// s_town->Init 内の FlushUploads で resident 済み。専用コマンドリスト(CmdList4)で構築→実行→
	// GPU-idle→scratch解放。既定OFF＝TLAS未構築で無コスト・描画は完全に従来通り（バイト一致）。
	{
		char ev[8];
		// 既定ON: TLAS を毎起動構築し、RTAO/DDGI を ImGui からいつでも切替可能にする（RTAO/DDGI 自体は
		// 既定OFFなので描画は不変・ロードコストのみ）。DX12_GI=0 で完全無効化（構築せずゼロコスト）。
		DWORD giN = GetEnvironmentVariableA("DX12_GI", ev, sizeof(ev));
		s_giEnabled = !(giN > 0 && ev[0] == '0');
		s_giDebugView = (GetEnvironmentVariableA("DX12_GI_DEBUG", ev, sizeof(ev)) > 0);  // 初期値（ImGuiで切替可）
	}
	if (s_giEnabled && s_town && g_Engine->GetFeatureSupport().raytracingSupported)
	{
		s_rtManager = new RayTracingManager();
		D3D12_VIEWPORT vpRt = g_Engine->GetViewport();
		if (s_rtManager->Init(g_Engine->Device(), descriptorHeap, (uint32_t)vpRt.Width, (uint32_t)vpRt.Height))
		{
			auto* dev = g_Engine->Device();
			ComPtr<ID3D12CommandAllocator> alloc;
			ComPtr<ID3D12GraphicsCommandList> list;
			ComPtr<ID3D12GraphicsCommandList4> list4;
			if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
				SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) &&
				SUCCEEDED(list.As(&list4)))
			{
				s_town->BuildRayTracingScene(s_rtManager, list4.Get());
				list4->Close();
				ID3D12CommandList* lists[] = { list4.Get() };
				g_Engine->Queue()->ExecuteCommandLists(1, lists);
				g_Engine->WaitForGpuIdle();
				s_rtManager->ReleaseBuildScratch();   // 非同期構築完了後に一時scratchを解放
				printf("[DXR] TLAS built: %u instances (GI enabled)\n", s_rtManager->GetInstanceCount());
				fflush(stdout);

				// Phase R: TLAS が構築できた時だけ RTAO を生成（OFF時は無コスト・GTAO経路不変）
				{
					char rtEv[8];
					s_rtaoEnabled = (GetEnvironmentVariableA("DX12_RTAO", rtEv, sizeof(rtEv)) > 0) && (rtEv[0] != '0');
					D3D12_VIEWPORT vpAo = g_Engine->GetViewport();
					s_rtao = new RtaoSystem();
					if (!s_rtao->Init(g_Engine->Device(), (UINT)vpAo.Width, (UINT)vpAo.Height))
					{ delete s_rtao; s_rtao = nullptr; s_rtaoEnabled = false; }
				}
				// Phase G: TLAS がある時だけ DDGI を生成（OFF時は無コスト・町ambient不変）
				{
					char ddEv[8];
					s_ddgiEnabled = (GetEnvironmentVariableA("DX12_DDGI", ddEv, sizeof(ddEv)) > 0) && (ddEv[0] != '0');
					s_ddgi = new DdgiSystem();
					if (!s_ddgi->Init(g_Engine->Device(), s_town->BoundsMin(), s_town->BoundsMax()))
					{ delete s_ddgi; s_ddgi = nullptr; s_ddgiEnabled = false; }
				}
				// 仕上げ: TLAS がある時だけ RTR（レイトレース反射）を生成
				{
					char rrEv[8];
					s_rtrEnabled = (GetEnvironmentVariableA("DX12_RTR", rrEv, sizeof(rrEv)) > 0) && (rrEv[0] != '0');
					D3D12_VIEWPORT vpR = g_Engine->GetViewport();
					s_rtr = new RtReflectionSystem();
					if (!s_rtr->Init(g_Engine->Device(), descriptorHeap, (UINT)vpR.Width, (UINT)vpR.Height))
					{ delete s_rtr; s_rtr = nullptr; s_rtrEnabled = false; }
				}
			}
		}
		else { delete s_rtManager; s_rtManager = nullptr; }
	}

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

// ---- VSM ランタイムトグル（Debug UI）----
void Scene::SetVsmEnabled(bool enabled)
{
	if (enabled && !s_vsmEnabled)
	{
		s_vsmAtlasReady = false;                       // 有効化直後にアトラス強制再描画
		if (s_vsm && s_vsm->IsValid()) s_vsm->RequestCacheReset();  // キャッシュも作り直し（stale回避）
	}
	s_vsmEnabled = enabled;
	s_vsmGateInit = true;   // 以後 env で上書きしない（UI の選択を尊重）
}
bool Scene::GetVsmEnabled() const { return s_vsmEnabled; }
bool Scene::VsmAvailable() const { return s_vsm && s_vsm->IsValid(); }
void Scene::SetVsmAtlasDebug(bool on) { s_vsmAtlasDebug = on; s_vsmGateInit = true; }
bool Scene::GetVsmAtlasDebug() const { return s_vsmAtlasDebug; }
void Scene::SetVsmShadowDebug(bool on) { s_vsmShadowDebug = on; s_vsmGateInit = true; }
bool Scene::GetVsmShadowDebug() const { return s_vsmShadowDebug; }
void Scene::SetVsmCache(bool on) { s_vsmCache = on; s_vsmGateInit = true; }   // 次フレームの SetCacheMode で反映（切替時に内部リセット）
bool Scene::GetVsmCache() const { return s_vsmCache; }
void Scene::SetVsmFootprintLod(bool on) { if (s_vsm) s_vsm->SetFootprintLod(on); }   // マーカー/サンプラ/デバッグを次フレームでロックステップ切替
bool Scene::GetVsmFootprintLod() const { return s_vsm && s_vsm->GetFootprintLod(); }
uint32_t Scene::GetVsmLastPairCount() const { return s_vsm ? s_vsm->LastPairCount() : 0u; }
uint32_t Scene::GetVsmResidentPages() const { return s_vsm ? s_vsm->LastResidentPages() : 0u; }
uint32_t Scene::GetVsmRequestedPages() const { return s_vsm ? s_vsm->LastRequestedPages() : 0u; }
void Scene::SetGiDebug(bool on) { s_giDebugView = on; }
bool Scene::GetGiDebug() const { return s_giDebugView; }
bool Scene::GetGiEnabled() const { return s_giEnabled && s_rtManager && s_rtManager->IsValid(); }
void Scene::SetRtaoEnabled(bool on) { s_rtaoEnabled = on; }
bool Scene::GetRtaoEnabled() const { return s_rtaoEnabled; }
bool Scene::RtaoAvailable() const { return s_rtao && s_rtao->IsValid() && s_rtManager && s_rtManager->IsValid() && s_rtManager->GetInstanceCount() > 0; }
void Scene::SetDdgiEnabled(bool on) { s_ddgiEnabled = on; }
bool Scene::GetDdgiEnabled() const { return s_ddgiEnabled; }
bool Scene::DdgiAvailable() const { return s_ddgi && s_ddgi->IsValid() && s_rtManager && s_rtManager->IsValid() && s_rtManager->GetInstanceCount() > 0; }
bool Scene::GetDdgiReady() const { return s_ddgi && s_ddgi->IsReady(); }
uint32_t Scene::GetDdgiProbeCount() const { return s_ddgi ? s_ddgi->ProbeCount() : 0u; }
uint32_t Scene::GetGiInstanceCount() const { return s_rtManager ? s_rtManager->GetInstanceCount() : 0u; }
void Scene::SetDdgiIntensity(float v) { if (s_ddgi) s_ddgi->SetIntensity(v); }
float Scene::GetDdgiIntensity() const { return s_ddgi ? s_ddgi->GetIntensity() : 0.0f; }
void Scene::SetRtrEnabled(bool on) { s_rtrEnabled = on; }
bool Scene::GetRtrEnabled() const { return s_rtrEnabled; }
bool Scene::RtrAvailable() const { return s_rtr && s_rtr->IsValid() && s_rtManager && s_rtManager->IsValid() && s_rtManager->GetInstanceCount() > 0 && s_envCubemapHandle; }

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

	// 検証用: DX12_VSM_AUTOPAN=<m/frame> で毎フレーム g_Camera を X 方向へ平行移動。
	// 移動時の VSM キャッシュ挙動（トロイダルスロットの wrap・カウンタ・影の正しさ）を静止キャプチャで検証するため。
	{
		static float s_panSpd = [] { char e[16]; return GetEnvironmentVariableA("DX12_VSM_AUTOPAN", e, sizeof(e)) > 0 ? (float)atof(e) : 0.0f; }();
		if (s_panSpd != 0.0f && g_Camera)
			g_Camera->SetPosition(DirectX::XMVectorAdd(g_Camera->GetPosition(), DirectX::XMVectorSet(s_panSpd, 0.0f, 0.0f, 0.0f)));
		// 検証用: DX12_VSM_AUTOYAW=<rad/frame> で毎フレーム水平回転（位置固定・向きだけ変化）。
		// 回転は見えるページ集合を総入れ替え＝キャッシュ churn/プール溢れを起こす。平行移動では出ない破綻を再現。
		static float s_yawSpd = [] { char e[16]; return GetEnvironmentVariableA("DX12_VSM_AUTOYAW", e, sizeof(e)) > 0 ? (float)atof(e) : 0.0f; }();
		if (s_yawSpd != 0.0f && g_Camera)
		{
			static float s_yawAcc = 0.0f; s_yawAcc += s_yawSpd;
			XMVECTOR p = g_Camera->GetPosition();
			// LookAt は垂直反転仕様: target Y をカメラより上に置くと見下ろし（地面グレージング）になる。
			XMVECTOR tgt = XMVectorAdd(p, XMVectorSet(sinf(s_yawAcc) * 10.0f, 8.0f, cosf(s_yawAcc) * 10.0f, 0.0f));
			g_Camera->LookAt(tgt);
		}
	}

	float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
	XMMATRIX viewMat = g_Camera->GetViewMatrix();
	XMMATRIX projMat = g_Camera->GetProjectionMatrix(aspect);
	sc->View = XMMatrixTranspose(viewMat);
	sc->Proj = XMMatrixTranspose(projMat);
	XMStoreFloat4(&sc->CameraWorld, g_Camera->GetPosition());
	static float s_sceneTimeSec = 0.0f; s_sceneTimeSec += 1.0f / 60.0f; // 葉の風アニメ用の経過時間
	sc->CameraWorld.w = s_sceneTimeSec;

	// Sun direction from azimuth/elevation (AtmosphereParams)
	float azRad = XMConvertToRadians(s_atmosphereParams.sunAzimuth);
	float elRad = XMConvertToRadians(s_atmosphereParams.sunElevation);
	float cosEl = cosf(elRad);
	XMVECTOR sunDir = XMVector3Normalize(XMVectorSet(
		sinf(azRad) * cosEl,
		sinf(elRad),
		cosf(azRad) * cosEl,
		0.0f));
	XMStoreFloat4(&sc->SunDirection, sunDir);
	sc->SunDirection.w = 1.0f;
	sc->SunColor = XMFLOAT4(
		s_atmosphereParams.sunColorR * s_atmosphereParams.sunIntensity,
		s_atmosphereParams.sunColorG * s_atmosphereParams.sunIntensity,
		s_atmosphereParams.sunColorB * s_atmosphereParams.sunIntensity,
		1.0f);
	{
		XMVECTOR det;
		XMMATRIX vp = viewMat * projMat;
		XMMATRIX invVp = XMMatrixInverse(&det, vp);
		if (XMVectorGetX(XMVectorAbs(det)) < 1e-10f)
			invVp = XMMatrixIdentity();
		sc->InvViewProj = XMMatrixTranspose(invVp);
	}

	// ---- 平面反射用ミラーカメラCB（水面 y=s_reflPlaneY で反射）----
	// ミラーは View にだけ畳み込む（worldPos/法線は真のワールドのまま＝影/IBL が正しい）。
	// CameraWorld は反射した視点位置に（TownPS の視線依存項が正しく反射像を陰影付けするため）。
	if (reflectionConstantBuffer[currentIndex] && s_town)
	{
		XMFLOAT3 cw;
		s_reflPlaneY = s_town->FirstCrosswalkWorld(cw) ? cw.y : 0.0f;
		if (auto* rc = reflectionConstantBuffer[currentIndex]->GetPtr<SceneConstants>())
		{
			*rc = *sc;                                             // Sun/クラスタ等を継承
			XMMATRIX mirror   = XMMatrixReflect(XMVectorSet(0.0f, 1.0f, 0.0f, -s_reflPlaneY));
			XMMATRIX reflView = mirror * viewMat;                  // row-vector: p*mirror*view
			XMMATRIX reflVP   = reflView * projMat;
			rc->View = XMMatrixTranspose(reflView);
			rc->Proj = XMMatrixTranspose(projMat);                 // Proj はメインと同一
			XMVECTOR rdet; XMMATRIX rinv = XMMatrixInverse(&rdet, reflVP);
			if (XMVectorGetX(XMVectorAbs(rdet)) < 1e-10f) rinv = XMMatrixIdentity();
			rc->InvViewProj = XMMatrixTranspose(rinv);
			XMVECTOR eye = g_Camera->GetPosition();
			XMVECTOR reflEye = XMVectorSetY(eye, 2.0f * s_reflPlaneY - XMVectorGetY(eye));
			XMStoreFloat4(&rc->CameraWorld, reflEye);
			rc->CameraWorld.w = 1.0f;
		}
	}

	if (s_shadow && s_shadow->IsValid())
	{
		XMFLOAT3 sunDirF3;
		XMStoreFloat3(&sunDirF3, sunDir);
		// 影の動的距離。500m は 4 カスケードに対し広すぎ→cascade0 のテクセルが ~4cm と粗く、
		// カメラ移動時のエイリアス・ちらつきの主因だった。UE5 の DynamicShadowDistance 同様に
		// 歩行範囲(~160m)へ絞ると cascade0 は ~1.3cm と 3倍以上緻密＝クッキリ＆安定。
		XMFLOAT3 shadowCamPosF3; XMStoreFloat3(&shadowCamPosF3, g_Camera->GetPosition());
		s_shadow->UpdateCascades(viewMat, projMat, sunDirF3, shadowCamPosF3, 0.1f, 160.0f);

		// VSM本体 V1: クリップマップ定数を更新（光空間クリップマップ。描画/サンプルは V3/V4 で有効化）。
		if (s_vsm && s_vsm->IsValid())
		{
			XMVECTOR sunV = XMLoadFloat3(&sunDirF3);            // 光へ向かう方向
			XMVECTOR lightDir = XMVector3Normalize(XMVectorNegate(sunV)); // 光の進行方向
			XMVECTOR up = XMVectorSet(0, 1, 0, 0);
			if (fabsf(XMVectorGetY(lightDir)) > 0.99f) up = XMVectorSet(0, 0, 1, 0);
			XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), lightDir, up);
			XMVECTOR vsmDet;
			XMMATRIX vsmInvVP = XMMatrixInverse(&vsmDet, viewMat * projMat);
			// 診断 DX12_VSM_CAMOFS: VSMの描画カメラを実カメラからXにオフセット。
			// 「フレームN-1のアトラスをフレームNのカメラでサンプル＝移動ラグ」を静止画で再現し、
			// レジデンシー不足(移動先の必要ページが古アトラスに無い→崩れ)を切り分けるための実験フラグ。
			static float s_vsmCamOfs = [] {
				char e[16]; return GetEnvironmentVariableA("DX12_VSM_CAMOFS", e, sizeof(e)) > 0 ? (float)atof(e) : 0.0f;
			}();
			XMFLOAT3 vsmCamPos = shadowCamPosF3; vsmCamPos.x -= s_vsmCamOfs;
			s_vsm->UpdateConstants(lightView, vsmInvVP, vsmCamPos, -800.0f, 800.0f);
		}
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
			// Pack elapsed time into CameraPos.w for water wave animation
			static float s_terrainElapsedTime = 0.0f;
			s_terrainElapsedTime += 1.0f / 60.0f;
			terrConst->CameraPos.w = s_terrainElapsedTime;
			terrConst->DebugParams = XMFLOAT4(
				static_cast<float>(m_terrainPsDebugMode),
				m_terrainCheapPathEnabled ? 1.0f : 0.0f,
				m_terrainCheapGrazingThresh,
				m_terrainCheapNearPreserveMeters);
			terrConst->SunDirection = sc->SunDirection;
			terrConst->SunColor = sc->SunColor;
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

		const bool wantTreeShadows = s_treeGpuCull && s_treeGpuCull->IsValid()
			&& s_shadow->HasTreeShadowPipeline()
			&& s_treeGpuCull->GetInstanceCount() > 0
			&& s_atmosphereParams.enableTreeShadows;

		// 影にはトランク（幹）メッシュのみ使用（軽量: 3621 idx vs LOD0 merged: 155940 idx）
		int shadowLod = 0;

		const bool useGpuCull = wantTreeShadows && s_shadow->HasShadowCullPipeline()
			&& s_treeGpuCull->GetTreeInfoResource();

		const UINT maxShadowInstances = static_cast<UINT>(s_atmosphereParams.treeShadowMaxInstances);
		if (useGpuCull)
		{
			XMFLOAT3 camWorldPos;
			XMStoreFloat3(&camWorldPos, g_Camera->GetPosition());

			VertexBuffer* preVb = TreeVegetation::GetMergedVertexBufferLod(0);
			IndexBuffer*  preIb = TreeVegetation::GetMergedIndexBufferLod(0);
			uint32_t preIdxCount = TreeVegetation::GetMergedIndexCountLod(0);
			if (preVb && preIb && preIdxCount > 0)
				s_shadow->SetShadowIndexCount(0, preIdxCount);

			const float baseShadowDist = s_atmosphereParams.treeShadowDistance;
			const UINT treeCascades = static_cast<UINT>(s_atmosphereParams.treeShadowCascades);
			for (UINT cascade = 0; cascade < std::min(treeCascades, ShadowSystem::kCascadeCount); ++cascade)
			{
				const float cascadeDist = baseShadowDist * (1u << cascade);
				s_shadow->DispatchShadowCull(
					commandList, cascade,
					s_treeGpuCull->GetTreeInfoResource(),
					s_treeGpuCull->GetInstanceCount(),
					maxShadowInstances,
					camWorldPos,
					cascadeDist);
			}
		}

		// 全カスケードをクリア（未初期化防止）。描画は cascade 0 のみ。
		for (UINT cascade = 0; cascade < ShadowSystem::kCascadeCount; ++cascade)
		{
			s_shadow->BeginShadowPass(commandList, cascade);
			commandList->SetPipelineState(s_shadow->GetShadowPSO());
			commandList->SetGraphicsRootSignature(s_shadow->GetShadowRootSignature());
			commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// VSM主軸時（既定）: 町/木/建物の影は全てVSMアトラスが担うので、CSMカスケードへの
			// キャスタ描画をスキップ（クリア＋SRV遷移のみ）＝同じ~4000キャスタをCSM4面+VSMアトラスへ描く
			// 二重描画を排除。町PS(SampleSunShadowHybrid)は VSM非被覆画素を lit 扱いにしCSMを参照しない。
			// DX12_VSM=0 でVSMを切ると従来通りCSMがキャスタを描画する。
			if (s_vsmEnabled) { s_shadow->EndShadowPass(commandList, cascade); continue; }

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

			// 町の影キャスト（全4カスケード。TownPS が viewDepth でカスケードを選択して
			// サンプルするため、~38m 以遠にも影が出る）。キャスタは camera 距離でカリング（回転不変）。
			if (s_town)
			{
				XMFLOAT3 shadowCamPos; XMStoreFloat3(&shadowCamPos, g_Camera->GetPosition());
				s_town->DrawDepth(commandList, lightVP, s_shadow, shadowCamPos, s_shadow->GetCascadeSplit(cascade));
			}

			// Tree shadows — LOD0 フルメッシュ
			if (wantTreeShadows && cascade < static_cast<UINT>(s_atmosphereParams.treeShadowCascades))
			{
				VertexBuffer* treeVb = TreeVegetation::GetMergedVertexBufferLod(0);
				IndexBuffer*  treeIb = TreeVegetation::GetMergedIndexBufferLod(0);
				uint32_t treeIdxCount = TreeVegetation::GetMergedIndexCountLod(0);
				if (treeVb && treeIb && treeIdxCount > 0)
				{
					if (useGpuCull)
					{
						s_shadow->DrawTreeShadowsIndirect(
							commandList, cascade,
							s_treeGpuCull->GetInstanceDataResource(),
							treeVb, treeIb,
							nullptr, D3D12_GPU_DESCRIPTOR_HANDLE{0});
					}
					else
					{
						UINT drawCount = std::min(s_treeGpuCull->GetInstanceCount(), maxShadowInstances);
						s_shadow->DrawTreeShadows(
							commandList, cascade,
							s_treeGpuCull->GetInstanceDataResource(),
							drawCount,
							treeVb, treeIb, treeIdxCount,
							nullptr, D3D12_GPU_DESCRIPTOR_HANDLE{0});
					}
				}
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

	// ---- [TOWN] 平面反射パス: 町の不透明ジオメトリを水面平面でミラーして半解像度 RT へ。
	//      水たまり解決パス(SsrSystem)が自分の画面UVでこの反射像をサンプルする（角度非依存）。----
	if (s_reflValid && s_reflColor && s_town && g_Camera && reflectionConstantBuffer[currentIndex])
	{
		GPU_CMD_BEGIN_EVENT(commandList, 90, 150, 210, L"Town Planar Reflection");
		ID3D12Resource* reflRes = s_reflColor.Get();
		auto toRT = CD3DX12_RESOURCE_BARRIER::Transition(reflRes,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		commandList->ResourceBarrier(1, &toRT);

		TownReflectionPass rp;
		rp.rtv = s_reflRtvHeap->GetCPUDescriptorHandleForHeapStart();
		rp.dsv = s_reflDsvHeap->GetCPUDescriptorHandleForHeapStart();
		rp.vp = { 0.0f, 0.0f, (float)s_reflW, (float)s_reflH, 0.0f, 1.0f };
		rp.scissor = { 0, 0, (LONG)s_reflW, (LONG)s_reflH };

		commandList->OMSetRenderTargets(1, &rp.rtv, FALSE, &rp.dsv);
		commandList->RSSetViewports(1, &rp.vp);
		commandList->RSSetScissorRects(1, &rp.scissor);
		const float kReflClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // a=0 → 被覆なし（クリア値と一致必須）
		commandList->ClearRenderTargetView(rp.rtv, kReflClear, 0, nullptr);
		commandList->ClearDepthStencilView(rp.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

		float aspect = (float)g_Engine->GetFrameBufferWidth() / (float)g_Engine->GetFrameBufferHeight();
		XMMATRIX viewMat = g_Camera->GetViewMatrix();
		XMMATRIX projMat = g_Camera->GetProjectionMatrix(aspect);
		XMMATRIX reflVP = XMMatrixReflect(XMVectorSet(0.0f, 1.0f, 0.0f, -s_reflPlaneY)) * viewMat * projMat;
		XMVECTOR eye = g_Camera->GetPosition();
		XMFLOAT3 reflCam; XMStoreFloat3(&reflCam, XMVectorSetY(eye, 2.0f * s_reflPlaneY - XMVectorGetY(eye)));

		s_town->Draw(commandList,
			reflectionConstantBuffer[currentIndex]->GetAddress(),
			envHandle,
			(s_shadow && s_shadow->IsValid()) ? s_shadow->GetShadowMapSrvGpu() : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 },
			(s_shadow && s_shadow->IsValid()) ? s_shadow->GetShadowCBAddress() : 0,
			reflVP, reflCam, &rp);

		// 反射RTの「空」部分（建物以外＝深度=遠）へ、ミラーした本物のスカイボックス（雲）を描く。
		// これで水面が平坦なグラデでなく実際の空を映す＝「濡れた水鏡」に見える。
		if (s_skyboxRenderer && s_skyboxRenderer->IsValid())
		{
			commandList->OMSetRenderTargets(1, &rp.rtv, FALSE, &rp.dsv);
			commandList->RSSetViewports(1, &rp.vp);
			commandList->RSSetScissorRects(1, &rp.scissor);
			commandList->SetDescriptorHeaps(1, &materialHeap);
			XMMATRIX reflView = XMMatrixReflect(XMVectorSet(0.0f, 1.0f, 0.0f, -s_reflPlaneY)) * viewMat;
			s_skyboxRenderer->Draw(commandList, reflView, projMat);
		}

		auto toSRV = CD3DX12_RESOURCE_BARRIER::Transition(reflRes,
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		commandList->ResourceBarrier(1, &toSRV);

		// メイン HDR RT + フル解像度ビューポートへ復帰（後続の DrawMain / 町本描画のため）
		commandList->OMSetRenderTargets(1, &hdrRtvHandle, FALSE, &dsvHandle);
		commandList->RSSetViewports(1, &g_Engine->GetViewport());
		commandList->RSSetScissorRects(1, &g_Engine->GetScissorRect());
		GPU_CMD_END_EVENT(commandList);
	}

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
		treeLod2PipelineState,
		(s_shadow && s_shadow->IsValid()) ? s_shadow->GetShadowMapSrvGpu() : D3D12_GPU_DESCRIPTOR_HANDLE{0},
		(s_shadow && s_shadow->IsValid()) ? s_shadow->GetShadowCBAddress() : 0);
	GPU_CMD_END_EVENT(commandList);
	Profiler::GpuMarkDrawMainEnd(commandList);

	// ---- [TOWN] Unreal T3D 町シーン描画（メイン HDR へ、skybox の前）----
	static char s_townOffEv[8];
	static const bool s_townOff = (GetEnvironmentVariableA("DX12_TOWN_OFF", s_townOffEv, sizeof(s_townOffEv)) > 0);
	if (s_town && !s_townOff)
	{
		GPU_CMD_BEGIN_EVENT(commandList, 200, 160, 80, L"Town Scene");
		XMMATRIX townViewProj = XMMatrixIdentity();
		XMFLOAT3 townCamPos(0, 0, 0);
		if (g_Camera)
		{
			float aspect = (float)g_Engine->GetFrameBufferWidth() / (float)g_Engine->GetFrameBufferHeight();
			townViewProj = g_Camera->GetViewMatrix() * g_Camera->GetProjectionMatrix(aspect);
			XMStoreFloat3(&townCamPos, g_Camera->GetPosition());
		}
		// V4: VSM 太陽シャドウを町へバインド（s_vsmEnabled 時のみ CSM を置換）。アトラスは前フレーム分を参照。
		if (s_vsm && s_vsm->IsValid())
			s_town->SetVsmBindings(s_vsm->GetRenderedConstantsAddress(),   // V5b Stage0: 描画時の中心で引く→移動の揺れ消
				s_vsm->GetPageTable()->GetGPUVirtualAddress(),
				s_vsm->GetAtlasSrvGpu(), s_vsmEnabled && s_vsmAtlasReady,   // ランタイムトグル + アトラス準備後のみ
				s_vsm->GetFootprintLod());   // Phase 1: フットプリントLOD をサンプラ(TownPS)へロックステップ配線
		// Phase G: DDGI を町へバインド（前フレームの完了バッファを参照。≥2フレーム蓄積後のみ有効化）。
		if (s_ddgi && s_ddgi->IsValid())
			s_town->SetDdgiBindings(s_ddgi->GetCbVA(), s_ddgi->GetReadBufferVA(),
				s_ddgiEnabled && s_ddgi->IsReady());
		else
			s_town->SetDdgiBindings(0, 0, false);   // ダミーへフォールバック
		s_town->Draw(commandList,
			sceneConstantBuffer[currentIndex]->GetAddress(),
			envHandle,
			(s_shadow && s_shadow->IsValid()) ? s_shadow->GetShadowMapSrvGpu() : D3D12_GPU_DESCRIPTOR_HANDLE{ 0 },
			(s_shadow && s_shadow->IsValid()) ? s_shadow->GetShadowCBAddress() : 0,
			townViewProj, townCamPos);
		GPU_CMD_END_EVENT(commandList);
	}

	// ---- Water Pass: 水面プレーン（テレインメッシュを Y=水位に固定して描画）----
	if (waterPipelineState && waterPipelineState->IsValid() && m_terrainSharedVB && m_terrainSharedIB

		&& terrainConstantBuffer[currentIndex] && s_terrainMaskHandle)
	{
		GPU_CMD_BEGIN_EVENT(commandList, 60, 120, 200, L"Water Pass");
		commandList->SetPipelineState(waterPipelineState->Get());
		commandList->SetGraphicsRootSignature(terrainRootSignature->Get());
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// CB: Transform + TerrainParams
		auto* waterTransform = constantBuffer[currentIndex]->GetPtr<Transform>();
		if (waterTransform)
		{
			commandList->SetGraphicsRootConstantBufferView(0, constantBuffer[currentIndex]->GetAddress());
		}
		commandList->SetGraphicsRootConstantBufferView(1, terrainConstantBuffer[currentIndex]->GetAddress());
		commandList->SetGraphicsRootDescriptorTable(2, terrainMaskGPU); // terrain textures (t0-t5)
		if (envHandle.ptr != 0)
			commandList->SetGraphicsRootDescriptorTable(3, envHandle); // IBL (t6-t8)
		// Shadow map at slots 5, 6
		if (s_shadow && s_shadow->IsValid())
		{
			commandList->SetGraphicsRootDescriptorTable(5, s_shadow->GetShadowMapSrvGpu());
			commandList->SetGraphicsRootConstantBufferView(6, s_shadow->GetShadowCBAddress());
		}
		// 拡張テレインテクスチャ (t9-t12): Rivers_Direction, WaterColor_Color, FreshWater, INHIBITORS
		if (s_terrainExtraMaskHandle)
			commandList->SetGraphicsRootDescriptorTable(7, s_terrainExtraMaskHandle->HandleGPU);

		// テレイン VB/IB を使用（VS で Y を水位に固定）
		auto vbView = m_terrainSharedVB->View();
		auto ibView = m_terrainSharedIB->View();
		commandList->IASetVertexBuffers(0, 1, &vbView);
		commandList->IASetIndexBuffer(&ibView);
		// IB フォーマットに応じてインデックス数を計算
		UINT idxStride = (ibView.Format == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
		UINT idxCount = ibView.SizeInBytes / idxStride;
		commandList->DrawIndexedInstanced(idxCount, 1, 0, 0, 0);
		GPU_CMD_END_EVENT(commandList);
	}

	// ---- Ocean Pass: 地形メッシュの外側まで水を伸ばすための独立した巨大プレーン ----
	if (oceanPipelineState && oceanPipelineState->IsValid()
		&& s_oceanVB && s_oceanIB && terrainConstantBuffer[currentIndex] && s_terrainMaskHandle)
	{
		GPU_CMD_BEGIN_EVENT(commandList, 30, 80, 180, L"Ocean Pass");
		commandList->SetPipelineState(oceanPipelineState->Get());
		commandList->SetGraphicsRootSignature(terrainRootSignature->Get());
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commandList->SetGraphicsRootConstantBufferView(0, constantBuffer[currentIndex]->GetAddress());
		commandList->SetGraphicsRootConstantBufferView(1, terrainConstantBuffer[currentIndex]->GetAddress());
		commandList->SetGraphicsRootDescriptorTable(2, terrainMaskGPU);
		if (envHandle.ptr != 0)
			commandList->SetGraphicsRootDescriptorTable(3, envHandle);
		if (s_shadow && s_shadow->IsValid())
		{
			commandList->SetGraphicsRootDescriptorTable(5, s_shadow->GetShadowMapSrvGpu());
			commandList->SetGraphicsRootConstantBufferView(6, s_shadow->GetShadowCBAddress());
		}
		if (s_terrainExtraMaskHandle)
			commandList->SetGraphicsRootDescriptorTable(7, s_terrainExtraMaskHandle->HandleGPU);
		auto oVb = s_oceanVB->View();
		auto oIb = s_oceanIB->View();
		commandList->IASetVertexBuffers(0, 1, &oVb);
		commandList->IASetIndexBuffer(&oIb);
		commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);
		GPU_CMD_END_EVENT(commandList);
	}

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

	// ---- VSM本体: ページ要求→割当→ページ描画（V5a: 静止時は保持アトラス再利用でスキップ）----
	// ON/OFF は ImGui/Debug UI（s_vsmEnabled）。既定値は環境変数 DX12_VSM から初期化（1回のみ）。
	if (!s_vsmGateInit)
	{
		char ev[8];
		// VSM を既定ON（常用の主軸影）。DX12_VSM=0 で明示的に無効化＝従来CSMに戻す。
		{ DWORD n = GetEnvironmentVariableA("DX12_VSM", ev, sizeof(ev)); s_vsmEnabled = (n == 0) || (ev[0] != '0'); }
		s_vsmAtlasDebug  = GetEnvironmentVariableA("DX12_VSM_ATLAS",  ev, sizeof(ev)) > 0;
		s_vsmShadowDebug = GetEnvironmentVariableA("DX12_VSM_SHADOW", ev, sizeof(ev)) > 0;
		s_vsmForceRender = GetEnvironmentVariableA("DX12_VSM_NOCACHE", ev, sizeof(ev)) > 0;
		// 永続キャッシュは既定ON（正しく高速な経路。移動で全再描画=重い の回避）。
		// DX12_VSM_CACHE=0 で明示的に無効化可（非キャッシュ経路の比較/診断用）。
		if (GetEnvironmentVariableA("DX12_VSM_CACHE", ev, sizeof(ev)) > 0)
			s_vsmCache = (ev[0] != '0');
		else
			s_vsmCache = true;
		s_vsmGateInit = true;
	}
	if (s_vsm && s_vsm->IsValid())
		s_vsm->SetCacheMode(s_vsmCache);   // 永続キャッシュ mode（初回/切替時に内部でリセット予約）
	if (s_vsm && s_vsm->IsValid() && s_vsmEnabled)
	{
		// V5a: カメラ/太陽が動いたフレーム、または未準備(有効化直後)のみ再描画（描画キャッシュ）。
		if (s_vsm->NeedsRender() || !s_vsmAtlasReady || s_vsmForceRender)
		{
			s_vsm->BeginRenderStates(postCommandList);    // V4: atlas/pageTable → working
			s_vsm->MarkPages(postCommandList, g_Engine->GetDepthStencilResource());
			s_vsm->Allocate(postCommandList);
			s_vsm->BuildPageParams(postCommandList);
			s_vsm->BuildCasterBinning(postCommandList);   // V3c-m2: (caster,page) ペアリスト構築
			if (!s_vsmRenderBatches.empty())              // V3c-m3: 各ページへキャスタ深度を描画
				s_vsm->RenderPages(postCommandList, s_vsmRenderBatches.data(), (uint32_t)s_vsmRenderBatches.size());
			s_vsm->EndRenderStates(postCommandList);      // V4: → PIXEL_SHADER_RESOURCE(resting)。町がサンプル可
			s_vsmAtlasReady = true;
		}
		// 可視化/サンプルは毎フレーム（保持アトラスを参照＝V5キャッシュのアーキテクチャ）
		if (s_vsmAtlasDebug)
			s_vsm->RenderAtlasDebug(postCommandList, g_Engine->GetHdrRtvCpuHandle());
		if (s_vsmShadowDebug)
			s_vsm->RenderShadowDebug(postCommandList, g_Engine->GetHdrRtvCpuHandle(), g_Engine->GetDepthStencilResource());
	}

	// ---- DXR-GI F1 検証: primary ray のヒット距離 vs ラスタ深度を色分け（DX12_GI_DEBUG）----
	// 緑=一致(TLAS正), 赤=不一致, 青=RT取りこぼし。TLAS＋トランスフォームの正しさを確認する。
	if (s_giEnabled && s_giDebugView && s_rtManager && s_rtManager->IsValid())
	{
		{
			float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
			XMVECTOR det;
			XMMATRIX invVP = XMMatrixInverse(&det, g_Camera->GetViewMatrix() * g_Camera->GetProjectionMatrix(aspect));
			XMFLOAT3 cam; XMStoreFloat3(&cam, g_Camera->GetPosition());
			s_rtManager->RenderDebugPrimary(postCommandList, g_Engine->GetHdrRtvCpuHandle(),
				g_Engine->GetDepthStencilResource(), invVP, cam);
		}
	}

	// ---- Phase G: DDGI プローブ更新（postCL。町は前フレームの完了バッファを参照＝1フレーム遅延はEMAで不可視）----
	if (s_ddgiEnabled && s_ddgi && s_ddgi->IsValid() && s_rtManager && s_rtManager->IsValid()
		&& s_rtManager->GetInstanceCount() > 0 && s_envCubemapHandle && descriptorHeap && s_town)
	{
		// G-b: 実太陽（SceneConstants の方向 + atmosphere 色 × sunScale 3.0）でヒットの太陽バウンスを計算。
		XMFLOAT3 sunDir(0.0f, 1.0f, 0.0f);
		auto* dsc = sceneConstantBuffer[currentIndex] ? sceneConstantBuffer[currentIndex]->GetPtr<SceneConstants>() : nullptr;
		if (dsc) sunDir = XMFLOAT3(dsc->SunDirection.x, dsc->SunDirection.y, dsc->SunDirection.z);
		const float sunScale = 3.0f;
		XMFLOAT3 sunColorScaled(
			s_atmosphereParams.sunColorR * s_atmosphereParams.sunIntensity * sunScale,
			s_atmosphereParams.sunColorG * s_atmosphereParams.sunIntensity * sunScale,
			s_atmosphereParams.sunColorB * s_atmosphereParams.sunIntensity * sunScale);
		s_ddgi->Execute(postCommandList, descriptorHeap->GetHeap(), s_rtManager->GetTlasGpuVA(),
			s_envCubemapHandle->HandleGPU, s_town->GeometryInfoVA(), s_town->InstanceGeoBaseVA(),
			sunColorScaled, sunDir);
	}

	// ---- GI: AO を HDR に乗算適用（bloom/tonemap 前）----
	//  RTAO(レイトレース) が有効かつ TLAS があればそちらを使い、GTAO(スクリーン空間) は実行しない（二重遮蔽回避）。
	//  RTAO OFF 時は従来どおり GTAO を実行（DX12_NO_GTAO で無効化可）。
	{
		const bool rtaoActive = s_rtaoEnabled && s_rtao && s_rtao->IsValid()
			&& s_rtManager && s_rtManager->IsValid() && s_rtManager->GetInstanceCount() > 0;

		float aoAspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
		XMVECTOR aoDet;
		XMMATRIX aoInvVP = XMMatrixInverse(&aoDet,
			g_Camera->GetViewMatrix() * g_Camera->GetProjectionMatrix(aoAspect));
		XMFLOAT3 aoCam; XMStoreFloat3(&aoCam, g_Camera->GetPosition());

		if (rtaoActive)
		{
			RtaoSystem::Params rp;
			s_rtao->Execute(postCommandList, g_Engine->GetDepthStencilResource(),
				g_Engine->GetHdrRtvCpuHandle(), s_rtManager->GetTlasGpuVA(), aoInvVP, aoCam, rp);
		}
		else
		{
			char gtaoEnv[8];
			bool gtaoOff = GetEnvironmentVariableA("DX12_NO_GTAO", gtaoEnv, sizeof(gtaoEnv)) > 0;
			if (!gtaoOff && s_gtao && s_gtao->IsValid())
			{
				GtaoSystem::Params gp;
				s_gtao->Execute(postCommandList, g_Engine->GetDepthStencilResource(),
					g_Engine->GetHdrRtvCpuHandle(), aoInvVP, aoCam, gp);
			}
		}
	}

	// ---- 仕上げ: レイトレース反射（RTR）。SSR の反射ソースを生成（SSR より前に実行）----
	if (s_rtrEnabled && RtrAvailable() && g_Camera)
	{
		float rAspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
		XMVECTOR rDet;
		XMMATRIX rInvVP = XMMatrixInverse(&rDet, g_Camera->GetViewMatrix() * g_Camera->GetProjectionMatrix(rAspect));
		XMFLOAT3 rCam; XMStoreFloat3(&rCam, g_Camera->GetPosition());
		XMFLOAT3 rSunDir(0.0f, 1.0f, 0.0f);
		auto* rsc = sceneConstantBuffer[currentIndex] ? sceneConstantBuffer[currentIndex]->GetPtr<SceneConstants>() : nullptr;
		if (rsc) rSunDir = XMFLOAT3(rsc->SunDirection.x, rsc->SunDirection.y, rsc->SunDirection.z);
		const float rSunScale = 3.0f;
		XMFLOAT3 rSunCol(
			s_atmosphereParams.sunColorR * s_atmosphereParams.sunIntensity * rSunScale,
			s_atmosphereParams.sunColorG * s_atmosphereParams.sunIntensity * rSunScale,
			s_atmosphereParams.sunColorB * s_atmosphereParams.sunIntensity * rSunScale);
		float rGiInt = s_ddgi ? s_ddgi->GetIntensity() : 1.0f;
		s_rtr->Execute(postCommandList, g_Engine->GetDepthStencilResource(),
			s_rtManager->GetTlasGpuVA(), s_envCubemapHandle->HandleGPU,
			s_town->GeometryInfoVA(), s_town->InstanceGeoBaseVA(),
			s_ddgi ? s_ddgi->GetCbVA() : 0, s_ddgi ? s_ddgi->GetReadBufferVA() : 0,
			(s_ddgiEnabled && s_ddgi && s_ddgi->IsReady()),
			rInvVP, rCam, rSunCol, rSunDir, rGiInt);
	}

	// ---- SSR 濡れた水たまり反射（Atmosphere 前 = 反射の上に霧が乗る）----
	{ char e[8]; if (GetEnvironmentVariableA("DX12_NO_PUDDLE", e, sizeof(e)) > 0) goto skipPuddle; }
	if (s_ssr && s_ssr->IsValid() && s_town)
	{
		D3D12_GPU_VIRTUAL_ADDRESS sceneCbAddr =
			sceneConstantBuffer[currentIndex] ? sceneConstantBuffer[currentIndex]->GetAddress() : 0;
		if (sceneCbAddr)
		{
			SsrSystem::PuddleParams pp;
			pp.enabled = true;
			// 水たまり領域（world, m）。地面基準点として横断歩道（路面レベル）を使い、
			// その付近の地面のみ濡らす（高さゲートで屋根等を除外）。位置/大きさはここで調整。
			XMFLOAT3 cw;
			if (s_town->FirstCrosswalkWorld(cw))
			{
				pp.center = XMFLOAT2(cw.x, cw.z);
				pp.groundY = cw.y;
			}
			else { pp.center = XMFLOAT2(0.0f, 0.0f); pp.groundY = 0.0f; }
			pp.half = XMFLOAT2(30.0f, 30.0f);  // 濡れ地面の範囲（広場全体＝UE5の雨上がり感）
			pp.edgeFalloff = 2.5f;
			pp.wetDarken = 0.45f;   // 濡れて暗く（反射のコントラストを出す。Fresnelで見下ろしは下地が透ける）
			// P2 を再利用: thickness=ノイズタイル(1/m), edgeFade=CheapContrast 量（UE5 Puddle_Blend_Constrast 相当）
			pp.stride = 0.0f; pp.steps = 0.0f; pp.thickness = 0.013f; pp.edgeFade = 1.4f; // タイル(1/m)/コントラスト
			pp.skyTint = XMFLOAT3(1.0f, 1.0f, 1.0f); pp.reflectivity = 1.0f;
			// UE5 実ブレンドノイズ（水たまり分布）。一度だけロードしてキャッシュ。
			// 512² 8bit にダウンサンプル済み（4096²16bit=170MB は VRAM を圧迫し激遅のため）
			static Texture2D* s_puddleNoise =
				Texture2D::Get(std::string("assets/town/Downtown_West/Textures/Blends/T_blend_noise_a_512.png"));
			ID3D12Resource* noiseRes = (s_puddleNoise && s_puddleNoise->IsValid()) ? s_puddleNoise->Resource() : nullptr;
			s_ssr->Execute(postCommandList,
				g_Engine->GetHdrColorResource(), g_Engine->GetHdrRtvCpuHandle(),
				g_Engine->GetDepthStencilResource(), s_prefilterCubemap.Get(),
				(s_rtrEnabled && s_rtr) ? s_rtr->GetReflectionResource()
					: (s_reflColor ? s_reflColor.Get() : s_prefilterCubemap.Get()),  // RTR時=RT反射, 既定=平面反射カラー
				noiseRes,                                                     // 水たまり分布ノイズ
				sceneCbAddr, pp);
		}
	}
	skipPuddle:;

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

		Profiler::GpuMarkAtmosphereBegin(postCommandList);
		s_atmosphere->Execute(postCommandList, materialHeap,
			s_hdrSrvHandle ? s_hdrSrvHandle->HandleGPU : D3D12_GPU_DESCRIPTOR_HANDLE{0},
			g_Engine->GetHdrRtvCpuHandle(),
			hdrRes, depthRes,
			s_shadow,
			invVP, camPos, sunDirF,
			XMFLOAT4(
				s_atmosphereParams.sunColorR * s_atmosphereParams.sunIntensity,
				s_atmosphereParams.sunColorG * s_atmosphereParams.sunIntensity,
				s_atmosphereParams.sunColorB * s_atmosphereParams.sunIntensity,
				1.0f),
			g_Engine->CurrentBackBufferIndex(),
			s_atmosphereParams);
		Profiler::GpuMarkAtmosphereEnd(postCommandList);
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
	Profiler::GpuMarkPostProcessEndAndResolve(postCommandList);
	// 全スタンプの Resolve（未書き込みスタンプはプレースホルダーで補完）
	Profiler::GpuResolveAllStamps(postCommandList);
}

// 拡張テレインマスクの GPU ハンドル公開 (RenderSystem 等から参照)
D3D12_GPU_DESCRIPTOR_HANDLE Scene_GetTerrainExtraMaskGpu()
{
	return s_terrainExtraMaskGpuPub;
}
