#pragma once
#include <d3d12.h>
#include "ComPtr.h"

// data(size バイト) を DEFAULT ヒープ(VRAM)へ即時アップロードして返す共有ヘルパー。
// ディスクリート GPU の毎フレーム PCIe 読みを避けるため VB/IB は VRAM に置く。
ComPtr<ID3D12Resource> GpuUploadToDefault(const void* data, size_t size);

class VertexBuffer
{
public:
	VertexBuffer(size_t size, size_t stride, const void* pInitData); // �R���X�g���N�^�Ńo�b�t�@�𐶐�
	D3D12_VERTEX_BUFFER_VIEW View() const; // ���_�o�b�t�@�r���[���擾
	bool IsValid(); // �o�b�t�@�̐����ɐ������������擾

private:
	bool m_IsValid = false; // �o�b�t�@�̐����ɐ������������擾
	ComPtr<ID3D12Resource> m_pBuffer = nullptr; // �o�b�t�@
	D3D12_VERTEX_BUFFER_VIEW m_View = {}; // ���_�o�b�t�@�r���[

	VertexBuffer(const VertexBuffer&) = delete;
	void operator = (const VertexBuffer&) = delete;
};
