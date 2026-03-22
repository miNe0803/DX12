#include "PipelineState.h"
#include "Engine.h"
#include "Core/GpuDebugLabels.h"
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include <sstream>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	std::unordered_map<std::wstring, ComPtr<ID3DBlob>> s_shaderBlobCache;
	std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> s_graphicsPsoCache;

	ComPtr<ID3DBlob> LoadShaderBlobCached(const std::wstring& filePath)
	{
		if (auto it = s_shaderBlobCache.find(filePath); it != s_shaderBlobCache.end())
			return it->second;

		ComPtr<ID3DBlob> blob;
		const HRESULT hr = D3DReadFileToBlob(filePath.c_str(), blob.GetAddressOf());
		if (FAILED(hr))
			return nullptr;

		s_shaderBlobCache[filePath] = blob;
		return blob;
	}

	std::string NarrowAscii(const std::wstring& s)
	{
		std::string out;
		out.reserve(s.size());
		for (wchar_t c : s)
			out.push_back(static_cast<char>(c & 0xFF));
		return out;
	}

	std::string BuildPsoCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, const std::wstring& vsPath, const std::wstring& psPath)
	{
		std::ostringstream oss;
		oss << "RS=" << reinterpret_cast<uintptr_t>(d.pRootSignature)
			<< "|VS=" << NarrowAscii(vsPath)
			<< "|PS=" << NarrowAscii(psPath)
			<< "|RTV0=" << static_cast<int>(d.RTVFormats[0])
			<< "|DSV=" << static_cast<int>(d.DSVFormat)
			<< "|SM=" << d.SampleMask
			<< "|SC=" << d.SampleDesc.Count
			<< "|PT=" << static_cast<int>(d.PrimitiveTopologyType)
			<< "|ILN=" << d.InputLayout.NumElements;
		for (UINT i = 0; i < d.InputLayout.NumElements; ++i)
		{
			const D3D12_INPUT_ELEMENT_DESC& e = d.InputLayout.pInputElementDescs[i];
			oss << "|E" << i
				<< ":" << (e.SemanticName ? e.SemanticName : "")
				<< ":" << e.SemanticIndex
				<< ":" << static_cast<int>(e.Format)
				<< ":" << e.InputSlot
				<< ":" << e.AlignedByteOffset
				<< ":" << static_cast<int>(e.InputSlotClass)
				<< ":" << e.InstanceDataStepRate;
		}
		const D3D12_RENDER_TARGET_BLEND_DESC& rt0 = d.BlendState.RenderTarget[0];
		oss << "|Cull=" << static_cast<int>(d.RasterizerState.CullMode)
			<< "|DWM=" << static_cast<int>(d.DepthStencilState.DepthWriteMask)
			<< "|BE=" << static_cast<int>(rt0.BlendEnable)
			<< "|SB=" << static_cast<int>(rt0.SrcBlend)
			<< "|DB=" << static_cast<int>(rt0.DestBlend)
			<< "|BO=" << static_cast<int>(rt0.BlendOp);
		return oss.str();
	}

	std::wstring ShaderFileBase(const std::wstring& path)
	{
		const size_t slash = path.find_last_of(L"/\\");
		return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
	}
}

PipelineState::PipelineState()
{
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // HDR メイン描画先
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
}

bool PipelineState::IsValid()
{
	return m_IsValid;
}

void PipelineState::SetInputLayout(D3D12_INPUT_LAYOUT_DESC layout)
{
	desc.InputLayout = layout;
}

void PipelineState::SetRootSignature(ID3D12RootSignature* rootSignature)
{
	desc.pRootSignature = rootSignature;
}

void PipelineState::SetVS(std::wstring filePath)
{
	m_vsPath = std::move(filePath);
	m_pVsBlob = LoadShaderBlobCached(m_vsPath);
	if (!m_pVsBlob)
	{
		printf("VS load failed\n");
		return;
	}

	desc.VS = CD3DX12_SHADER_BYTECODE(m_pVsBlob.Get());
}

void PipelineState::SetPS(std::wstring filePath)
{
	m_psPath = std::move(filePath);
	m_pPSBlob = LoadShaderBlobCached(m_psPath);
	if (!m_pPSBlob)
	{
		printf("PS load failed\n");
		return;
	}

	desc.PS = CD3DX12_SHADER_BYTECODE(m_pPSBlob.Get());
}

void PipelineState::SetCullMode(D3D12_CULL_MODE mode)
{
	desc.RasterizerState.CullMode = mode;
}

void PipelineState::SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK mask)
{
	desc.DepthStencilState.DepthWriteMask = mask;
}

void PipelineState::SetAlphaBlendPremultiplied()
{
	D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.LogicOpEnable = FALSE;
	rt.SrcBlend = D3D12_BLEND_ONE;
	rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = D3D12_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D12_BLEND_ONE;
	rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
}

void PipelineState::Create()
{
	if (m_pVsBlob.Get() == nullptr || m_pPSBlob.Get() == nullptr)
	{
		printf("PipelineState: shader not loaded\n");
		return;
	}
	const std::string key = BuildPsoCacheKey(desc, m_vsPath, m_psPath);
	if (auto it = s_graphicsPsoCache.find(key); it != s_graphicsPsoCache.end())
	{
		m_pPipelineState = it->second;
		m_IsValid = true;
		return;
	}

	auto hr = g_Engine->Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_pPipelineState.ReleaseAndGetAddressOf()));
	if (FAILED(hr))
	{
		printf("PipelineState create failed\n");
		return;
	}

	{
		std::wstring label = L"PSO:";
		label += ShaderFileBase(m_vsPath);
		label += L'|';
		label += ShaderFileBase(m_psPath);
		GPU_SET_NAME(m_pPipelineState.Get(), label.c_str());
	}

	s_graphicsPsoCache[key] = m_pPipelineState;
	m_IsValid = true;
}

void PipelineState::WarmupShaderBytecode(const std::vector<std::wstring>& shaderPaths)
{
	for (const std::wstring& path : shaderPaths)
	{
		(void)LoadShaderBlobCached(path);
	}
}

ID3D12PipelineState* PipelineState::Get()
{
	return m_pPipelineState.Get();
}
