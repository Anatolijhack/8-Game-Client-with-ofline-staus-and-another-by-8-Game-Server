#pragma once
#include <memory>
#include <string>
#include <SFML/Graphics.hpp>
#include "Registry.h"
#include "Components.h"
#include "System.h"
#include "RenderSystem.h"
#include "Scene.h"
#include "Game.h"            // GameState, UpgradeKind
#include "Hub.h"             // Hud (òàê ïîäêëþ÷åíî â âàøåì main.cpp)
#include "WorldMirror.h"
#include "NetClient.h"

// Ñåòåâàÿ ñöåíà: çàìåíÿåò GameScene è WaveClearScene.
// Èãðîâûå ñèñòåìû çäåñü ÍÅ çàïóñêàþòñÿ: ìèð ñ÷èòàåò ñåðâåð, à ñöåíà òîëüêî
//   1. îòäà¸ò ââîä ñåòåâîìó ñëîþ,
//   2. ðàç â êàäð ïðèìåíÿåò ê Registry ñàìûé ñâåæèé ñíèìîê (WorldMirror),
//   3. ðèñóåò Registry âàøèì RenderSystem è Hud.
// Íàïèñàíà ïî èíòåðôåéñó Scene, êîòîðûé âèäåí â âàøåì main.cpp (Scene.h ÿ íå âèäåë).

class NetScene : public Scene
{
	Registry& reg;
	GameData& data;
	Hud& hud;
	WorldMirror mirror;
	NetClient net;
	std::string notice;
	ModuleRegistry render;                 // òîëüêî RenderSystem
	sf::RenderWindow* window = nullptr;
	sf::Font font;
	bool hasFont = false;
	bool started = false;
	int backIndex;                         // êóäà âåðíóòüñÿ ïðè Failed (íàïðèìåð, ýêðàí âõîäà); -1 = îñòà¸ìñÿ
	int pendingSwitch = -1;
	int titleCounter = 0;

	void drawStatus()
	{
		sf::Vector2f size(window->getSize());

		sf::RectangleShape dim(size);
		dim.setFillColor(sf::Color(0, 0, 0, 150));
		window->draw(dim);

		if (!hasFont) return;

		sf::Text text(font, net.status(), 26);
		text.setFillColor(net.state() == NetClient::State::Failed ? sf::Color(240, 90, 90) : sf::Color::White);
		auto b = text.getLocalBounds();
		text.setOrigin({ b.position.x + b.size.x / 2.f, b.position.y + b.size.y / 2.f });
		text.setPosition({ size.x / 2.f, size.y / 2.f });
		window->draw(text);
	}

	void updateTitle()
	{
		if (!window || ++titleCounter < 30) return;
		titleCounter = 0;

		static const char* names[] = { "LOBBY", "COUNTDOWN", "PLAYING", "RESULTS" };

		std::string t = "Game - " + net.status();
		if (net.playing())
		{
			t += " - Wave " + std::to_string(data.wave) + " - Score: " + std::to_string(data.score);
			if (net.rtt_ms() >= 0) t += " - ping " + std::to_string(net.rtt_ms()) + " ms";

			const auto party = net.party();
			if (party.in_party)
			{
				t += " | " + party.code + " | " + names[party.state < 4 ? party.state : 0];
				if (party.state == 1) t += " " + std::to_string(party.countdown);
				for (const auto& m : party.members)
					t += " #" + std::to_string(m.num)
					+ (m.offline ? "(offline)" : m.ready ? "(ready)" : "");
			}
		}
		if (!notice.empty()) t += " | " + notice;

		window->setTitle(t);
	}


public:
	NetScene(Registry& r, GameData& d, Hud& h, NetClient::Config cfg, std::string token, int backSceneIndex = -1)
		: reg(r), data(d), hud(h), mirror(r, d), net(std::move(cfg), mirror), render(r), backIndex(backSceneIndex)
	{
		net.set_token(std::move(token));

		const char* paths[] = {
			"C:/Windows/Fonts/arial.ttf",
			"C:/Windows/Fonts/segoeui.ttf",
			"/System/Library/Fonts/Supplemental/Arial.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			"font.ttf"
		};
		for (auto p : paths)
			if (font.openFromFile(p)) { hasFont = true; break; }
	}

	Registry& GetRegistry() override { return reg; }

	void SetWindow(sf::RenderWindow* win) override
	{
		window = win;
		render.push_module(std::make_shared<RenderSystem>(reg, *window));
	}

	void Init() override
	{
		pendingSwitch = -1;
		if (!started)
		{
			render.Init();
			render.Start();
			started = true;
		}
		mirror.clear();
		net.start();                       // ïîâòîðíûé âûçîâ áåçîïàñåí
	}

	void Update(float, const InputState& input) override
	{
		pendingSwitch = -1;

		net.send_input(input);

		const bool reset = net.take_mirror_reset();
		const bool dropped = net.take_dropped();
		if (reset || dropped)
			mirror.clear();

		const auto party = net.party();
		mirror.set_my_num(party.my_num);

		if (const auto r = net.take_left_reason())
			notice = (r == 3) ? "You were kicked" : "You left the party";

		if (party.resumed)                       // <-- новое
			notice = "Reconnected";

		if (party.in_party && party.state == 2)
			mirror.apply_latest();

		if (net.state() == NetClient::State::Failed && backIndex >= 0)
			pendingSwitch = backIndex;

		updateTitle();
	}

	void Render(float dt) override
	{
		render.Render(dt);                 // RenderSystem ðèñóåò çåðêàëî ìèðà êàê îáû÷íûé ìèð
		if (!window) return;

		const bool live = net.playing() && mirror.received_any();
		if (!live)
		{
			drawStatus();                  // "Connecting...", "Retry in 4 s", "Invalid token" è ò.ï.
			return;
		}

		const std::uint8_t ph = mirror.phase();    // 0 èãðà, 1 âûáîð áîíóñà, 2 ïîáåäà, 3 ïîðàæåíèå
		const GameState state = ph == 2 ? GameState::Victory
			: ph == 3 ? GameState::GameOver
			: GameState::Playing;

		hud.Draw(*window, reg, state, data);

		if (ph == 1)
			hud.DrawUpgradePick(*window, data,
				static_cast<UpgradeKind>(mirror.optionA()),
				static_cast<UpgradeKind>(mirror.optionB()));
	}

	void Stop() override
	{
		net.stop();
		mirror.clear();
	}

	int WantsSwitchTo() const override { return pendingSwitch; }
};