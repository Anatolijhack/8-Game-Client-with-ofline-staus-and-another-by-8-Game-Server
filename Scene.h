#pragma once
#include <SFML/Graphics.hpp>
#include "Registry.h"
#include "Components.h"

class Scene
{
public:
	virtual ~Scene() = default;
	virtual void Init() {}
	virtual void Update(float dt, const InputState& input) {}
	virtual void Render(float dt) {}
	virtual Registry& GetRegistry() = 0;
	virtual void Stop() {}
	virtual void SetWindow(sf::RenderWindow* win) {}

	virtual int WantsSwitchTo() const { return -1; }
};