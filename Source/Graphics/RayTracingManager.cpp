#include "RayTracingManager.h"
#include "../../DescriptorHeap.h"
#include "../../Engine.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// Align to D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT
static constexpr uint32_t kShaderRecordAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
static uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

bool RayTracingManager::Init(ID3D12Device5* device, DescriptorHeap* heap,
	uint32_t screenW, uint32_t screenH)
{
	if (!device || !heap) return false;

	// Check DXR support
	const auto& features = g_Engine->GetFeatureSupport();
	if (!features.raytracingSupported)
	{
		printf("RayTracingManager: DXR not supported on this device.\n");
		return false;
	}

	m_screenW = screenW;
	m_screenH = screenH;

	if (!CreateReflectionResources(device, heap)) return false;
	if (!CreateRTPipeline(device)) return false;
	if (!CreateDebugPipeline(device))   // F1 検証ビュー（非致命: 失敗しても本体は動く）
		printf("RayTracingManager: debug PSO init failed (non-fatal)\n");

	// Constants CB
	auto uploadProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
	device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &cbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantsCB));
	D3D12_RANGE r{0,0};
	m_constantsCB->Map(0, &r, &m_constantsMapped);

	m_valid = true;
	printf("RayTracingManager::Init: OK (%ux%u reflection, half-res)\n", m_screenW/2, m_screenH/2);
	return true;
}

void RayTracingManager::Shutdown()
{
	if (m_constantsCB && m_constantsMapped) { m_constantsCB->Unmap(0, nullptr); m_constantsMapped = nullptr; }
	m_blasList.clear();
	m_tlasBuffer.Reset(); m_tlasScratch.Reset(); m_instanceDescBuffer.Reset();
	m_reflectionTexture.Reset();
	m_rtPso.Reset(); m_rtPsoProperties.Reset();
	m_shaderTable.Reset(); m_globalRootSig.Reset(); m_constantsCB.Reset();
	m_valid = false;
}

bool RayTracingManager::CreateReflectionResources(ID3D12Device5* device, DescriptorHeap* heap)
{
	uint32_t halfW = m_screenW / 2;
	uint32_t halfH = m_screenH / 2;

	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R16G16B16A16_FLOAT, halfW, halfH, 1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	auto hr = device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_reflectionTexture));
	if (FAILED(hr)) { printf("RT: reflection texture failed\n"); return false; }

	// SRV for water shader sampling
	m_reflectionSrvIdx = heap->AllocateIndex();
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	heap->CreateSRVAt(m_reflectionSrvIdx, m_reflectionTexture.Get(), srv);

	// UAV for RT output
	m_reflectionUavIdx = heap->AllocateIndex();
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	heap->CreateUAVAt(m_reflectionUavIdx, m_reflectionTexture.Get(), uav);

	return true;
}

