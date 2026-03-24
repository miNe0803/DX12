#include "Camera.h"

#include <Windows.h>
#include <algorithm>
#include <imgui.h>
#include "DebugLog.h"

using namespace DirectX;

Camera::Camera() :
    m_position(XMVectorSet(0, 200, -1000, 0)),
    m_targetPos(XMVectorSet(0, 200, 0, 0)),
    m_up(XMVectorSet(0, 1, 0, 0)),
    m_yaw(0.0f), m_pitch(0.0f),
    m_moveSpeed(2.0f),
    m_keyRotationSpeed(2.2f),      // ラジアン/秒
    m_mouseSensitivity(0.0035f)    // ラジアン/ピクセル
{
}

void Camera::Update(float dt) {
    const XMVECTOR prevPos = m_position;

    // ImGui が入力を使用している場合はカメラ操作を無効化
    if (ImGui::GetCurrentContext() != nullptr)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard)
        {
            // UI操作中はカメラを更新しない（LookAt など外部制御を想定）
            return;
        }
    }

    // 1) 回転処理：マウスドラッグで Yaw / Pitch を操作（FPS風）
    const bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    POINT p{};
    if (GetCursorPos(&p))
    {
        if (lmbDown)
        {
            if (!m_mouseDragging)
            {
                m_mouseDragging = true;
                m_lastMouseX = p.x;
                m_lastMouseY = p.y;
            }
            const long dx = p.x - m_lastMouseX;
            const long dy = p.y - m_lastMouseY;
            m_lastMouseX = p.x;
            m_lastMouseY = p.y;

            m_yaw += static_cast<float>(dx) * m_mouseSensitivity;
            m_pitch += static_cast<float>(dy) * m_mouseSensitivity; // 上下反転（自然な視点操作）
        }
        else
        {
            m_mouseDragging = false;
        }
    }

    // キーボードでも回転可能（矢印キー）
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) m_yaw += m_keyRotationSpeed * dt;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) m_yaw -= m_keyRotationSpeed * dt;
    if (GetAsyncKeyState(VK_UP) & 0x8000) m_pitch += m_keyRotationSpeed * dt;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) m_pitch -= m_keyRotationSpeed * dt;

    // ピッチ制限（真上・真下を向かないように）
    m_pitch = std::clamp(m_pitch, -XM_PIDIV2 + 0.1f, XM_PIDIV2 - 0.1f);

    // 2) 回転行列を生成
    XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0);

    // 前方向ベクトル（ローカル +Z）
    XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
    // 右方向ベクトル = Up × Forward
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(m_up, forward));

    // 3) 移動処理（WASD）
    if (GetAsyncKeyState('W') & 0x8000) m_position += forward * m_moveSpeed * dt;
    if (GetAsyncKeyState('S') & 0x8000) m_position -= forward * m_moveSpeed * dt;
    if (GetAsyncKeyState('D') & 0x8000) m_position += right * m_moveSpeed * dt;
    if (GetAsyncKeyState('A') & 0x8000) m_position -= right * m_moveSpeed * dt;

    // 4) 注視点の更新
    m_targetPos = m_position + forward;

    static float logAccumSec = 0.0f;
    logAccumSec += dt;
    if (logAccumSec >= 0.5f)
    {
        XMFLOAT3 p{};
        XMFLOAT3 pPrev{};
        XMStoreFloat3(&p, m_position);
        XMStoreFloat3(&pPrev, prevPos);

        const float dx = p.x - pPrev.x;
        const float dy = p.y - pPrev.y;
        const float dz = p.z - pPrev.z;
        const float distPerFrame = sqrtf(dx * dx + dy * dy + dz * dz);
        const float distXZPerFrame = sqrtf(dx * dx + dz * dz);
        const float speedMps = (dt > 1e-6f) ? (distPerFrame / dt) : 0.0f;

        DebugLog(
            "[Camera] Pos=(%.2f, %.2f, %.2f) Move/frame=%.3fm (XZ=%.3fm) Speed=%.2fm/s dt=%.4f\n",
            p.x, p.y, p.z,
            distPerFrame, distXZPerFrame,
            speedMps, dt);
        logAccumSec = 0.0f;
    }
}

void Camera::LookAt(const XMVECTOR& worldTarget)
{
    XMVECTOR d = XMVectorSubtract(worldTarget, m_position);
    float x = XMVectorGetX(d);
    float y = XMVectorGetY(d);
    float z = XMVectorGetZ(d);
    float horizDist = sqrtf(x * x + z * z);
    if (horizDist < 1e-5f)
        horizDist = 1e-5f;

    m_pitch = atan2f(y, horizDist);
    m_yaw = atan2f(x, z);

    // ピッチ制限
    m_pitch = std::clamp(m_pitch, -XM_PIDIV2 + 0.1f, XM_PIDIV2 - 0.1f);

    XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0);
    XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
    m_targetPos = m_position + forward;
}

XMMATRIX Camera::GetViewMatrix() const {
    return XMMatrixLookAtLH(
        m_position,
        m_position + XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0)),
        m_up);
}

XMMATRIX Camera::GetProjectionMatrix(float aspect) const {
    return XMMatrixPerspectiveFovLH(XMConvertToRadians(60), aspect, 0.1f, 5000.0f);
}
