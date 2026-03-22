#pragma once

/// ImGui 等から更新 → Scene::Update で PBRConstants に反映
struct NprGpuTuning
{
	float opaqueExposure = 0.8f;
	/// NPR 不透明の最終色に乗算（既定 1）。RenderDoc で HDR_SceneColor がシルエット級に暗く見えるとき 2～8 など。
	float nprHdrViewBoost = 1.0f;
	/// 線形 HDR の各チャンネル上限（Bloom/ACES 白飛び抑制）。0 でクランプ無効。
	float nprHdrLinearClampMax = 5.0f;
	float normalScale = 1.0f;
	float rimPower = 3.0f;
	float rimStrength = 0.3f;
	float virtualLight = 0.85f;
	float transExposure = 0.8f;
	float opaqueAlphaClip = 0.5f;
	float ambientShadowStrength = 1.0f;
	/// セル影の主光: 0=法線マップ優先（細かい影） 1=頂点法線のみ（顔影が安定）
	float celVertexNormalBlend = 0.52f;
	/// 0=ランプ境界が柔らかい 1=急峻（トゥーン帯がはっきり）
	float celShadeSharpness = 0.58f;
	/// リムライト計算で頂点法線を混ぜる（顔周りのギザ軽減）
	float rimVertexNormalBlend = 0.22f;
	/// 0=通常 / 1–4=従来ランプ・緑テスト / 5=t0 生 / 6=t0×頂点(pow前) / 7=リニア×頂点(スフィア前) …RenderDoc 用
	int nprDebugRampView = 0;
	/// true のフレームでモデル登録時、各メッシュの RampMap パスを Output に出す（VS デバッグ出力）
	bool logNprRampPathsOnRegister = false;
};

extern NprGpuTuning g_NprGpuTuning;
