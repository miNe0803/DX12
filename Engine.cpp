#include "Engine.h"
#include "EngineBarrier.h"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <stdio.h>
#include <Windows.h>

Engine* g_Engine;



bool Engine::Init(HWND hwnd, UINT windowWidth, UINT windowHeight)
{
	m_FrameBufferWidth = windowWidth;
	m_FrameBufferHeight = windowHeight;
	m_hWnd = hwnd;

	if (!CreateDevice())
	{
		printf("?f?o?C?X?????????s");
		return false;
	}
	if (!CreateCommandQueue())
	{
		printf("?R?}???h?L???[?????????s");
		return false;
	}
	if (!CreateSwapChain())
	{
		printf("?X???b?v?`?F?C???????????s");
		return false;
	}
	if (!CreateCommandList())
	{
		printf("?R?}???h???X?g?????????s");
		return false;
	}
	if (!CreateFence())
	{
		printf("?t?F???X?????????s");
		return false;
	}
	if (!CreateDepthStencil())
	{
		printf("?f?v?X?X?e???V???o?b?t?@?????????s\n");
		return false;
	}
	// ?r???[?|?[?g??V?U?[??`???
	CreateViewPort();
	CreateScissorRect();

	if (!CreateRenderTarget())
	{
		printf("?????_?[?^?[?Q?b?g?????????s");
		return false;
	}

	printf("?`??G???W??????????????\n");
	return true;
}

bool Engine::CreateDevice()
{
	auto hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_pDevice.ReleaseAndGetAddressOf()));
	if (FAILED(hr)) return false;
#if defined(_DEBUG)
	// D3D12 ?f?o?b?O: ?G???[????u???[?N???A?o??E?B???h?E??????o???iIntel/NVIDIA ?????????????��????L???j
	ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(m_pDevice->QueryInterface(IID_PPV_ARGS(&infoQueue))))
	{
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
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
	// DXGI?t?@?N?g???[?????
	IDXGIFactory4* pFactory = nullptr;
	HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&pFactory));
	if (FAILED(hr))
	{
		return false;
	}

	// ?X???b?v?`?F?C???????
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

	// ?X???b?v?`?F?C???????
	IDXGISwapChain* pSwapChain = nullptr;
	hr = pFactory->CreateSwapChain(m_pQueue.Get(), &desc, &pSwapChain);
	if (FAILED(hr))
	{
		pFactory->Release();
		return false;
	}

	// IDXGISwapChain3?????
	hr = pSwapChain->QueryInterface(IID_PPV_ARGS(m_pSwapChain.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		pFactory->Release();
		pSwapChain->Release();
		return false;
	}

	// ?o?b?N?o?b?t?@????????
	m_CurrentBackBufferIndex = m_pSwapChain->GetCurrentBackBufferIndex();

	pFactory->Release();
	pSwapChain->Release();
	return true;
}

bool Engine::CreateCommandList()
{
	// ?R?}???h?A???P?[?^?[???
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

	// ?R?}???h???X?g?????
	hr = m_pDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		m_pAllocator[m_CurrentBackBufferIndex].Get(),
		nullptr,
		IID_PPV_ARGS(&m_pCommandList)
	);

	if (FAILED(hr))
	{
		return false;
	}

	//?R?}???h???X?g??J???????????????????A????????????B
	m_pCommandList->Close();

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

	m_fenceValue[m_CurrentBackBufferIndex]++;

	//???????s???????C?x???g?n???h??????????B
	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	return m_fenceEvent != nullptr;
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
	// RTV用ヒープ: バックバッファ2 + HDR用1
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.NumDescriptors = FRAME_BUFFER_COUNT + 1;
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
		m_pDevice->CreateRenderTargetView(m_pRenderTargets[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += m_RtvDescriptorSize;
	}

	// HDR カラーバッファ (R16G16B16A16_FLOAT)
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
	// 初回は BeginRender で PIXEL_SHADER_RESOURCE → RENDER_TARGET に遷移する
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
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	m_pDevice->CreateRenderTargetView(m_pHdrColor.Get(), &rtvDesc, rtvHandle);

	return true;
}

bool Engine::CreateDepthStencil()
{
	//DSV?p??f?B?X?N???v?^?q?[?v????????
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.NumDescriptors = 1;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	auto hr = m_pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_pDsvHeap));
	if (FAILED(hr))
	{
		return false;
	}

	//?f?B?X?N???v?^??T?C?Y?????
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
		Engine::kDepthStencilFormat,
		1,
		0,
		D3D12_TEXTURE_LAYOUT_UNKNOWN,
		D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
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

	//?f?B?X?N???v?^????
	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_pDsvHeap->GetCPUDescriptorHandleForHeapStart();

	m_pDevice->CreateDepthStencilView(m_pDepthStencilBuffer.Get(), nullptr, dsvHandle);

	return true;
}

void Engine::BeginRender()
{
	m_currentRenderTarget = m_pHdrColor.Get();
	m_pAllocator[m_CurrentBackBufferIndex]->Reset();
	m_pCommandList->Reset(m_pAllocator[m_CurrentBackBufferIndex].Get(), nullptr);
	m_pCommandList->RSSetViewports(1, &m_Viewport);
	m_pCommandList->RSSetScissorRects(1, &m_Scissor);
	// HDR への RTV セット・クリアは Scene::Draw() 先頭で「1.バリア → 2.セット → 3.クリア」の順で行う
}

void Engine::WaitRender()
{
	//?`??I?????
	const UINT64 fenceValue = m_fenceValue[m_CurrentBackBufferIndex];
	m_pQueue->Signal(m_pFence.Get(), fenceValue);
	m_fenceValue[m_CurrentBackBufferIndex]++;

	// ????t???[????`??????????????????@????.
	if (m_pFence->GetCompletedValue() < fenceValue)
	{
		// ????????C?x???g????.
		auto hr = m_pFence->SetEventOnCompletion(fenceValue, m_fenceEvent);
		if (FAILED(hr))
		{
			return;
		}

		// ??@????.
		if (WAIT_OBJECT_0 != WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE))
		{
			return;
		}
	}
}

void Engine::EndRender()
{
	// ポストプロセスでバックバッファに描画済み → バックバッファを PRESENT へ
	ID3D12Resource* backBuffer = m_pRenderTargets[m_CurrentBackBufferIndex].Get();
	EngineDoTransition(m_pCommandList.Get(), backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

	m_pCommandList->Close();

	ID3D12CommandList* ppCmdLists[] = { m_pCommandList.Get() };
	m_pQueue->ExecuteCommandLists(1, ppCmdLists);

	m_pSwapChain->Present(1, 0);
	WaitRender();
	m_CurrentBackBufferIndex = m_pSwapChain->GetCurrentBackBufferIndex();
}

ID3D12Device6* Engine::Device()
{
	return m_pDevice.Get();
}

ID3D12GraphicsCommandList* Engine::CommandList()
{
	return m_pCommandList.Get();
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

D3D12_CPU_DESCRIPTOR_HANDLE Engine::GetDsvCpuHandle()
{
	return m_pDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void Engine::ExecuteAndWait()
{
	m_pCommandList->Close();
	ID3D12CommandList* ppCmdLists[] = { m_pCommandList.Get() };
	m_pQueue->ExecuteCommandLists(1, ppCmdLists);
	const UINT64 fenceValue = m_fenceValue[0];
	m_pQueue->Signal(m_pFence.Get(), fenceValue);
	m_fenceValue[0]++;
	if (m_pFence->GetCompletedValue() < fenceValue)
	{
		m_pFence->SetEventOnCompletion(fenceValue, m_fenceEvent);
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}
}