bool RayTracingManager::CreateRTPipeline(ID3D12Device5* device)
{
	// Global root signature: CBV b0, SRV t0 (TLAS), UAV u0 (output), SRV t1 (depth)
	CD3DX12_ROOT_PARAMETER1 params[4]{};
	params[0].InitAsConstantBufferView(0);
	params[1].InitAsShaderResourceView(0); // TLAS
	CD3DX12_DESCRIPTOR_RANGE1 uavRange{};
	uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
	params[2].InitAsDescriptorTable(1, &uavRange);
	params[3].InitAsShaderResourceView(1); // depth buffer

	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
	rsDesc.Init_1_1(4, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

	ComPtr<ID3DBlob> rsBlob, rsErr;
	auto hr = D3DX12SerializeVersionedRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
		rsBlob.GetAddressOf(), rsErr.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
		IID_PPV_ARGS(m_globalRootSig.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;

	// RT PSO will be created when shaders are compiled (non-fatal if missing now)
	// The RT pipeline uses D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE
	// with RayGen, ClosestHit, Miss shaders + HitGroup

	printf("RayTracingManager: RT root signature created. PSO deferred until shaders compiled.\n");
	return true;
}

bool RayTracingManager::CreateDebugPipeline(ID3D12Device5* device)
{
	// scene-depth SRV 用の小ヒープ [0]
	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_debugHeap)))) return false;

	// root: b0 CBV, t0 TLAS(root SRV), t1 depth(table), s0 point clamp
	CD3DX12_DESCRIPTOR_RANGE1 depthRange{};
	depthRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);   // t1
	CD3DX12_ROOT_PARAMETER1 p[3]{};
	p[0].InitAsConstantBufferView(0);
	p[1].InitAsShaderResourceView(0);                        // t0 TLAS
	p[2].InitAsDescriptorTable(1, &depthRange, D3D12_SHADER_VISIBILITY_PIXEL);
	D3D12_STATIC_SAMPLER_DESC smp{};
	smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	smp.ShaderRegister = 0; smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rs;
	rs.Init_1_1(3, p, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
	ComPtr<ID3DBlob> blob, err;
	if (FAILED(D3DX12SerializeVersionedRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_1, &blob, &err))) return false;
	if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
		IID_PPV_ARGS(&m_debugRootSig)))) return false;

	ComPtr<ID3DBlob> vs, ps;
	if (FAILED(D3DReadFileToBlob(L"ToneMap_VS.cso", &vs)) &&
		FAILED(D3DReadFileToBlob(L"Shaders\\PostProcess\\ToneMap_VS.cso", &vs))) { printf("RT dbg: ToneMap_VS.cso missing\n"); return false; }
	if (FAILED(D3DReadFileToBlob(L"RtDebugPrimary_PS.cso", &ps)) &&
		FAILED(D3DReadFileToBlob(L"Shaders\\RT\\RtDebugPrimary_PS.cso", &ps))) { printf("RT dbg: RtDebugPrimary_PS.cso missing\n"); return false; }

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
	pd.pRootSignature = m_debugRootSig.Get();
	pd.VS = CD3DX12_SHADER_BYTECODE(vs.Get());
	pd.PS = CD3DX12_SHADER_BYTECODE(ps.Get());
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	pd.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	pd.DepthStencilState.DepthEnable = FALSE;
	pd.SampleMask = UINT_MAX;
	pd.NumRenderTargets = 1;
	pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
	pd.SampleDesc.Count = 1;
	if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_debugPso)))) { printf("RT dbg: PSO failed\n"); return false; }
	printf("RayTracingManager: debug primary-ray PSO ready.\n");
	return true;
}

void RayTracingManager::RenderDebugPrimary(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv,
	ID3D12Resource* depthResource, const XMMATRIX& invViewProj, const XMFLOAT3& cameraPos)
{
	if (!m_valid || !m_debugPso || !m_tlasBuffer || !depthResource) return;
	auto* device = g_Engine->Device();

	D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
	sd.Format = DXGI_FORMAT_R32_FLOAT;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(depthResource, &sd, m_debugHeap->GetCPUDescriptorHandleForHeapStart());

	if (m_constantsMapped)
	{
		auto* cb = static_cast<RTConstants*>(m_constantsMapped);
		cb->InvViewProj = XMMatrixTranspose(invViewProj);
		cb->CameraPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0);
	}

	auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	cmd->ResourceBarrier(1, &toSrv);

	cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, nullptr);
	D3D12_VIEWPORT vp{ 0.0f, 0.0f, (float)m_screenW, (float)m_screenH, 0.0f, 1.0f };
	D3D12_RECT scc{ 0, 0, (LONG)m_screenW, (LONG)m_screenH };
	cmd->RSSetViewports(1, &vp); cmd->RSSetScissorRects(1, &scc);
	ID3D12DescriptorHeap* heaps[] = { m_debugHeap.Get() };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetPipelineState(m_debugPso.Get());
	cmd->SetGraphicsRootSignature(m_debugRootSig.Get());
	cmd->SetGraphicsRootConstantBufferView(0, m_constantsCB->GetGPUVirtualAddress());
	cmd->SetGraphicsRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());
	cmd->SetGraphicsRootDescriptorTable(2, m_debugHeap->GetGPUDescriptorHandleForHeapStart());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->DrawInstanced(3, 1, 0, 0);

	auto back = CD3DX12_RESOURCE_BARRIER::Transition(depthResource,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	cmd->ResourceBarrier(1, &back);
}

