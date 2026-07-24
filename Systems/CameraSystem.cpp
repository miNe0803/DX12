#include "Systems/CameraSystem.h"
#include "Camera.h"
#include "Engine/ECS/Components.h"
#include <DirectXMath.h>
#include <Windows.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

void CameraSystem::Update(Camera* camera, float dt, entt::registry& registry)
{
	if (!camera)
		return;

	// --- TPS 追従（オービット可能）---: プレイヤー中心にマウスドラッグ/矢印キーで回す。
	// 距離 d は CameraOffset.xz 長、注視点は Height*0.8。ピッチで俯瞰/煽りを切替。
	static float s_orbitYaw = 0.0f;      // rad, 0 = プレイヤー背後(-Z)
	static float s_orbitPitch = 0.22f;   // rad, 既定はやや見下ろし
	static bool  s_dragging = false;
	static long  s_lastX = 0, s_lastY = 0;
	const float kMouseSens = 0.0035f;
	const float kKeySpeed = 2.2f;

	auto view = registry.view<PlayerComponent, TransformComponent>();
	for (auto entity : view)
	{
		const auto& player = view.get<PlayerComponent>(entity);
		if (!player.FollowCamera)
			continue;

		const auto& transform = view.get<TransformComponent>(entity);

		// ImGui が入力を使用中はカメラ操作しない。
		bool uiMouse = false, uiKey = false;
		if (ImGui::GetCurrentContext() != nullptr)
		{
			const ImGuiIO& io = ImGui::GetIO();
			uiMouse = io.WantCaptureMouse; uiKey = io.WantCaptureKeyboard;
		}

		// マウス左ドラッグでオービット（自由視点と同じ操作感）。
		const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		POINT mp{};
		if (!uiMouse && lmb && GetCursorPos(&mp))
		{
			if (!s_dragging) { s_dragging = true; s_lastX = mp.x; s_lastY = mp.y; }
			const long dx = mp.x - s_lastX, dy = mp.y - s_lastY;
			s_lastX = mp.x; s_lastY = mp.y;
			s_orbitYaw += static_cast<float>(dx) * kMouseSens;
			s_orbitPitch += static_cast<float>(dy) * kMouseSens;
		}
		else s_dragging = false;

		// 矢印キーでも回転可能。
		if (!uiKey)
		{
			if (GetAsyncKeyState(VK_RIGHT) & 0x8000) s_orbitYaw += kKeySpeed * dt;
			if (GetAsyncKeyState(VK_LEFT) & 0x8000)  s_orbitYaw -= kKeySpeed * dt;
			if (GetAsyncKeyState(VK_UP) & 0x8000)    s_orbitPitch += kKeySpeed * dt;
			if (GetAsyncKeyState(VK_DOWN) & 0x8000)  s_orbitPitch -= kKeySpeed * dt;
		}
		s_orbitPitch = std::clamp(s_orbitPitch, -0.20f, 1.20f);   // 真下/真上に回り込みすぎない

		// オービット位置（プレイヤー中心）。
		const XMFLOAT3 pp = transform.Position;
		const XMFLOAT3 off = player.CameraOffset;
		float d = sqrtf(off.x * off.x + off.z * off.z);
		if (d < 0.5f) d = 4.0f;
		const float cp = cosf(s_orbitPitch), sp = sinf(s_orbitPitch);
		const float focusY = pp.y + player.Height * 0.8f;

		XMFLOAT3 camPosF;
		camPosF.x = pp.x + sinf(s_orbitYaw) * d * cp;
		camPosF.z = pp.z - cosf(s_orbitYaw) * d * cp;    // yaw=0 → 背後(-Z)
		camPosF.y = focusY + sp * d;                     // pitch で上下
		camera->SetPosition(XMLoadFloat3(&camPosF));

		// Camera::LookAt はピッチ上下反転（全体が 2*camY-targetY で補正）。TPS でも同補正で注視点を狙う。
		XMFLOAT3 lookAt = pp;
		lookAt.y = 2.0f * camPosF.y - focusY;
		camera->LookAt(XMLoadFloat3(&lookAt));
		return;
	}

	camera->Update(dt);   // プレイヤー未追従なら自由視点
}
