#pragma once

#include <DirectXMath.h>

// プレイヤー（移動・接地・カメラ追従用パラメータ）
struct PlayerComponent
{
	float WalkSpeed = 4.0f;
	float Height = 1.75f;
	float GroundOffset = 0.0f;

	// true のとき CameraSystem が TPS 風に追従（false なら Camera::Update のみ＝自由視点）
	bool FollowCamera = false;
	DirectX::XMFLOAT3 CameraOffset = { 0.0f, 1.8f, -4.0f };
};
