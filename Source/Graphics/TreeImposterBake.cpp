#include "TreeImposterBake.h"
#include "DebugLog.h"
#include "SharedStruct.h"
#include "DescriptorHeap.h"
#include "Engine.h"
#include "ModelBounds.h"
#include "PipelineState.h"
#include "RootSignature.h"
#include "TreeVegetation.h"
#include "VertexBuffer.h"
#include "IndexBuffer.h"



#include <cstring>

#include <d3dx12.h>



using namespace DirectX;



namespace

{

	const UINT kAtlasW = 2048u;

	const UINT kAtlasH = 256u;

	const UINT kSlice = 256u;

	const UINT kSceneSlots = 24u; // 3 species * 8 dirs

}



bool TreeImposterBake::CreateQuadMeshes(VertexBuffer** outVb, IndexBuffer** outIb)

{

	if (!outVb || !outIb)

		return false;

	Vertex verts[4];

	std::memset(verts, 0, sizeof(verts));

	verts[0].Position = { -0.5f, -0.5f, 0.f };

	verts[1].Position = { 0.5f, -0.5f, 0.f };

	verts[2].Position = { 0.5f, 0.5f, 0.f };

	verts[3].Position = { -0.5f, 0.5f, 0.f };

	verts[0].UV = { 0.f, 1.f };

	verts[1].UV = { 1.f, 1.f };

	verts[2].UV = { 1.f, 0.f };

	verts[3].UV = { 0.f, 0.f };

	for (int i = 0; i < 4; ++i)

	{

		verts[i].Normal = { 0.f, 0.f, 1.f };

		verts[i].Tangent = { 1.f, 0.f, 0.f };

		verts[i].Color = { 1.f, 1.f, 1.f, 1.f };

		verts[i].BoneIndex[0] = 0;

		verts[i].BoneWeight[0] = 1.f;

	}

	uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };

	*outVb = new VertexBuffer(sizeof(verts), sizeof(Vertex), verts);

	*outIb = new IndexBuffer(sizeof(idx), idx);

	return *outVb && *outIb && (*outVb)->IsValid() && (*outIb)->IsValid();

}



bool TreeImposterBake::BakeAtlases(

	DescriptorHeap* heap,

	ID3D12RootSignature* rootSig,

	D3D12_GPU_VIRTUAL_ADDRESS materialCbGpu,

	D3D12_GPU_DESCRIPTOR_HANDLE iblTable,

	VertexBuffer* meshVB,

	IndexBuffer* meshIB,

	uint32_t meshIndexCount,

	const ModelBounds& meshLocalBounds,

	const TreeSpeciesMaterials* sm0,

	const TreeSpeciesMaterials* sm1,

	const TreeSpeciesMaterials* sm2,

	ComPtr<ID3D12Resource>& outAtlas0,

	ComPtr<ID3D12Resource>& outAtlas1,

	ComPtr<ID3D12Resource>& outAtlas2,

	DescriptorHandle* outMatTableStart[3])

