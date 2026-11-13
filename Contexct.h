#pragma once
#include "EventSubscriber.h"
#include "Scene.h"
#include <memory>
#include <unordered_map>
#include <iostream>
#include <thread>
#include <chrono>
#include <SFML/Graphics.hpp>
using Entity = int;
struct World;
class SceneManager;
struct InputState
{
	bool left = false;
	bool right = false;
	bool up = false;
	bool down = false;
	bool shoot = false;
};
struct Enemy {};
struct Bullet {};
struct Health
{
	int hp;
};
struct Contexct
{
	EventSubscriber* eventt;
	World* world;
	
};
struct Position
{
	int x;
	int y;
};
struct Velocity
{
	int x;
	int y;
};
struct Shooter
{
	float cooldown = 1.5f; // раз в 1.5 сек
	float timer = 0.0f;
};
struct World
{
    std::vector<Entity> toDestroy;
     bool shoot = false;
   
	Entity player;
	int nextEntity = 0;
	Entity crateEntity()
	{
		return nextEntity++;
	}
	std::unordered_map<Entity, Position> position;
	std::unordered_map<Entity, Velocity> velocity;
	std::unordered_map<Entity, Enemy> enemy;
	std::unordered_map<Entity, Bullet> bullet;
	std::unordered_map<Entity, Health> health;
	std::unordered_map<Entity, Shooter> shooter;
	
	template <typename T>
	bool hashComponent(Entity e) const;

	template<typename T>
	std::unordered_map<Entity, T>& getStorage();
	template<typename First,typename ...Componennts>
	std::vector<Entity> view()
	{
		std::vector<Entity> result;
		auto& obj = getStorage<First>();
		for (auto& [e,comp] : obj)
		{
			if((hashComponent<Componennts>(e)&&...))
			{
				result.push_back(e);
			}
		}
		return result;
	}
    void destroyEntity(Entity e)
    {
        toDestroy.push_back(e);
    }
	void cleanup()
	{
		for (auto e : toDestroy)
		{
			position.erase(e);
			velocity.erase(e);
			enemy.erase(e);   // 💥 ВАЖНО
			bullet.erase(e);  // 💥 ВАЖНО
			health.erase(e);  // 💥 ВАЖНО
		}

		toDestroy.clear();
	}

};
template<>
bool World::hashComponent<Shooter>(Entity e) const
{
	return shooter.count(e);
}

template<>
std::unordered_map<Entity, Shooter>& World::getStorage()
{
	return shooter;
}
template<>
bool World::hashComponent<Position>(Entity e) const 
{
	return position.count(e);
}
template<>
bool World::hashComponent<Velocity>(Entity e) const
{
	return velocity.count(e);
}
template<>
std::unordered_map<Entity, Position>& World::getStorage()
{
	return position;
}
template<>
std::unordered_map<Entity, Velocity>& World::getStorage()
{
	return velocity;
}
template<>
bool World::hashComponent<Bullet>(Entity e) const
{
	return bullet.count(e);
}

// Enemy 💥
template<>
bool World::hashComponent<Enemy>(Entity e) const
{
	return enemy.count(e);
}

// Health 💥
template<>
bool World::hashComponent<Health>(Entity e) const
{
	return health.count(e);
}
// Bullet 💥
template<>
std::unordered_map<Entity, Bullet>& World::getStorage()
{
	return bullet;
}

// Enemy 💥
template<>
std::unordered_map<Entity, Enemy>& World::getStorage()
{
	return enemy;
}

// Health 💥
template<>
std::unordered_map<Entity, Health>& World::getStorage()
{
	return health;
}
struct CommandBuffer
{
	std::vector<std::function<void(World&)>> commands;

	void add(std::function<void(World&)> obj)
	{
		commands.push_back(obj);
	}
	void apply(World& world)
	{
		for (auto& obj : commands)
		{
			obj(world);
		}
		commands.clear();
	}

};