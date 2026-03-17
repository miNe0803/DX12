#ifndef SCENE_H
#define SCENE_H

#include <entt/entt.hpp>

class Scene
{
public:
	bool Init();
	void Update();
	void Draw();

	entt::registry& GetRegistry() { return m_registry; }

private:
	entt::registry m_registry;
};

extern Scene* g_Scene;

#endif // !1