uint32_t RayTracingManager::AddBLAS(ID3D12GraphicsCommandList4* cmd,
	const RTGeometry* geos, uint32_t geoCount)
{
	auto* device = g_Engine->Device();
	if (geoCount == 0) return UINT32_MAX;

	// One BLAS built from all submesh geometries of a model.
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs(geoCount);
	for (uint32_t g = 0; g < geoCount; ++g)
	{
		auto& gd = geomDescs[g]; gd = {};
		gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		gd.Triangles.VertexBuffer.StartAddress = geos[g].vertexBuffer->GetGPUVirtualAddress();
		gd.Triangles.VertexBuffer.StrideInBytes = geos[g].vertexStride;
		gd.Triangles.VertexCount = geos[g].vertexCount;
		gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;   // Position at offset 0
		gd.Triangles.IndexBuffer = geos[g].indexBuffer->GetGPUVirtualAddress();
		gd.Triangles.IndexCount = geos[g].indexCount;
		gd.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	}

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = geoCount;
	inputs.pGeometryDescs = geomDescs.data();
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
	device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

	BLASEntry entry{};
	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto blasDesc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ResultDataMaxSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &blasDesc,
		D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&entry.blasBuffer));

	// Scratch is TRANSIENT: held in m_buildScratch until the one-time build is GPU-idle, then freed
	// (old code stored scratch permanently in BLASEntry -> VRAM leak at hundreds of BLAS).
	ComPtr<ID3D12Resource> scratch;
	auto scratchDesc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ScratchDataSizeInBytes,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &scratchDesc,
		D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&scratch));

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
	buildDesc.Inputs = inputs;
	buildDesc.DestAccelerationStructureData = entry.blasBuffer->GetGPUVirtualAddress();
	buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
	cmd->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(entry.blasBuffer.Get());
	cmd->ResourceBarrier(1, &uavBarrier);

	uint32_t idx = static_cast<uint32_t>(m_blasList.size());
	m_blasList.push_back(std::move(entry));
	m_buildScratch.push_back(std::move(scratch));
	return idx;
}

void RayTracingManager::BuildTLAS(ID3D12GraphicsCommandList4* cmd,
	const RTInstance* instances, uint32_t instanceCount)
{
	if (instanceCount == 0) return;
	auto* device = g_Engine->Device();

	// Resize instance desc buffer if needed
	if (instanceCount > m_tlasMaxInstances)
	{
		m_tlasMaxInstances = instanceCount + 1024; // over-allocate
		auto uploadProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto descBuf = CD3DX12_RESOURCE_DESC::Buffer(
			static_cast<UINT64>(m_tlasMaxInstances) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
		device->CreateCommittedResource(&uploadProp, D3D12_HEAP_FLAG_NONE, &descBuf,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(m_instanceDescBuffer.ReleaseAndGetAddressOf()));
	}

	// Fill instance descs
	D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
	D3D12_RANGE readRange{0,0};
	m_instanceDescBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
	for (uint32_t i = 0; i < instanceCount; ++i)
	{
		auto& inst = mapped[i];
		memset(&inst, 0, sizeof(inst));
		memcpy(inst.Transform, &instances[i].transform, sizeof(float) * 12);
		inst.InstanceID = i;
		inst.InstanceMask = static_cast<UINT>(instances[i].instanceMask);
		inst.InstanceContributionToHitGroupIndex = 0;
		inst.Flags = static_cast<UINT>(instances[i].flags);
		if (instances[i].blasIndex < m_blasList.size())
			inst.AccelerationStructure = m_blasList[instances[i].blasIndex].blasBuffer->GetGPUVirtualAddress();
	}
	m_instanceDescBuffer->Unmap(0, nullptr);

	m_tlasInstanceCount = instanceCount;

	// Prebuild info. Static town -> build ONCE with PREFER_FAST_TRACE (fastest ray traversal),
	// not the old per-frame PREFER_FAST_BUILD.
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.NumDescs = instanceCount;
	inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
	device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

	auto defaultProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

	// Recreate TLAS buffer if needed
	if (!m_tlasBuffer || m_tlasBuffer->GetDesc().Width < prebuild.ResultDataMaxSizeInBytes)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ResultDataMaxSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
			IID_PPV_ARGS(m_tlasBuffer.ReleaseAndGetAddressOf()));
	}
	if (!m_tlasScratch || m_tlasScratch->GetDesc().Width < prebuild.ScratchDataSizeInBytes)
	{
		auto desc = CD3DX12_RESOURCE_DESC::Buffer(prebuild.ScratchDataSizeInBytes,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		device->CreateCommittedResource(&defaultProp, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(m_tlasScratch.ReleaseAndGetAddressOf()));
	}

	// Build TLAS
	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
	buildDesc.Inputs = inputs;
	buildDesc.Inputs.InstanceDescs = m_instanceDescBuffer->GetGPUVirtualAddress();
	buildDesc.DestAccelerationStructureData = m_tlasBuffer->GetGPUVirtualAddress();
	buildDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();

	cmd->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

	auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_tlasBuffer.Get());
	cmd->ResourceBarrier(1, &uavBarrier);
}

