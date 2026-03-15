#include "Camera.h"
#include "keyboard.h" // 既存のキーボード入力クラスを使用
// マウス入力クラスも適宜インクルードしてください

using namespace DirectX;

Camera::Camera() :
    m_position(XMVectorSet(0, 200, -1000, 0)),
    m_targetPos(XMVectorSet(0, 200, 0, 0)),
    m_up(XMVectorSet(0, 1, 0, 0)),
    m_yaw(0.0f), m_pitch(0.0f),
    m_moveSpeed(500.0f), m_rotationSpeed(0.1f) {
}

void Camera::Update(float dt) {
    // 1. 回転処理 (簡易的なキー操作例：後でマウス座標の差分に変えるのがベスト)
    if (GetAsyncKeyState(VK_RIGHT)) m_yaw += m_rotationSpeed * dt;
    if (GetAsyncKeyState(VK_LEFT))  m_yaw -= m_rotationSpeed * dt;
    if (GetAsyncKeyState(VK_UP))    m_pitch += m_rotationSpeed * dt;
    if (GetAsyncKeyState(VK_DOWN))  m_pitch -= m_rotationSpeed * dt;

    // ピッチの制限
    m_pitch = __max(-XM_PIDIV2 + 0.1f, __min(XM_PIDIV2 - 0.1f, m_pitch));

    // 2. 回転行列から方向を算出
    XMMATRIX rotation = XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0);

    // 左手系の前方は +Z
    XMVECTOR forward = XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), rotation);
    // 左手系の右方向 = Up x Forward
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(m_up, forward));

    if (GetAsyncKeyState('W')) m_position += forward * m_moveSpeed * dt;
    if (GetAsyncKeyState('S')) m_position -= forward * m_moveSpeed * dt;
    if (GetAsyncKeyState('D')) m_position += right * m_moveSpeed * dt;
    if (GetAsyncKeyState('A')) m_position -= right * m_moveSpeed * dt;

    // 3. 注視点の更新
    m_targetPos = m_position + forward;
}

XMMATRIX Camera::GetViewMatrix() const {
    // 左手系関数を使用
    return XMMatrixLookAtLH(m_position, m_position + XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), XMMatrixRotationRollPitchYaw(m_pitch, m_yaw, 0)), m_up);
}

XMMATRIX Camera::GetProjectionMatrix(float aspect) const {
    // 左手系関数を使用。巨大モデル用にFarを調整
    return XMMatrixPerspectiveFovLH(XMConvertToRadians(60), aspect, 0.1f, 2000.0f);
}