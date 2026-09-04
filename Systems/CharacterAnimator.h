#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <DirectXMath.h>
#include <entt/entt.hpp>

// キャラクタのスケルタルアニメ再生（.skcl クリップ）＋ idle/walk/run 状態機械＋ルートモーション。
// クリップは Blender でUE→MMDリターゲット済みの skinM_engine（= palette 行列）を格納した独自バイナリ。
namespace CharacterAnimator
{
	// gidToName[gid]=エンジンボーン名, gidOffset[gid]=そのボーンの offsetMatrix(逆バインド, 列ベクトル/行優先)。
	// gid はボーンパレットの索引。キャラスポーン時に一度。
	void Init(const std::vector<std::string>& gidToName, const std::vector<DirectX::XMFLOAT4X4>& gidOffset);

	// フォルダ内の *.skcl を全ロード（=コア常駐）。worldScale=キャラの uniformScale。戻り=ロード本数。
	int  LoadClipLibrary(const char* folder, float worldScale);
	// 種類（グループ）をオンデマンドロード（コアは残し、前のグループは破棄）。戻り=ロード本数。
	int  LoadGroup(const std::string& name);
	const std::vector<std::string>& GroupNames();   // clips/groups の利用可能グループ一覧
	const char* CurrentGroup();
	// springbones.rig を読み込み、髪/リボン/袖等のランタイム・スプリングボーンを構築。戻り=有効spring骨数。
	int  LoadSpringRig(const char* path);
	bool IsReady();

	// PlayerSystem の後に毎フレーム呼ぶ。状態機械でクリップ選択＋時間送り＋（任意）ルートモーション。
	void Update(entt::registry& registry, float dt);

	// 現在フレームの skinM を dst[gid] へ書く（gidCount 分を単位行列で初期化してからクリップ骨を上書き）。
	void FillPalette(DirectX::XMFLOAT4X4* dst, uint32_t gidCount);

	// --- ImGui / デバッグ ---
	const std::vector<std::string>& ClipNames();
	void PlayOverride(const std::string& name, bool loop);  // 状態機械を無視して指定クリップを再生
	void ClearOverride();                                   // 状態機械へ復帰
	bool HasOverride();
	const char* StateName();          // "idle"/"walk"/"run"/"override"
	const char* CurrentClipName();
	float CurrentClip01();            // 再生位置 0..1
	bool  GetApplyRootMotion();
	void  SetApplyRootMotion(bool on);
	float WalkSpeedWorld();
	float RunSpeedWorld();

	// --- スプリングボーン（ランタイム二次運動） ---
	bool  SpringAvailable();
	int   SpringBoneCount();
	bool  GetSpringEnabled();
	void  SetSpringEnabled(bool on);
	// 揺れパラメータ（全チェーン共通）: stiffness 0..1(1=剛体追従), drag 0..1(大=すぐ止まる), gravity(下向き強さ)
	void  GetSpringParams(float& stiffness, float& drag, float& gravity);
	void  SetSpringParams(float stiffness, float drag, float gravity);
	// カテゴリ別有効（hair=髪/リボン/袖, skirt=スカート, rigid=胸/肩飾）＋衝突
	void  GetSpringCategories(bool& hair, bool& skirt, bool& rigid);
	void  SetSpringCategories(bool hair, bool skirt, bool rigid);
	bool  GetSpringCollision();
	void  SetSpringCollision(bool on);
	float GetColliderScale();
	void  SetColliderScale(float s);

	// --- デバッグ可視化 ---
	bool  GetSpringDebugDraw();
	void  SetSpringDebugDraw(bool on);
	struct DebugSeg { DirectX::XMFLOAT3 a, b; int category; };   // モデル空間: チェーンの head->tail
	struct DebugCollider { DirectX::XMFLOAT3 a, b; float r; };   // モデル空間: カプセル(a==b で球)
	const std::vector<DebugSeg>& DebugSegments();
	const std::vector<DebugCollider>& DebugColliders();
}
