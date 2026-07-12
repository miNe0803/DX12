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

	// ---- カラーグレーディング（ACES カーブ前に線形HDRへ適用）。UE5 PostProcessVolume 実値 ----
	float gradeSaturation = 1.025f;         // UE ColorSaturation=1.025
	float gradeContrast   = 1.03f;          // UE ColorContrast=1.03（0.18 ピボット）
	// UE WhiteTemp=6000K（6500K中立よりやや暖色）→ 微小な暖色ゲイン
	float gradeGainR = 1.015f, gradeGainG = 1.0f, gradeGainB = 0.97f;

	// ---- オート露出（eye adaptation）----
	bool  autoExposure = true;    // 平均輝度から露出を自動決定（exposure は手動バイアスに）
	float aeKey = 0.20f;          // 中間グレーのキー値（大きいほど明るく）
	float aeMinExposure = 0.35f;  // 露出クランプ（下限）
	float aeMaxExposure = 3.0f;   // 露出クランプ（上限）
	float aeSpeed = 0.05f;        // 時間平滑の追従率（フレーム毎, 小さいほどゆっくり）
};
