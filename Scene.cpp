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

#include <filesystem>
#include <vector>
#include <cstdio>

using namespace DirectX;
namespace fs = std::filesystem;

// Replace extension
std::wstring ReplaceExtension(const std::wstring& origin, const char* ext)
{
	fs::path p = origin.c_str();
	return p.replace_extension(ext).c_str();
}

// --- [Global/Static Objects] ---
DescriptorHeap* descriptorHeap;
std::vector<DescriptorHandle*> materialHandles;

Scene* g_Scene;
ConstantBuffer* constantBuffer[Engine::FRAME_BUFFER_COUNT];
ConstantBuffer* pbrPropertyBuffer[Engine::FRAME_BUFFER_COUNT];

RootSignature* rootSignature;
PipelineState* pipelineState;
Camera* g_Camera;

// Base transform from loader (XMFLOAT4X4)
DirectX::XMFLOAT4X4 g_ModelBaseTransform;

std::vector<Mesh> meshes;
std::vector<VertexBuffer*> vertexBuffers;
std::vector<IndexBuffer*> indexBuffers;

namespace {
	SkyboxRenderer* s_skyboxRenderer = nullptr;
	ComPtr<ID3D12Resource> skyboxCubemap;       // スカイボックス用 + PBR環境光（IBL）用
	ComPtr<ID3D12Resource> skyboxEquirect;     // スカイボックスは Equirect 2D を直接サンプル
	PostProcessSystem* s_postProcess = nullptr;
	DescriptorHandle* s_hdrSrvHandle = nullptr;
	DescriptorHandle* s_envCubemapHandle = nullptr; // PBR用環境キューブマップSRV
	PostProcessSettings s_postProcessSettings;
}

const wchar_t* modelFile = L"assets\\sakura1\\sakura1.fbx";
float rotateY = 0.0f;
const float modelScale = 1.0f;

bool Scene::Init()
{
	// Load model via Assimp
	ImportSettings importSetting(modelFile, meshes, false, true, modelScale);
	importSetting.outClips = nullptr;

	AssimpLoader loader;
	if (!loader.Load(importSetting)) return false;

	g_ModelBaseTransform = importSetting.outBaseTransform;

	descriptorHeap = new DescriptorHeap();
	if (!descriptorHeap || !descriptorHeap->GetHeap()) return false;

	vertexBuffers.reserve(meshes.size());
	indexBuffers.reserve(meshes.size());
	for (size_t i = 0; i < meshes.size(); i++)
	{
		if (meshes[i].Vertices.empty() || meshes[i].Indices.empty()) continue;
		auto* pVB = new VertexBuffer(sizeof(Vertex) * meshes[i].Vertices.size(), sizeof(Vertex), meshes[i].Vertices.data());
		auto* pIB = new IndexBuffer(sizeof(uint32_t) * meshes[i].Indices.size(), meshes[i].Indices.data());
		if (!pVB->IsValid() || !pIB->IsValid()) return false;
		vertexBuffers.push_back(pVB);
		indexBuffers.push_back(pIB);
	}

	g_Camera = new Camera();
	g_Camera->SetPosition(XMVectorSet(0.0f, 1.2f, 2.5f, 0.0f));

	for (size_t i = 0; i < Engine::FRAME_BUFFER_COUNT; i++)
	{
		constantBuffer[i] = new ConstantBuffer(sizeof(Transform));
		if (!constantBuffer[i]->IsValid()) return false;

		pbrPropertyBuffer[i] = new ConstantBuffer(sizeof(XMFLOAT4));
		if (!pbrPropertyBuffer[i]->IsValid()) return false;
		auto pbr = pbrPropertyBuffer[i]->GetPtr<XMFLOAT4>();
		*pbr = XMFLOAT4(1.0f, 1.5f, 0.0f, 0.0f);
	}

	materialHandles.clear();
	for (size_t i = 0; i < meshes.size(); ++i)
	{
		Texture2D* albedoTex = Texture2D::Get(meshes[i].DiffuseMap);
		if (!albedoTex && !meshes[i].DiffuseMap.empty())
			albedoTex = Texture2D::Get(ReplaceExtension(meshes[i].DiffuseMap, "tga"));
		if (!albedoTex) albedoTex = Texture2D::GetWhite();
		DescriptorHandle* firstHandle = descriptorHeap->Register(albedoTex);
		materialHandles.push_back(firstHandle);

		Texture2D* normalTex = Texture2D::Get(meshes[i].NormalMap);
		if (!normalTex && !meshes[i].NormalMap.empty())
			normalTex = Texture2D::Get(ReplaceExtension(meshes[i].NormalMap, "tga"));
		if (!normalTex) normalTex = Texture2D::GetWhite();
		descriptorHeap->Register(normalTex);

		Texture2D* metallicTex = Texture2D::Get(meshes[i].MetallicMap);
		if (!metallicTex && !meshes[i].MetallicMap.empty())
			metallicTex = Texture2D::Get(ReplaceExtension(meshes[i].MetallicMap, "tga"));
		if (!metallicTex) metallicTex = Texture2D::GetWhite();
		descriptorHeap->Register(metallicTex);

		Texture2D* roughnessTex = Texture2D::Get(meshes[i].RoughnessMap);
		if (!roughnessTex && !meshes[i].RoughnessMap.empty())
			roughnessTex = Texture2D::Get(ReplaceExtension(meshes[i].RoughnessMap, "tga"));
		if (!roughnessTex) roughnessTex = Texture2D::GetWhite();
		descriptorHeap->Register(roughnessTex);
	}

	rootSignature = new RootSignature();
	pipelineState = new PipelineState();
	pipelineState->SetInputLayout(Vertex::InputLayout);
	pipelineState->SetRootSignature(rootSignature->Get());
	pipelineState->SetVS(L"SimpleVS.cso");
	pipelineState->SetPS(L"StandardPBR_PS.cso");
	pipelineState->Create();

	if (!pipelineState->IsValid()) return false;

	// --- Skybox Initialization ---
	{
		DebugLog("[Skybox] --- init begin ---\n");
		g_Engine->Allocator(0)->Reset();
		g_Engine->CommandList()->Reset(g_Engine->Allocator(0), nullptr);

		ID3D12Resource* cubemap = nullptr;
		ID3D12Resource* equirect = nullptr;
		IBLGenerator ibl;
		auto doExecuteAndWait = []() { g_Engine->ExecuteAndWait(); };

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

		// PBR用: 環境光（IBL）に使うキューブマップをSRV登録（空の光をオブジェクトに反映）
		if (skyboxCubemap.Get())
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
			srvDesc.TextureCube.MipLevels = 1;
			s_envCubemapHandle = descriptorHeap->RegisterResource(skyboxCubemap.Get(), srvDesc);
		}

		// スカイボックスは Equirect 2D を直接サンプル（面のつなぎ目がなくなる）
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
	}

	// HDR 用 SRV 登録と PostProcessSystem
	{
		s_postProcessSettings.exposure = 1.0f;
		s_postProcessSettings.gamma = 2.2f;
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
		if (s_postProcess && !s_postProcess->Init(g_Engine->Device()))
		{
			delete s_postProcess;
			s_postProcess = nullptr;
		}
	}

	return true;
}

