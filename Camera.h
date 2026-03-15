#pragma once
#include <DirectXMath.h>

class Camera {
public:
    Camera();
    void Update(float dt); // 入力による更新

    // 行列取得
    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjectionMatrix(float aspect) const;

    // 将来の追跡モード用の口
    void SetTarget(const DirectX::XMVECTOR& targetPos) { m_targetPos = targetPos; }
    void SetPosition(const DirectX::XMVECTOR& pos) { m_position = pos; }

private:
    DirectX::XMVECTOR m_position;   // 現在地
    DirectX::XMVECTOR m_targetPos;  // 注視点
    DirectX::XMVECTOR m_up;         // 上方向ベクトル

    float m_yaw;   // 左右回転
    float m_pitch; // 上下回転
    float m_moveSpeed;
    float m_rotationSpeed;
};