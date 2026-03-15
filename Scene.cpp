#include "Scene.h"
#include "Engine.h"
#include "App.h"
#include <d3dx12.h>
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

#include <filesystem>
#include <vector>

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

const wchar_t* modelFile = L"assets\\sakura1\\sakura1.fbx";
//const wchar_t* modelFile = L"assets\\hibana\\hibana.pmx";
//const wchar_t* modelFile = L"assets\\Alicia\\FBX\\Alicia_solid_Unity.FBX";
float rotateY = 0.0f;
const float modelScale = 1.0f;

bool Scene::Init()
{
	// Load model via Assimp (meshes filled here)
	ImportSettings importSetting(modelFile, meshes, false, true, modelScale);
	importSetting.outClips = nullptr;

	// Single load call
	AssimpLoader loader;
	if (!loader.Load(importSetting)) return false;

	// --- Use base transform from loader ---
	// Store XMFLOAT4X4 from loader
	g_ModelBaseTransform = importSetting.outBaseTransform;

	descriptorHeap = new DescriptorHeap();
	if (!descriptorHeap || !descriptorHeap->GetHeap()) return false;

	// Vertex/index buffer creation
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

	// --- [Camera setup] ---
	g_Camera = new Camera();
	g_Camera->SetPosition(XMVectorSet(0.0f, 1.2f, 2.5f, 0.0f));

	for (size_t i = 0; i < Engine::FRAME_BUFFER_COUNT; i++)
	{
		constantBuffer[i] = new ConstantBuffer(sizeof(Transform));
		if (!constantBuffer[i]->IsValid()) return false;

		pbrPropertyBuffer[i] = new ConstantBuffer(sizeof(XMFLOAT4));
		if (!pbrPropertyBuffer[i]->IsValid()) return false;
		auto pbr = pbrPropertyBuffer[i]->GetPtr<XMFLOAT4>();
		*pbr = XMFLOAT4(1.0f, 1.5f, 0.0f, 0.0f); // RimParams.y = 1.5 (NormalScale)
	}

	// Material / texture registration (use paths from loader; .fbm subfolder is preserved)
	materialHandles.clear();
	for (size_t i = 0; i < meshes.size(); ++i)
	{
		Texture2D* albedoTex = Texture2D::Get(meshes[i].DiffuseMap);
		if (!albedoTex && !meshes[i].DiffuseMap.empty())
			albedoTex = Texture2D::Get(ReplaceExtension(meshes[i].DiffuseMap, "tga"));
		if (!albedoTex) albedoTex = Texture2D::GetWhite();
		DescriptorHandle* firstHandle = descriptorHeap->Register(albedoTex);
		materialHandles.push_back(firstHandle);

		Texture2D* nTex = meshes[i].NormalMap.empty() ? nullptr : Texture2D::Get(meshes[i].NormalMap);
		if (!nTex) nTex = Texture2D::Get(ReplaceExtension(meshes[i].DiffuseMap, "_n.tga"));
		Texture2D* mTex = meshes[i].MetallicMap.empty() ? nullptr : Texture2D::Get(meshes[i].MetallicMap);
		if (!mTex) mTex = Texture2D::Get(ReplaceExtension(meshes[i].DiffuseMap, "_m.tga"));
		Texture2D* rTex = meshes[i].RoughnessMap.empty() ? nullptr : Texture2D::Get(meshes[i].RoughnessMap);
		if (!rTex) rTex = Texture2D::Get(ReplaceExtension(meshes[i].DiffuseMap, "_r.tga"));
		if (!nTex) nTex = Texture2D::GetWhite();
		if (!mTex) mTex = Texture2D::GetWhite();
		if (!rTex) rTex = Texture2D::GetWhite();
		descriptorHeap->Register(nTex);
		descriptorHeap->Register(mTex);
		descriptorHeap->Register(rTex);
	}

	rootSignature = new RootSignature();
	pipelineState = new PipelineState();
	pipelineState->SetInputLayout(Vertex::InputLayout);
	pipelineState->SetRootSignature(rootSignature->Get());
	pipelineState->SetVS(L"SimpleVS.cso");
	pipelineState->SetPS(L"StandardPBR_PS.cso");
	pipelineState->Create();

	return pipelineState->IsValid();
}

void Scene::Update()
{
	// 1. Camera update
	float dt = 0.016f;
	g_Camera->Update(dt);

	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto currentTransform = constantBuffer[currentIndex]->GetPtr<Transform>();
	if (!currentTransform) return;

	float aspect = static_cast<float>(WINDOW_WIDTH) / static_cast<float>(WINDOW_HEIGHT);

	// World: model space [LH, +Y up] -> world space. HLSL uses row-vector: pos * World, so we pass transpose.
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
	auto currentIndex = g_Engine->CurrentBackBufferIndex();
	auto commandList = g_Engine->CommandList();
	auto materialHeap = descriptorHeap->GetHeap();

	commandList->SetGraphicsRootSignature(rootSignature->Get());
	commandList->SetPipelineState(pipelineState->Get());
	commandList->SetDescriptorHeaps(1, &materialHeap);

	for (size_t i = 0; i < meshes.size(); i++)
	{
		auto vbView = vertexBuffers[i]->View();
		auto ibView = indexBuffers[i]->View();

		commandList->SetGraphicsRootConstantBufferView(0, constantBuffer[currentIndex]->GetAddress());
		commandList->SetGraphicsRootConstantBufferView(1, pbrPropertyBuffer[currentIndex]->GetAddress());
		commandList->SetGraphicsRootDescriptorTable(2, materialHandles[i]->HandleGPU);

		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		commandList->IASetVertexBuffers(0, 1, &vbView);
		commandList->IASetIndexBuffer(&ibView);

		// size_t to UINT for DrawIndexedInstanced
		commandList->DrawIndexedInstanced(static_cast<UINT>(meshes[i].Indices.size()), 1, 0, 0, 0);
	}
}
