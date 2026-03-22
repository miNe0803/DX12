#pragma once

// ECS: カメラ等にアタッチするポストプロセス用パラメータ（純粋なデータ）
struct PostProcessSettings
{
	float exposure = 1.0f;
	float gamma = 2.2f;
	float bloomIntensity = 1.0f;
	float threshold = 1.0f;
	/// Bloom 抽出のソフトニー幅（輝度）。0 でニー無し（閾値超えの「差分」だけを線形に抽出）
	float bloomKnee = 0.45f;
	float blurSize = 1.0f;
	float padding[2] = {};
	/// NPR レイヤー専用トーン（HDR→LDR）。PBR の exposure/gamma とは独立。
	float nprPostExposure = 1.0f;
	float nprPostGamma = 2.2f;
};
