#include "Engine.h"
#include "EngineBarrier.h"
#include "Core/GpuDebugLabels.h"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <stdio.h>
#include <Windows.h>

Engine* g_Engine;

namespace {
// First BeginRender only: flush queue (avoids COMMAND_ALLOCATOR_SYNC).
int g_EngineFirstBeginRender = 1;
}

bool Engine::Init(HWND hwnd, UINT windowWidth, UINT windowHeight)
{
	m_FrameBufferWidth = windowWidth;
	m_FrameBufferHeight = windowHeight;
	m_hWnd = hwnd;

	if (!CreateDevice())
	{
		printf("Engine::Init: CreateDevice failed.\n");
		return false;
	}
	if (!CreateCommandQueue())
	{
		printf("Engine::Init: CreateCommandQueue failed.\n");
		return false;
	}
	if (!CreateSwapChain())
	{
		printf("Engine::Init: CreateSwapChain failed.\n");
		return false;
	}
	if (!InitGfxCommandLists())
	{
		printf("Engine::Init: InitGfxCommandLists failed.\n");
		return false;
	}
	if (!CreateFence())
	{
		printf("Engine::Init: CreateFence failed.\n");
		return false;
	}
	if (!CreateDepthStencil())
	{
		printf("Engine::Init: CreateDepthStencil failed.\n");
		return false;
	}
	CreateViewPort();
	CreateScissorRect();

	if (!CreateRenderTarget())
	{
		printf("Engine::Init: CreateRenderTarget failed.\n");
		return false;
	}

	printf("Engine::Init: OK.\n");
	return true;
}

bool Engine::CreateDevice()
{
	auto hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_pDevice.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;
#if defined(_DEBUG)
	ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(m_pDevice->QueryInterface(IID_PPV_ARGS(&infoQueue))))
	{
		// PIX 併用時、検証が厳しくなり ERROR が増え SetBreakOnSeverity でデバッガが止まる（クラッシュに見える）。
		// DX12_DISABLE_BREAK_ON_ERROR=1 で無効化（DX12_DISABLE_DEBUG_LAYER=1 と併用推奨）。
		wchar_t ev[8]{};
		const DWORD n = GetEnvironmentVariableW(L"DX12_DISABLE_BREAK_ON_ERROR", ev, 8u);
		const bool noBreak = (n > 0 && ev[0] == L'1');
		if (!noBreak)
		{
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
			infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		}
	}
#endif
	return true;
}

bool Engine::CreateCommandQueue()
{
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;

	auto hr = m_pDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(m_pQueue.ReleaseAndGetAddressOf()));

	return SUCCEEDED(hr);
}

bool Engine::CreateSwapChain()
{
	IDXGIFactory4* pFactory = nullptr;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&pFactory));
	if (FAILED(hr))
	{
		return false;
	}

	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferDesc.Width = m_FrameBufferWidth;
	desc.BufferDesc.Height = m_FrameBufferHeight;
	desc.BufferDesc.RefreshRate.Numerator = 60;
	desc.BufferDesc.RefreshRate.Denominator = 1;
	desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = FRAME_BUFFER_COUNT;
	desc.OutputWindow = m_hWnd;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	IDXGISwapChain* pSwapChain = nullptr;
	hr = pFactory->CreateSwapChain(m_pQueue.Get(), &desc, &pSwapChain);
	if (FAILED(hr))
	{
		pFactory->Release();
		return false;
	}

	hr = pSwapChain->QueryInterface(IID_PPV_ARGS(m_pSwapChain.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		pFactory->Release();
		pSwapChain->Release();
		return false;
	}

	m_CurrentBackBufferIndex = m_pSwapChain->GetCurrentBackBufferIndex();

	pFactory->Release();
	pSwapChain->Release();
	return true;
}