void Scene::Update()
{
	float dt = 0.016f;
	g_Camera->Update(dt);

	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto currentTransform = constantBuffer[currentIndex]->GetPtr<Transform>();
	if (!currentTransform) return;

	float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);

	XMMATRIX baseTransform = XMLoadFloat4x4(&g_ModelBaseTransform);
	currentTransform->World = XMMatrixTranspose(
		XMMatrixScaling(modelScale, modelScale, modelScale) *
		baseTransform * XMMatrixRotationY(rotateY)
	);

	currentTransform->View = XMMatrixTranspose(g_Camera->GetViewMatrix());
	currentTransform->Proj = XMMatrixTranspose(g_Camera->GetProjectionMatrix(aspect));
}

void Scene::Draw()
{
	auto commandList = g_Engine->CommandList();

	// ==========================================
	// [Pass 1] HDRバッファへのシーン描画 — D3D12 は「1.状態を変える → 2.セットする → 3.処理する」の順を厳守
	// ==========================================

	// 【1番目】HDR を「読み取り専用(SRV)」から「書き込み可能(RTV)」へ遷移（バリアの記述漏れでクラッシュする典型）
	EngineDoTransition(commandList, g_Engine->GetHdrColorResource(),
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		D3D12_RESOURCE_STATE_RENDER_TARGET);

	// 【2番目】描画先を HDR RTV + DSV にセット
	D3D12_CPU_DESCRIPTOR_HANDLE hdrRtvHandle = g_Engine->GetHdrRtvCpuHandle();
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_Engine->GetDsvCpuHandle();
	commandList->OMSetRenderTargets(1, &hdrRtvHandle, FALSE, &dsvHandle);

	// 【3番目】クリア（バリアの後に実行しないと INVALID_SUBRESOURCE_STATE）
	const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
	commandList->ClearRenderTargetView(hdrRtvHandle, clearColor, 0, nullptr);
	commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// --- メッシュ・スカイボックス描画 ---
	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto materialHeap = descriptorHeap->GetHeap();
	commandList->SetDescriptorHeaps(1, &materialHeap);
	commandList->SetGraphicsRootSignature(rootSignature->Get());
	commandList->SetPipelineState(pipelineState->Get());

	for (size_t i = 0; i < meshes.size(); i++)
	{
		auto vbView = vertexBuffers[i]->View();
		auto ibView = indexBuffers[i]->View();

		commandList->SetGraphicsRootConstantBufferView(0, constantBuffer[currentIndex]->GetAddress());
		commandList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer[currentIndex]->GetAddress());
		commandList->SetGraphicsRootDescriptorTable(2, materialHandles[i]->HandleGPU);
		// 環境マップ（空の光）は全メッシュ共通で root param 3
		if (s_envCubemapHandle)
			commandList->SetGraphicsRootDescriptorTable(3, s_envCubemapHandle->HandleGPU);

		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commandList->IASetVertexBuffers(0, 1, &vbView);
		commandList->IASetIndexBuffer(&ibView);

		commandList->DrawIndexedInstanced(static_cast<UINT>(meshes[i].Indices.size()), 1, 0, 0, 0);
	}

	if (s_skyboxRenderer && s_skyboxRenderer->IsValid())
	{
		float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);
		s_skyboxRenderer->Draw(commandList, g_Camera->GetViewMatrix(), g_Camera->GetProjectionMatrix(aspect));
	}

	// ポストプロセス: HDR → トーンマップ → バックバッファ
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
		s_postProcess->Execute(commandList, s_hdrSrvHandle->HandleGPU, g_Engine->GetBackBufferRtvCpuHandle(), s_postProcessSettings);
	}
}
