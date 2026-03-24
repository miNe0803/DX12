#pragma once
#include "ComPtr.h"

struct ID3D12RootSignature;

class RootSignature
{
public:
	// forTerrain=false: PBR インスタンス (CBV0/1, Root SRV t0 s1, t0-t5 材質, t6-t8 IBL)。
	// true: 地形 (CBV0/1, t0-t3 マスク+地面, t4-t6 IBL, Root SRV t0 s1 payload)
	explicit RootSignature(bool forTerrain = false);
	bool IsValid(); // ルートシグネチャの生成に成功したかどうかを返す
	ID3D12RootSignature* Get(); // ルートシグネチャを返す

private:
	bool m_IsValid = false; // ルートシグネチャの生成に成功したかどうか
	ComPtr<ID3D12RootSignature> m_pRootSignature = nullptr; // ルートシグネチャ
};