bool Engine::InitGfxCommandLists()
{
	HRESULT hr;
	for (size_t i = 0; i < FRAME_BUFFER_COUNT; i++)
	{
		hr = m_pDevice->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(m_pAllocator[i].ReleaseAndGetAddressOf()));
	}

	if (FAILED(hr))
	{
		return false;
	}

	// Command list は allocator 0 で作成し、各フレームで Reset 時に allocator を差し替える（D3D12 的にOK）。
	// Init フェーズの実行が allocator 0 に偏っていても、BeginRender 側でフレーム毎に安全に待ってから Reset する。
	hr = m_pDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_pAllocator[0].Get(),
		nullptr,
		IID_PPV_ARGS(&m_pMainGfxCmdList)
	);

	if (FAILED(hr))
	{
		return false;
	}

	m_pMainGfxCmdList->Close();
	GPU_SET_NAME(m_pMainGfxCmdList.Get(), L"CmdList:Main (Scene terrain/NPR/legacy PBR)");

	for (int w = 0; w < PBR_RECORD_WORKERS; ++w)
	{
		for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
		{
			hr = m_pDevice->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(m_pPbrRecordAllocator[w][i].ReleaseAndGetAddressOf()));
			if (FAILED(hr))
				return false;
		}
		hr = m_pDevice->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			m_pPbrRecordAllocator[w][0].Get(),
			nullptr,
			IID_PPV_ARGS(m_pPbrRecordGfxCmdList[w].ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
		m_pPbrRecordGfxCmdList[w]->Close();
		// 「parallel DrawIndexedInstanced」込みで 48 wchar 超えるため余裕を持たせる
		wchar_t pbrClName[96];
		swprintf_s(pbrClName, L"CmdList:PBR_Record%u (parallel DrawIndexedInstanced)", static_cast<unsigned>(w));
		GPU_SET_NAME(m_pPbrRecordGfxCmdList[w].Get(), pbrClName);
	}

	for (size_t i = 0; i < FRAME_BUFFER_COUNT; ++i)
	{
		hr = m_pDevice->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(m_pPostAllocator[i].ReleaseAndGetAddressOf()));
		if (FAILED(hr))
			return false;
	}
	hr = m_pDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_pPostAllocator[0].Get(),
		nullptr,
		IID_PPV_ARGS(m_pPostGfxCmdList.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	m_pPostGfxCmdList->Close();
	GPU_SET_NAME(m_pPostGfxCmdList.Get(), L"CmdList:Post (tonemap / ImGui)");

	return true;
}

bool Engine::CreateFence()
{
	for (auto i = 0u; i < FRAME_BUFFER_COUNT; i++)
	{
		m_fenceValue[i] = 0;
	}

	auto hr = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_pFence.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		return false;
	}

	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	return m_fenceEvent != nullptr;
}

void Engine::WaitForGpuIdle()
{
	if (!m_pQueue || !m_pFence || !m_fenceEvent)
		return;
	UINT64 v = m_mainGraphicsFenceValue;
	if (m_fenceValue[0] > v) v = m_fenceValue[0];
	if (m_fenceValue[1] > v) v = m_fenceValue[1];
	++v;
	m_pQueue->Signal(m_pFence.Get(), v);
	if (m_pFence->GetCompletedValue() < v)
	{
		if (FAILED(m_pFence->SetEventOnCompletion(v, m_fenceEvent)))
			return;
		if (WAIT_OBJECT_0 != WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE))
			return;
	}
	if (m_fenceValue[0] < v) m_fenceValue[0] = v;
	if (m_fenceValue[1] < v) m_fenceValue[1] = v;
	m_mainGraphicsFenceValue = v;
}

void Engine::CreateViewPort()
{
	m_Viewport.TopLeftX = 0;
	m_Viewport.TopLeftY = 0;
	m_Viewport.Width = static_cast<float>(m_FrameBufferWidth);
	m_Viewport.Height = static_cast<float>(m_FrameBufferHeight);
	m_Viewport.MinDepth = 0.0f;
	m_Viewport.MaxDepth = 1.0f;
}

void Engine::CreateScissorRect()
{
	m_Scissor.left = 0;
	m_Scissor.right = m_FrameBufferWidth;
	m_Scissor.top = 0;
	m_Scissor.bottom = m_FrameBufferHeight;
}

