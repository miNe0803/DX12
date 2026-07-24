#include "Systems/PlayerSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/TerrainSystem.h"
#include "keyboard.h"
#include <imgui.h>
#include <cmath>

void PlayerSystem::Update(entt::registry& registry, float dt)
{
	// ImGui にキーボードが取られている間（UI入力中）は移動しない。
	bool uiCapture = false;
	if (ImGui::GetCurrentContext() != nullptr)
		uiCapture = ImGui::GetIO().WantCaptureKeyboard;

	auto view = registry.view<PlayerComponent, TransformComponent>();
	for (auto entity : view)
	{
		auto& player = view.get<PlayerComponent>(entity);
		auto& transform = view.get<TransformComponent>(entity);

		// カメラ相対 WASD 移動。W=カメラの向いてる奥、S=手前、A/D=カメラ基準の左右。
		// CameraYaw は CameraSystem がオービット角を書き込む（前フレーム値＝1フレーム遅延, 実用上問題なし）。
		if (!uiCapture)
		{
			float inF = 0.0f, inR = 0.0f;
			if (Keyboard_IsKeyDown(KK_W)) inF += 1.0f;
			if (Keyboard_IsKeyDown(KK_S)) inF -= 1.0f;
			if (Keyboard_IsKeyDown(KK_D)) inR += 1.0f;
			if (Keyboard_IsKeyDown(KK_A)) inR -= 1.0f;
			// カメラ前方(画面奥)=(-sinYaw, cosYaw), 右=(cosYaw, sinYaw)。yaw=0 で 前方=+Z/右=+X（背後カメラ）。
			const float yaw = player.CameraYaw;
			const float fx = -sinf(yaw), fz = cosf(yaw);
			const float rx = cosf(yaw), rz = sinf(yaw);
			// 移動 = カメラ前方(+M, 画面奥へ)。W で顔の向く方＝画面奥へ進む。
			float mx = fx * inF + rx * inR;
			float mz = fz * inF + rz * inR;
			float len = sqrtf(mx * mx + mz * mz);
			if (len > 1e-4f)
			{
				mx /= len; mz /= len;
				transform.Position.x += mx * player.WalkSpeed * dt;
				transform.Position.z += mz * player.WalkSpeed * dt;
				// モデルの前方軸は -Z（RotationY=0 でカメラを向く）。進行方向 (mx,mz) を向くには atan2(-m)。
				transform.RotationY = atan2f(-mx, -mz);
			}
		}

		// 接地: 地形があればその高さ、無ければ町の地面(GroundY, Scene が道路レベルを設定)へ足を置く。
		float ground = TerrainSystem::GetHeight(registry, transform.Position.x, transform.Position.z);
		if (ground == 0.0f) ground = player.GroundY;   // 地形無し町では GroundY(道路レベル)を使用
		transform.Position.y = ground + player.GroundOffset;
	}
}
