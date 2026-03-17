#pragma once
#include "ComPtr.h"

struct ID3D12RootSignature;

class RootSignature
{
public:
	// forTerrain=false: メッシュ用 (t0-t3 マテリアル, t4-t6 IBL)。true: 地形用 (t0-t7 マスク, t8-t10 IBL)
	explicit RootSignature(bool forTerrain = false);
	bool IsValid(); // ルートシグネチャの生成に成功したかどうかを返す
	ID3D12RootSignature* Get(); // ルートシグネチャを返す

private:
	bool m_IsValid = false; // ルートシグネチャの生成に成功したかどうか
	ComPtr<ID3D12RootSignature> m_pRootSignature = nullptr; // ルートシグネチャ
};


