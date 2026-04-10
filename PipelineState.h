#pragma once
#include "ComPtr.h"
#include <d3dx12.h>
#include <string>
#include <vector>

class PipelineState
{
public:
	PipelineState();
	bool IsValid();

	// --- Traditional IA pipeline (VS/PS) ---
	void SetInputLayout(D3D12_INPUT_LAYOUT_DESC layout);
	void SetRootSignature(ID3D12RootSignature* rootSignature);
	void SetVS(std::wstring filePath);
	void SetPS(std::wstring filePath);
	void SetCullMode(D3D12_CULL_MODE mode);
	void SetDepthWriteMask(D3D12_DEPTH_WRITE_MASK mask);
	void SetDepthFunc(D3D12_COMPARISON_FUNC func);
	void SetNumRenderTargets(UINT numRenderTargets);
	void SetRenderTargetFormat(DXGI_FORMAT fmt);
	void SetAlphaBlendPremultiplied();
	void Create();
	static void WarmupShaderBytecode(const std::vector<std::wstring>& shaderPaths);

	// --- Mesh Shader pipeline (AS/MS/PS) ---
	void SetAS(std::wstring filePath);
	void SetMS(std::wstring filePath);
	/// Create a Mesh Shader PSO using Pipeline State Stream.
	/// Requires AS and MS set; PS is optional (depth-only if omitted).
	void CreateMeshPipeline();

	ID3D12PipelineState* Get();

private:
	bool m_IsValid = false;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	ComPtr<ID3D12PipelineState> m_pPipelineState = nullptr;
	ComPtr<ID3DBlob> m_pVsBlob;
	ComPtr<ID3DBlob> m_pPSBlob;
	ComPtr<ID3DBlob> m_pAsBlob;
	ComPtr<ID3DBlob> m_pMsBlob;
	std::wstring m_vsPath;
	std::wstring m_psPath;
	std::wstring m_asPath;
	std::wstring m_msPath;
};
