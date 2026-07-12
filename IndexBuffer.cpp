#include "IndexBuffer.h"
#include <d3dx12.h>
#include "Engine.h"
#include "VertexBuffer.h"   // GpuUploadToDefault

IndexBuffer::IndexBuffer(size_t size, const uint32_t* pInitData)
{
	// DEFAULT ヒープ(VRAM)へアップロード（UPLOAD ヒープの毎フレーム PCIe 読みを解消）
	m_pBuffer = GpuUploadToDefault(pInitData, size);
	if (!m_pBuffer)
	{
		printf("[OnInit] IndexBuffer: VRAM upload failed\n");
		return;
	}
	m_View = {};
	m_View.BufferLocation = m_pBuffer->GetGPUVirtualAddress();
	m_View.Format = DXGI_FORMAT_R32_UINT;
	m_View.SizeInBytes = static_cast<UINT>(size);
	m_IsValid = true;
}

bool IndexBuffer::IsValid()
{
	return m_IsValid;
}

D3D12_INDEX_BUFFER_VIEW IndexBuffer::View() const
{
	return m_View;
}
