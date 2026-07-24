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

		// WASD ワールド軸移動。追従カメラは -Z 背後固定なので W=+Z が「奥へ」＝直感的。
		if (!uiCapture)
		{
			float mx = 0.0f, mz = 0.0f;
			if (Keyboard_IsKeyDown(KK_W)) mz += 1.0f;
			if (Keyboard_IsKeyDown(KK_S)) mz -= 1.0f;
			if (Keyboard_IsKeyDown(KK_D)) mx += 1.0f;
			if (Keyboard_IsKeyDown(KK_A)) mx -= 1.0f;
			float len = sqrtf(mx * mx + mz * mz);
			if (len > 1e-4f)
			{
				mx /= len; mz /= len;
				transform.Position.x += mx * player.WalkSpeed * dt;
				transform.Position.z += mz * player.WalkSpeed * dt;
				transform.RotationY = atan2f(mx, mz);   // 進行方向を向く
			}
		}

		// 接地（地形が無ければ GetHeight=0 → y=GroundOffset）。
		float ground = TerrainSystem::GetHeight(registry, transform.Position.x, transform.Position.z);
		transform.Position.y = ground + player.GroundOffset;
	}
}
