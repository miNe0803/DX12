#pragma once

// ECS: カメラ等にアタッチするポストプロセス用パラメータ（純粋なデータ）
struct PostProcessSettings
{
	float exposure = 1.0f;
	float gamma = 2.2f;
	float bloomIntensity = 1.0f;
	float threshold = 1.0f;
	float blurSize = 1.0f;
	float padding[3] = {};  // 16バイトアライメント
};