{

	const TreeSpeciesMaterials* speciesSm[3] = { sm0, sm1, sm2 };

	ComPtr<ID3D12Resource>* const atlasPtr[3] = { &outAtlas0, &outAtlas1, &outAtlas2 };

	if (!g_Engine || !heap || !rootSig || !meshVB || !meshIB || meshIndexCount == 0)

		return false;



	ID3D12Device* device = g_Engine->Device();

	auto* cmd = g_Engine->MainGraphicsCmdList();

	auto* alloc = g_Engine->Allocator(0);



	if (!device || !cmd || !alloc)

		return false;



	XMFLOAT3 cmin = meshLocalBounds.Min;

	XMFLOAT3 cmax = meshLocalBounds.Max;

	// ランタイム FootLocal（GetImposterBillboardParams）と同じ：足元の真下を原点に
	const float footX = 0.5f * (cmin.x + cmax.x);
	const float footZ = 0.5f * (cmin.z + cmax.z);

	XMMATRIX worldBake = XMMatrixTranslation(-footX, -cmin.y, -footZ);



	InstanceData inst{};

	XMStoreFloat4x4(&inst.World, XMMatrixTranspose(worldBake));

	inst.NprPerMesh = XMFLOAT4(-1.f, 0.f, 0.f, 0.f);



	ComPtr<ID3D12Resource> instUpload;

	{

		CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);

		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(InstanceData));

		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bufDesc,

			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(instUpload.ReleaseAndGetAddressOf()))))

			return false;

		void* p = nullptr;

		if (FAILED(instUpload->Map(0, nullptr, &p)) || !p)

			return false;

		std::memcpy(p, &inst, sizeof(inst));

		instUpload->Unmap(0, nullptr);

	}



	const UINT sceneUploadBytes = sizeof(SceneConstants) * kSceneSlots;

	ComPtr<ID3D12Resource> sceneUpload;

	{

		CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);

		auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(sceneUploadBytes);

		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bufDesc,

			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(sceneUpload.ReleaseAndGetAddressOf()))))

			return false;

	}



	const float aspect = 1.0f;

	const float fovY = XM_PIDIV4;

	const float radius = 0.5f * (cmax.x - cmin.x + cmax.z - cmin.z) + 4.0f;

	const float treeH = cmax.y - cmin.y;
	const float midY = 0.5f * treeH;



	const XMMATRIX proj = XMMatrixPerspectiveFovLH(fovY, aspect, 0.1f, radius * 10.0f);



	void* pScene = nullptr;

	if (FAILED(sceneUpload->Map(0, nullptr, &pScene)) || !pScene)

		return false;

	for (UINT si = 0; si < 3u; ++si)

	{

		for (UINT i = 0; i < 8u; ++i)

		{

			const float ang = (2.0f * XM_PI * static_cast<float>(i)) / 8.0f;

			const float eyeX = cosf(ang) * radius * 1.2f;

			const float eyeZ = sinf(ang) * radius * 1.2f;

			const XMVECTOR eye = XMVectorSet(eyeX, midY, eyeZ, 1.f);

			const XMVECTOR at = XMVectorSet(0.f, midY, 0.f, 1.f);

			const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);

			const XMMATRIX view = XMMatrixLookAtLH(eye, at, up);



			SceneConstants sc{};

			sc.View = XMMatrixTranspose(view);

			sc.Proj = XMMatrixTranspose(proj);

			XMStoreFloat4(&sc.CameraWorld, eye);

			sc.CameraWorld.w = 1.f;

			const size_t slot = static_cast<size_t>(si) * 8u + static_cast<size_t>(i);

			std::memcpy(static_cast<char*>(pScene) + slot * sizeof(SceneConstants), &sc, sizeof(SceneConstants));

		}

	}

	sceneUpload->Unmap(0, nullptr);



	ComPtr<ID3D12Resource> depthTex;

	{

		CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);

		auto dsDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, kAtlasW, kAtlasH, 1, 0, 1, 0,

			D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

		D3D12_CLEAR_VALUE clearValue = {};

		clearValue.Format = DXGI_FORMAT_D32_FLOAT;

		clearValue.DepthStencil.Depth = 1.0f;

		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dsDesc,

			D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(depthTex.ReleaseAndGetAddressOf()))))

			return false;

	}



	const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	const UINT dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);



	ComPtr<ID3D12DescriptorHeap> rtvHeap;

	{

		D3D12_DESCRIPTOR_HEAP_DESC hd = {};

		hd.NumDescriptors = 3;

		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(rtvHeap.ReleaseAndGetAddressOf()))))

			return false;

	}

	ComPtr<ID3D12DescriptorHeap> dsvHeap;

	{

		D3D12_DESCRIPTOR_HEAP_DESC hd = {};

		hd.NumDescriptors = 1;

		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(dsvHeap.ReleaseAndGetAddressOf()))))

			return false;

	}



	D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu = dsvHeap->GetCPUDescriptorHandleForHeapStart();

	device->CreateDepthStencilView(depthTex.Get(), nullptr, dsvCpu);



	for (int si = 0; si < 3; ++si)

	{

		CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_DEFAULT);

		D3D12_CLEAR_VALUE clearValue = {};

		clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

		clearValue.Color[0] = clearValue.Color[1] = clearValue.Color[2] = 0.15f;

		clearValue.Color[3] = 0.0f;

		auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, kAtlasW, kAtlasH, 1, 1, 1, 0,

			D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

		atlasPtr[si]->Reset();

		if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,

			D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue, IID_PPV_ARGS(atlasPtr[si]->ReleaseAndGetAddressOf()))))

			return false;



		D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = rtvHeap->GetCPUDescriptorHandleForHeapStart();

		rtvCpu.ptr += static_cast<SIZE_T>(si) * rtvSize;

		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};

		rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

		device->CreateRenderTargetView(atlasPtr[si]->Get(), &rtvDesc, rtvCpu);

	}



	PipelineState bakePso;

	bakePso.SetInputLayout(Vertex::InputLayout);

	bakePso.SetRootSignature(rootSig);

	bakePso.SetVS(L"SimpleVS.cso");

	bakePso.SetPS(L"BakeTreeLOD0_PS.cso");

	bakePso.SetRenderTargetFormat(DXGI_FORMAT_R8G8B8A8_UNORM);

	bakePso.SetCullMode(D3D12_CULL_MODE_NONE);

	bakePso.Create();

	if (!bakePso.IsValid())

	{

		DebugLog("[TreeImposterBake] Bake PSO failed\n");

		return false;

	}



	alloc->Reset();

	cmd->Reset(alloc, bakePso.Get());



	ID3D12DescriptorHeap* heaps[] = { heap->GetHeap() };

	cmd->SetDescriptorHeaps(1, heaps);



	for (int si = 0; si < 3; ++si)

	{

		const TreeSpeciesMaterials* sm = speciesSm[si];

		if (!sm || !sm->matLod0 || sm->matLod0->HandleGPU.ptr == 0)

		{

			DebugLog("[TreeImposterBake] species %d missing matLod0, skip bake\n", si);

			continue;

		}



		D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu = rtvHeap->GetCPUDescriptorHandleForHeapStart();

		rtvCpu.ptr += static_cast<SIZE_T>(si) * rtvSize;



		for (UINT slice = 0; slice < 8u; ++slice)

		{

			const UINT slot = static_cast<UINT>(si) * 8u + slice;

			D3D12_VIEWPORT vp = {};

			vp.Width = static_cast<float>(kSlice);

			vp.Height = static_cast<float>(kSlice);

			vp.MinDepth = 0.f;

			vp.MaxDepth = 1.f;

			vp.TopLeftX = static_cast<float>(slice * kSlice);

			vp.TopLeftY = 0.f;

			D3D12_RECT sc = {};

			sc.left = static_cast<LONG>(slice * kSlice);

			sc.top = 0;

			sc.right = static_cast<LONG>((slice + 1) * kSlice);

			sc.bottom = static_cast<LONG>(kSlice);



			cmd->RSSetViewports(1, &vp);

			cmd->RSSetScissorRects(1, &sc);



			const float clearCol[4] = { 0.15f, 0.12f, 0.1f, 0.f };

			cmd->OMSetRenderTargets(1, &rtvCpu, FALSE, &dsvCpu);

			cmd->ClearRenderTargetView(rtvCpu, clearCol, 0, nullptr);

			cmd->ClearDepthStencilView(dsvCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);



			cmd->SetPipelineState(bakePso.Get());

			cmd->SetGraphicsRootSignature(rootSig);



			const D3D12_GPU_VIRTUAL_ADDRESS sceneGpu = sceneUpload->GetGPUVirtualAddress() + static_cast<UINT64>(slot) * sizeof(SceneConstants);

			cmd->SetGraphicsRootConstantBufferView(0, sceneGpu);

			cmd->SetGraphicsRootConstantBufferView(1, materialCbGpu);

			cmd->SetGraphicsRootShaderResourceView(2, instUpload->GetGPUVirtualAddress());

			if (iblTable.ptr != 0)

				cmd->SetGraphicsRootDescriptorTable(4, iblTable);

			cmd->SetGraphicsRootShaderResourceView(5, 0);

			const UINT treeVisConstants[4] = { 0, 0, 0, 0 };

			cmd->SetGraphicsRoot32BitConstants(6, 4, treeVisConstants, 0);



			cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

			// LOD0: TreeVegetation のマージ VB/IB を matLod0 で焼く（.tmesh / 手続きメッシュ）。

			if (meshVB && meshIB && meshIndexCount > 0 && sm->matLod0 && sm->matLod0->HandleGPU.ptr != 0)

			{

				cmd->SetGraphicsRootDescriptorTable(3, sm->matLod0->HandleGPU);

				D3D12_VERTEX_BUFFER_VIEW vbView = meshVB->View();

				D3D12_INDEX_BUFFER_VIEW ibView = meshIB->View();

				cmd->IASetVertexBuffers(0, 1, &vbView);

				cmd->IASetIndexBuffer(&ibView);

				cmd->DrawIndexedInstanced(meshIndexCount, 1, 0, 0, 0);

			}

		}

	}



	// Barriers: atlas -> SRV, depth -> common

	for (int si = 0; si < 3; ++si)

	{

		CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(

			atlasPtr[si]->Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		cmd->ResourceBarrier(1, &b);

	}

	// ExecuteAndWait() が MainGraphicsCmdList を Close するのでここでは Close しない（二重 Close → COMMAND_LIST_CLOSED）

	g_Engine->ExecuteAndWait();



	// SRV 登録: 6 連続（PBR テーブル t0..t5）

	for (int si = 0; si < 3; ++si)

	{

		if (!atlasPtr[si] || !atlasPtr[si]->Get())

		{

			outMatTableStart[si] = nullptr;

			continue;

		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;

		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		srvDesc.Texture2D.MipLevels = 1;

		srvDesc.Texture2D.MostDetailedMip = 0;

		DescriptorHandle* first = heap->RegisterResource(atlasPtr[si]->Get(), srvDesc);

		if (!first)

			return false;

		outMatTableStart[si] = first;

		for (int k = 0; k < 5; ++k)

		{

			DescriptorHandle* dup = heap->RegisterResource(atlasPtr[si]->Get(), srvDesc);

			if (!dup)

				return false;

		}

	}



	DebugLog("[TreeImposterBake] BakeAtlases OK (3 species x 8 dirs)\n");

	return true;

}

