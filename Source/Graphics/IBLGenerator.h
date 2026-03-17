#pragma once

#include "ComPtr.h"
#include <d3d12.h>
#include <string>
#include <functional>

// EXR読み込みとEquirect→キューブマップ変換（起動時1回。ECS移行時もこのクラスはそのまま利用）
class IBLGenerator
{
public:
	IBLGenerator() = default;
	~IBLGenerator() = default;

	// exrPath: .exr ファイルパス（equirectangular想定）, cubemapSize: 1面の解像度（例: 512）
	// executeAndWait: コマンドを記録した直後に呼ぶ。Close/Execute/Wait を行う（参照リソースがスコープ内のうちに実行するため）。
	// 成功時は *outCubemap にキューブマップリソースを返す。呼び出し側で SRV 登録・解放を管理すること。
	// outEquirectTexture: 省略可。指定時は Equirect 2D テクスチャを AddRef して返す（スカイボックスで直接サンプル用）。
	bool Generate(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* commandList,
		const wchar_t* exrPath,
		UINT cubemapSize,
		ID3D12Resource** outCubemap,
		std::function<void()> executeAndWait,
		ID3D12Resource** outEquirectTexture = nullptr
	);

	// EXR がない場合のフォールバック。単色のキューブマップを生成する（空が確実に表示されるようにする）
	// outUploadBufferToKeep: 省略可。指定時は Execute 完了まで保持すること（コマンドリストが参照するため）
	static bool CreateDefaultCubemap(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* commandList,
		UINT size,
		float r, float g, float b, float a,
		ID3D12Resource** outCubemap,
		ID3D12Resource** outUploadBufferToKeep = nullptr
	);

	bool IsValid() const { return m_isValid; }

private:
	bool m_isValid = false;

	bool LoadEXRToScratch(const wchar_t* exrPath, int* outW, int* outH, float** outRgba);
	bool CreateEquirectTexture(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, int width, int height, const float* rgba, ID3D12Resource** outResource);
	bool CreateCubemapResource(ID3D12Device* device, UINT size, ID3D12Resource** outResource);
	bool CreateComputePipeline(ID3D12Device* device);
	void RunEquirect2Cube(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* equirectTexture,
		ID3D12Resource* cubemapResource,
		UINT size
	);

	ComPtr<ID3D12RootSignature> m_pComputeRootSignature;
	ComPtr<ID3D12PipelineState> m_pComputePSO;
	ComPtr<ID3D12Resource> m_pEquirectUploadBuffer;    // Execute 完了まで保持
	ComPtr<ID3D12Resource> m_pEquirectTexture;       // コマンドリストが参照するため Execute 完了まで保持
	ComPtr<ID3D12DescriptorHeap> m_pComputeDescriptorHeap; // RunEquirect2Cube で使用、Execute 完了まで保持
	ComPtr<ID3D12Resource> m_pParamsBuffer;          // コンピュート用定数バッファ、Execute 完了まで保持
};
