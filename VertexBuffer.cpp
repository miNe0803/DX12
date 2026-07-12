#include "VertexBuffer.h"
#include "Engine.h"
#include <d3dx12.h>
#include <cstring>

// data を DEFAULT ヒープ(VRAM)へ。ステージング UPLOAD -> CopyBufferRegion ->
// GENERIC_READ 遷移を、再利用コマンドリストで即時実行して待機する。
ComPtr<ID3D12Resource> GpuUploadToDefault(const void* data, size_t size)
{
	auto dev = g_Engine ? g_Engine->Device() : nullptr;
	if (!dev || size == 0) return nullptr;

	ComPtr<ID3D12Resource> def;
	auto rd = CD3DX12_RESOURCE_DESC::Buffer(size);
	auto hpD = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(dev->CreateCommittedResource(&hpD, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&def))))
		return nullptr;

	ComPtr<ID3D12Resource> up;
	auto hpU = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	if (FAILED(dev->CreateCommittedResource(&hpU, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up))))
		return nullptr;
	if (data)
	{
		void* p = nullptr;
		if (SUCCEEDED(up->Map(0, nullptr, &p))) { std::memcpy(p, data, size); up->Unmap(0, nullptr); }
	}

	// 再利用する専用コピー用コマンドリスト（メインスレッド前提）
	static ComPtr<ID3D12CommandAllocator> s_alloc;
	static ComPtr<ID3D12GraphicsCommandList> s_list;
	if (!s_alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_alloc));
	if (!s_list)  dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_alloc.Get(), nullptr, IID_PPV_ARGS(&s_list));
	else { s_alloc->Reset(); s_list->Reset(s_alloc.Get(), nullptr); }

	s_list->CopyBufferRegion(def.Get(), 0, up.Get(), 0, size);
	auto bar = CD3DX12_RESOURCE_BARRIER::Transition(def.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
	s_list->ResourceBarrier(1, &bar);
	s_list->Close();
	ID3D12CommandList* lists[] = { s_list.Get() };
	g_Engine->Queue()->ExecuteCommandLists(1, lists);
	g_Engine->WaitForGpuIdle();   // コピー完了 -> ステージング(up)を破棄しても安全
	return def;
}

VertexBuffer::VertexBuffer(size_t size, size_t stride, const void* pInitData)
{
	m_pBuffer = GpuUploadToDefault(pInitData, size);
	if (!m_pBuffer)
	{
		printf("VertexBuffer: VRAM upload failed\n");
		return;
	}
	m_View.BufferLocation = m_pBuffer->GetGPUVirtualAddress();
	m_View.SizeInBytes = static_cast<UINT>(size);
	m_View.StrideInBytes = static_cast<UINT>(stride);
	m_IsValid = true;
}

D3D12_VERTEX_BUFFER_VIEW VertexBuffer::View() const
{
	return m_View;
}

bool VertexBuffer::IsValid()
{
	return m_IsValid;
}

