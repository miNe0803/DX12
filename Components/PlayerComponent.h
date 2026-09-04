#pragma once

#include <DirectXMath.h>

// プレイヤー（移動・接地・カメラ追従用パラメータ）
struct PlayerComponent
{
	float WalkSpeed = 4.0f;
	float RunSpeed = 7.0f;
	// 走行/歩行の切替（'/' キーでトグル）。true=走り。
	bool  RunMode = false;
	// 今フレーム WASD 入力で移動したか（アニメ状態機械が idle/walk/run の判定に使用）。
	bool  IsMoving = false;
	float Height = 1.75f;
	float GroundOffset = 0.0f;
	// 立つ地面のワールドY（地形が無い町シーンでは道路レベル。Scene が毎フレーム設定）。
	float GroundY = 0.0f;

	// true のとき CameraSystem が TPS 風に追従（false なら Camera::Update のみ＝自由視点）
	bool FollowCamera = false;
	DirectX::XMFLOAT3 CameraOffset = { 0.0f, 1.8f, -4.0f };

	// CameraSystem がオービット yaw(rad) を毎フレーム書き込む。PlayerSystem がカメラ相対WASD移動に使う。
	float CameraYaw = 0.0f;
};
