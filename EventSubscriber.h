#pragma once
#include <unordered_map>
#include <functional>
#include <string>
#include <vector>
class EventSubscriber
{
private:
	std::unordered_map<std::string, std::vector<std::function<void(float)>>> handlers;
public:
	void subscribe(std::string str, std::function<void(float)> func);
	void emit(std::string str,float dt);

};

