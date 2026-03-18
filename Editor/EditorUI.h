#pragma once

#include <entt/entt.hpp>
#include <optional>

/// 各エディタウィンドウの開閉（メニューからトグル、× で閉じる）
struct EditorState
{
	bool showPlayerMovement = false;

	bool showBossStatus = false;
	bool showTimelineEditor = false;
	bool showAOESetup = false;

	bool showForestSpawner = false;
	bool showLODTuner = false;

	bool showTerrainParams = false;
	bool showAtmosphere = false;

	bool showHierarchy = false;
	bool showProfiler = false;

	bool showCharacterHierarchy = false;

	bool showAsyncModelLoad = false;
	bool showImGuiDemo = false;
};

class EditorUI
{
public:
	void Draw(entt::registry& registry);

private:
	void DrawMainMenu();
	void DrawWindows(entt::registry& registry);

	void DrawCharacterHierarchyAndInspector(entt::registry& registry);
	void DrawAsyncModelLoadPanel(entt::registry& registry);
	void DrawPlayerMovementPanel(entt::registry& registry);

	EditorState m_State;
	/// 親またはパーツ（インスペクタは親の Transform を編集）
	std::optional<entt::entity> m_selectedSceneModel;

	static entt::entity ResolveModelTransformEntity(entt::registry& registry, entt::entity selected);
};