bool Engine::CreateRenderTarget()
{
	// RTV heap: swap chain buffers + HDR_PBR + HDR_NPR
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.NumDescriptors = FRAME_BUFFER_COUNT + 2;
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	auto hr = m_pDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_pRtvHeap.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		return false;
	}

	m_RtvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < FRAME_BUFFER_COUNT; i++)
	{
		m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(m_pRenderTargets[i].ReleaseAndGetAddressOf()));
		wchar_t swapName[48];
		swprintf_s(swapName, L"SwapChain:BackBuffer%u", i);
		GPU_SET_NAME(m_pRenderTargets[i].Get(), swapName);
		m_pDevice->CreateRenderTargetView(m_pRenderTargets[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += m_RtvDescriptorSize;
	}

	// HDR color buffer R16G16B16A16_FLOAT
	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	clearValue.Color[0] = clearValue.Color[1] = clearValue.Color[2] = 0.0f;
	clearValue.Color[3] = 1.0f;
	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	auto resDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		m_FrameBufferWidth,
		m_FrameBufferHeight,
		1, 1, 1, 0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
	// Scene transitions to RT in Draw; starts as SRV
	hr = m_pDevice->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearValue,
		IID_PPV_ARGS(m_pHdrColor.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		return false;
	}
	GPU_SET_NAME(m_pHdrColor.Get(), L"RT:HDR_PBR_SceneColor");
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	m_pDevice->CreateRenderTargetView(m_pHdrColor.Get(), &rtvDesc, rtvHandle);
	rtvHandle.ptr += m_RtvDescriptorSize;

	// NPR レイヤー（アルファ合成用。初期クリアは透明）
	D3D12_CLEAR_VALUE nprClearValue = clearValue;
	nprClearValue.Color[3] = 0.0f;
	hr = m_pDevice->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&nprClearValue,
		IID_PPV_ARGS(m_pNprHdrColor.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
		return false;
	GPU_SET_NAME(m_pNprHdrColor.Get(), L"RT:HDR_NPR_SceneColor");
	m_pDevice->CreateRenderTargetView(m_pNprHdrColor.Get(), &rtvDesc, rtvHandle);

	return true;
}

bool Engine::CreateDepthStencil()
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = 1;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	auto hr = m_pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pDsvHeap));
	if (FAILED(hr))
	{
		return false;
	}

	m_DsvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

	D3D12_CLEAR_VALUE dsvClearValue;
	dsvClearValue.Format = Engine::kDepthStencilFormat;
	dsvClearValue.DepthStencil.Depth = 1.0f;
	dsvClearValue.DepthStencil.Stencil = 0;

	auto heapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	CD3DX12_RESOURCE_DESC resourceDesc(
		D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		0,
		m_FrameBufferWidth,
		m_FrameBufferHeight,
		1,
		1,
		Engine::kDepthStencilResourceFormat,
		1,
		0,
		D3D12_TEXTURE_LAYOUT_UNKNOWN,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
	hr = m_pDevice->CreateCommittedResource(
		&heapProp,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&dsvClearValue,
		IID_PPV_ARGS(m_pDepthStencilBuffer.ReleaseAndGetAddressOf())
	);

	if (FAILED(hr))
	{
		return false;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_pDsvHeap->GetCPUDescriptorHandleForHeapStart();

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = Engine::kDepthStencilFormat;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.Texture2D.MipSlice = 0;
	m_pDevice->CreateDepthStencilView(m_pDepthStencilBuffer.Get(), &dsvDesc, dsvHandle);
	GPU_SET_NAME(m_pDepthStencilBuffer.Get(), L"DSV:MainDepth");

	return true;
}

void Engine::BeginRender()
{
	// フレーム（バックバッファ）ごとに fence を追跡し、再利用する分だけ待つ。
	const UINT frameIndex = m_CurrentBackBufferIndex % FRAME_BUFFER_COUNT;
	if (g_EngineFirstBeginRender)
	{
		g_EngineFirstBeginRender = 0;
		this->WaitForGpuIdle();
	}
	else if (m_fenceValue[frameIndex] > 0 && m_pFence && m_fenceEvent)
	{
		if (m_pFence->GetCompletedValue() < m_fenceValue[frameIndex])
		{
			if (SUCCEEDED(m_pFence->SetEventOnCompletion(m_fenceValue[frameIndex], m_fenceEvent)))
				WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
		}
	}

	m_currentRenderTarget = m_pHdrColor.Get();
	m_pAllocator[frameIndex]->Reset();
	m_pMainGfxCmdList->Reset(m_pAllocator[frameIndex].Get(), nullptr);
	m_pMainGfxCmdList->RSSetViewports(1, &m_Viewport);
	m_pMainGfxCmdList->RSSetScissorRects(1, &m_Scissor);
	// HDR clear: Scene::Draw (barrier, OMSetRTV, Clear)

	for (int w = 0; w < PBR_RECORD_WORKERS; ++w)
	{
		m_pPbrRecordAllocator[w][frameIndex]->Reset();
		m_pPbrRecordGfxCmdList[w]->Reset(m_pPbrRecordAllocator[w][frameIndex].Get(), nullptr);
		m_pPbrRecordGfxCmdList[w]->RSSetViewports(1, &m_Viewport);
		m_pPbrRecordGfxCmdList[w]->RSSetScissorRects(1, &m_Scissor);
	}
	m_pPostAllocator[frameIndex]->Reset();
	m_pPostGfxCmdList->Reset(m_pPostAllocator[frameIndex].Get(), nullptr);
	m_pPostGfxCmdList->RSSetViewports(1, &m_Viewport);
	m_pPostGfxCmdList->RSSetScissorRects(1, &m_Scissor);
}

void Engine::EndRender()
{
	// Present 後は CurrentBackBufferIndex が次フレーム用に進む。タイムスタンプ/読み戻しは「今提出した」バッファのスロットを見る。
	m_lastSubmittedBackBufferIndex = m_CurrentBackBufferIndex;

	ID3D12Resource* backBuffer = m_pRenderTargets[m_CurrentBackBufferIndex].Get();
	// バックバッファ最終書き込みは Post（ポストプロセス・ImGui）のあと
	EngineDoTransition(m_pPostGfxCmdList.Get(), backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	m_pMainGfxCmdList->Close();
	for (int w = 0; w < PBR_RECORD_WORKERS; ++w)
		m_pPbrRecordGfxCmdList[w]->Close();
	m_pPostGfxCmdList->Close();

	ID3D12CommandList* ppCmdLists[1 + PBR_RECORD_WORKERS + 1];
	ppCmdLists[0] = m_pMainGfxCmdList.Get();
	for (int w = 0; w < PBR_RECORD_WORKERS; ++w)
		ppCmdLists[1 + w] = m_pPbrRecordGfxCmdList[w].Get();
	ppCmdLists[1 + PBR_RECORD_WORKERS] = m_pPostGfxCmdList.Get();
	m_pQueue->ExecuteCommandLists(1 + PBR_RECORD_WORKERS + 1, ppCmdLists);

	++m_mainGraphicsFenceValue;
	m_pQueue->Signal(m_pFence.Get(), m_mainGraphicsFenceValue);
	m_fenceValue[m_lastSubmittedBackBufferIndex % FRAME_BUFFER_COUNT] = m_mainGraphicsFenceValue;

	m_pSwapChain->Present(1, 0);
	m_CurrentBackBufferIndex = m_pSwapChain->GetCurrentBackBufferIndex();
}

ID3D12Device6* Engine::Device()
{
	return m_pDevice.Get();
}

ID3D12GraphicsCommandList* Engine::MainGraphicsCmdList()
{
	return m_pMainGfxCmdList.Get();
}

ID3D12GraphicsCommandList* Engine::PbrRecordCmdList(int workerIndex)
{
	if (workerIndex < 0 || workerIndex >= PBR_RECORD_WORKERS)
		return nullptr;
	return m_pPbrRecordGfxCmdList[workerIndex].Get();
}

ID3D12GraphicsCommandList* Engine::PostGraphicsCmdList()
{
	return m_pPostGfxCmdList.Get();
}

ID3D12CommandAllocator* Engine::Allocator(UINT index)
{
	if (index >= FRAME_BUFFER_COUNT) return nullptr;
	return m_pAllocator[index].Get();
}

ID3D12CommandQueue* Engine::Queue()
{
	return m_pQueue.Get();
}

UINT Engine::CurrentBackBufferIndex()
{
	return m_CurrentBackBufferIndex;
}

ID3D12Resource* Engine::GetHdrColorResource()
{
	return m_pHdrColor.Get();
}

ID3D12Resource* Engine::GetNprHdrColorResource()
{
	return m_pNprHdrColor.Get();
}

ID3D12Resource* Engine::GetBackBufferResource()
{
	return m_pRenderTargets[m_CurrentBackBufferIndex].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE Engine::GetBackBufferRtvCpuHandle()
{
	auto h = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += m_CurrentBackBufferIndex * m_RtvDescriptorSize;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Engine::GetHdrRtvCpuHandle()
{
	auto h = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += FRAME_BUFFER_COUNT * m_RtvDescriptorSize;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Engine::GetNprHdrRtvCpuHandle()
{
	auto h = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
	h.ptr += (FRAME_BUFFER_COUNT + 1) * m_RtvDescriptorSize;
	return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Engine::GetDsvCpuHandle()
{
	return m_pDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void Engine::ExecuteAndWait()
{
	m_pMainGfxCmdList->Close();
	ID3D12CommandList* ppCmdLists[] = { m_pMainGfxCmdList.Get() };
	m_pQueue->ExecuteCommandLists(1, ppCmdLists);

	UINT64 fenceValue = m_mainGraphicsFenceValue;
	if (m_fenceValue[0] > fenceValue) fenceValue = m_fenceValue[0];
	if (m_fenceValue[1] > fenceValue) fenceValue = m_fenceValue[1];
	++fenceValue;
	m_pQueue->Signal(m_pFence.Get(), fenceValue);

	if (m_pFence->GetCompletedValue() < fenceValue)
	{
		m_pFence->SetEventOnCompletion(fenceValue, m_fenceEvent);
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}

	if (m_fenceValue[0] < fenceValue) m_fenceValue[0] = fenceValue;
	if (m_fenceValue[1] < fenceValue) m_fenceValue[1] = fenceValue;
	m_mainGraphicsFenceValue = fenceValue;
}
