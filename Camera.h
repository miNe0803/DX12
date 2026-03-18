#pragma once
#include <DirectXMath.h>

class Camera {
public:
    Camera();
    // ???WASD + ????????????/FPS??
    void Update(float dt);

    // ????
    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix(float aspect) const;
    DirectX::XMVECTOR GetPosition() const { return m_position; }

    // ????????????CameraSystem ????
    void SetTarget(const DirectX::XMVECTOR& targetPos) { m_targetPos = targetPos; }
    void SetPosition(const DirectX::XMVECTOR& pos) { m_position = pos; }
    void LookAt(const DirectX::XMVECTOR& worldTarget);

private:
    DirectX::XMVECTOR m_position;   // ????
    DirectX::XMVECTOR m_targetPos;  // ?????????? position + forward?
    DirectX::XMVECTOR m_up;         // ???

    float m_yaw;   // ?????Yaw?
    float m_pitch; // ?????Pitch?
    float m_moveSpeed;

    // ????
    float m_keyRotationSpeed;   // ??????rad/sec?
    float m_mouseSensitivity;   // ?????rad/pixel?

    // ??????????LMB?
    bool m_mouseDragging = false;
    long m_lastMouseX = 0;
    long m_lastMouseY = 0;
};