void RayTracingManager::RebuildTlasWithDynamic(ID3D12GraphicsCommandList4* cmd,
	const RTInstance* dynamic, uint32_t dynamicCount)
{
	if (m_staticInstances.empty()) return;   // 静的セット未保存なら何もしない（初期構築前）
	m_rebuildScratch.clear();
	m_rebuildScratch.reserve(m_staticInstances.size() + dynamicCount);
	m_rebuildScratch.insert(m_rebuildScratch.end(), m_staticInstances.begin(), m_staticInstances.end());
	if (dynamic && dynamicCount)
		m_rebuildScratch.insert(m_rebuildScratch.end(), dynamic, dynamic + dynamicCount);
	// BuildTLAS はバッファを再利用（容量内なら再確保なし）。PREFER_FAST_TRACE のまま毎フレーム再構築。
	BuildTLAS(cmd, m_rebuildScratch.data(), static_cast<uint32_t>(m_rebuildScratch.size()));
}

void RayTracingManager::DispatchWaterReflection(ID3D12GraphicsCommandList4* cmd,
	ID3D12Resource* depthBuffer, const XMMATRIX& invViewProj,
	const XMFLOAT3& cameraPos, float waterSurfaceY)
{
	if (!m_valid || !m_rtPso || !m_tlasBuffer) return;

	uint32_t halfW = m_screenW / 2;
	uint32_t halfH = m_screenH / 2;

	// Update constants
	if (m_constantsMapped)
	{
		auto* cb = static_cast<RTConstants*>(m_constantsMapped);
		cb->InvViewProj = XMMatrixTranspose(invViewProj);
		cb->CameraPos = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0);
		cb->waterSurfaceY = waterSurfaceY;
		cb->outputWidth = halfW;
		cb->outputHeight = halfH;
	}

	// Set pipeline and dispatch
	cmd->SetComputeRootSignature(m_globalRootSig.Get());
	cmd->SetPipelineState1(m_rtPso.Get());
	cmd->SetComputeRootConstantBufferView(0, m_constantsCB->GetGPUVirtualAddress());
	cmd->SetComputeRootShaderResourceView(1, m_tlasBuffer->GetGPUVirtualAddress());
	// UAV and depth SRV would be set via descriptor table

	D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
	dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenRecord;
	dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_shaderRecordSize;
	dispatchDesc.MissShaderTable.StartAddress = m_missRecord;
	dispatchDesc.MissShaderTable.SizeInBytes = m_shaderRecordSize;
	dispatchDesc.MissShaderTable.StrideInBytes = m_shaderRecordSize;
	dispatchDesc.HitGroupTable.StartAddress = m_hitGroupRecord;
	dispatchDesc.HitGroupTable.SizeInBytes = m_shaderRecordSize;
	dispatchDesc.HitGroupTable.StrideInBytes = m_shaderRecordSize;
	dispatchDesc.Width = halfW;
	dispatchDesc.Height = halfH;
	dispatchDesc.Depth = 1;

	cmd->DispatchRays(&dispatchDesc);

	// UAV barrier on reflection texture
	auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(m_reflectionTexture.Get());
	cmd->ResourceBarrier(1, &barrier);
}
