#pragma once
#include <DirectXMath.h>

/// モデル配置（同期 Spawn / 非同期完了時のスポーン共通）
struct ModelSpawnOptions
{
	enum class FootPlacement
	{
		None,               // position.y をそのまま
		SnapFeetToTerrain   // Y = 地形高さ + 足元補正（bounds.Min 基準）
	};

	DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
	float uniformScale = 1.0f;
	float rotationY = 0.0f;
	FootPlacement foot = FootPlacement::None;
	bool addPlayerComponent = false;
	/// true のとき親に NPRTag を付与し、子メッシュは NPR_PS（ランプ/toon3）で描画。OFF のとき PBR のみでデバッグ表示も効かない。
	bool addNprTag = true;
};
