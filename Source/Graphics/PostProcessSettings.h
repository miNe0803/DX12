#pragma once

// ECS: カメラ等にアタッチするポストプロセス用パラメータ（純粋なデータ）
struct PostProcessSettings
{
	float exposure = 1.0f;
	float gamma = 2.2f;
	float padding[2] = {};  // 16バイトアライメント
};
