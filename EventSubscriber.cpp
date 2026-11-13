#include "EventSubscriber.h"
#include <iostream>

void EventSubscriber::subscribe(std::string str, std::function<void(float)> func)
{
	handlers[str].push_back(func);
}

void EventSubscriber::emit(std::string str,float dt)
{
	std::cout << "Emit: " << str << std::endl; // 👈 ДОБАВЬ

	auto it = handlers.find(str);
	if (it == handlers.end())
	{
		std::cout << "NO HANDLERS\n"; // 👈 ДОБАВЬ
		return;
	}
	for (auto& obj : it->second)
	{
		obj(dt);
	}
}
