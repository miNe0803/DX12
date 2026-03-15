#include "RootSignature.h"
#include "Engine.h"
#include <d3dx12.h>

RootSignature::RootSignature()
{
	auto flag = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT; // アプリケーションの入力アセンブラを使用する
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS; // ドメインシェーダーのルートシグネチャへんアクセスを拒否する
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS; // ハルシェーダーのルートシグネチャへんアクセスを拒否する
	flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS; // ジオメトリシェーダーのルートシグネチャへんアクセスを拒否する


	// ECSのコンポーネント構造に合わせた3つのルートパラメータ
	// 0: TransformComponent用 (b0)
	// 1: PBRPropertyComponent用 (b1)
	// 2: MaterialComponentのテクスチャ群 (t0-t3)
	CD3DX12_ROOT_PARAMETER rootParam[3] = {}; // 定数バッファとテクスチャの2
	rootParam[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);

	// [Param 1] PBRプロパティ用: ピクセルシェーダーのみ参照 (b1)
	// ここに以前こだわった RimParams.y (NormalScale) が入る
	rootParam[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

	// PBRに必要な枚数を確報(アルベド、法線、金属度、粗さ)
	CD3DX12_DESCRIPTOR_RANGE tableRange[1] = {}; // ディスクリプタテーブル
	tableRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0); // t0~t3にSRVを4枚分確保
	rootParam[2].InitAsDescriptorTable(std::size(tableRange), tableRange, D3D12_SHADER_VISIBILITY_PIXEL);

	// スタティックサンプラー (s0)
	auto sampler = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

	// ルートシグニチャの設定（設定したいルートパラメーターとスタティックサンプラーを入れる）
	D3D12_ROOT_SIGNATURE_DESC desc = {};
	desc.NumParameters = std::size(rootParam); // ルートパラメーターの個数をいれる
	desc.NumStaticSamplers = 1; // サンプラーの個数をいれる
	desc.pParameters = rootParam; // ルートパラメーターのポインタをいれる
	desc.pStaticSamplers = &sampler; // サンプラーのポインタを入れる
	desc.Flags = flag; // フラグを設定

	ComPtr<ID3DBlob> pBlob;
	ComPtr<ID3DBlob> pErrorBlob;

	// シリアライズ
	auto hr = D3D12SerializeRootSignature(
		&desc,
		D3D_ROOT_SIGNATURE_VERSION_1_0,
		pBlob.GetAddressOf(),
		pErrorBlob.GetAddressOf());
	if (FAILED(hr))
	{
		printf("PBRルートシグネチャシリアライズに失敗\n");
		return;
	}

	// ルートシグネチャ生成
	hr = g_Engine->Device()->CreateRootSignature(
		0, // GPUが複数ある場合のノードマスク（今回は1個しか無い想定なので0）
		pBlob->GetBufferPointer(), // シリアライズしたデータのポインタ
		pBlob->GetBufferSize(), // シリアライズしたデータのサイズ
		IID_PPV_ARGS(m_pRootSignature.GetAddressOf())); // ルートシグニチャ格納先のポインタ
	if (FAILED(hr))
	{
		printf("PBRルートシグネチャの生成に失敗\n");
		return;
	}

	m_IsValid = true;
}

bool RootSignature::IsValid()
{
	return m_IsValid;
}

ID3D12RootSignature* RootSignature::Get()
{
	return m_pRootSignature.Get();
}
