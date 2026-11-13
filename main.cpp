#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <SFML/Graphics.hpp>

#include "Registry.h"
#include "Components.h"
#include "System.h"
#include "GameSystems.h"
#include "RenderSystem.h"
#include "Scene.h"
#include "Game.h"
#include "Hub.h"
#include "NetClient.h"
#include "Netscene.h"

class SceneManager
{
	std::vector<std::shared_ptr<Scene>> scenes;
	int current = -1;

public:
	void push_scene(std::shared_ptr<Scene> scene) { scenes.push_back(std::move(scene)); }

	void SwitchTo(int index)
	{
		if (index < 0 || index >= (int)scenes.size()) return;
		if (current != -1) scenes[current]->Stop();
		current = index;
		scenes[current]->Init();
	}

	void Run()
	{
		if (scenes.empty()) return;

		sf::RenderWindow window(sf::VideoMode({ 800, 600 }), "Game");
		window.setFramerateLimit(60); // вместо sleep_for(16ms)

		for (auto& s : scenes)
			s->SetWindow(&window); // до Init, чтобы RenderSystem прошёл Init/Start вместе со всеми
		SwitchTo(0);

		using clock = std::chrono::steady_clock;
		auto last = clock::now();

		while (window.isOpen())
		{
			while (auto event = window.pollEvent())
				if (event->is<sf::Event::Closed>())
					window.close();

			InputState input;
			input.left = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::A);
			input.right = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::D);
			input.up = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::W);
			input.down = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::S);
			input.shoot = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Space)
				|| sf::Mouse::isButtonPressed(sf::Mouse::Button::Left);
			sf::Vector2f mouse = window.mapPixelToCoords(sf::Mouse::getPosition(window));
			input.aimX = mouse.x;
			input.aimY = mouse.y;
			input.restart = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::R);
			input.num1 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num1);
			input.num2 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num2);
			input.num3 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num3);
			input.num4 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num4);
			input.num5 = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Num5);

			auto now = clock::now();
			float dt = std::chrono::duration<float>(now - last).count();
			last = now;
			if (dt > 0.1f) dt = 0.1f;

			scenes[current]->Update(dt, input);

			int want = scenes[current]->WantsSwitchTo();
			if (want != -1 && want != current)
				SwitchTo(want);

			window.clear();
			scenes[current]->Render(dt);
			window.display();
		}
	}

	void Stop()
	{
		if (current != -1) scenes[current]->Stop();
	}
};

// Сцена выбора бафа между волнами. Показывает 2 случайные карточки и ждёт,
// пока игрок нажмёт 1 или 2, затем применяет выбор и возвращается в игру.
class WaveClearScene : public Scene
{
	Registry& reg;
	GameData& data;
	Hud& hud;
	sf::RenderWindow* window = nullptr;
	std::mt19937 rng{ std::random_device{}() };
	UpgradeKind optionA = UpgradeKind::MaxHp, optionB = UpgradeKind::Heal;
	float minDisplay = 0.f; // короткая защита от случайного "долетевшего" нажатия
	bool chosen = false;
	int backIndex;

	UpgradeKind randomKind()
	{
		static const UpgradeKind pool[] = {
			UpgradeKind::MaxHp, UpgradeKind::Heal, UpgradeKind::FireRate,
			UpgradeKind::Speed, UpgradeKind::Shotgun, UpgradeKind::Laser,
			UpgradeKind::Bazooka
		};
		return pool[std::uniform_int_distribution<int>(0, (int)(sizeof(pool) / sizeof(pool[0])) - 1)(rng)];
	}

public:
	WaveClearScene(Registry& r, GameData& d, Hud& h, int backSceneIndex)
		: reg(r), data(d), hud(h), backIndex(backSceneIndex) {}

	Registry& GetRegistry() override { return reg; }
	void SetWindow(sf::RenderWindow* win) override { window = win; }

	void Init() override
	{
		optionA = randomKind();
		do { optionB = randomKind(); } while (optionB == optionA); // две разные карточки
		minDisplay = 0.2f;
		chosen = false;
	}

	void Update(float dt, const InputState& input) override
	{
		minDisplay -= dt;
		if (chosen || minDisplay > 0.f) return;

		int pick = input.num1 ? 1 : input.num2 ? 2 : 0;
		if (pick == 0) return;

		Entity player = findPlayer(reg);
		if (!player.isNull())
			applyUpgrade(reg, player, pick == 1 ? optionA : optionB);
		chosen = true;
	}

	void Render(float) override { if (window) hud.DrawUpgradePick(*window, data, optionA, optionB); }

	int WantsSwitchTo() const override { return chosen ? backIndex : -1; }
};

class GameScene : public Scene
{
	Registry& reg;
	GameData& data;
	Hud& hud;
	ModuleRegistry systems;
	sf::RenderWindow* window = nullptr;
	GameState state = GameState::Playing;
	int shownScore = -1;
	int shownWave = -1;
	int pendingSwitch = -1;
	bool hadEnemies = false;
	bool started = false;
	const int clearSceneIndex;

	// Заголовок окна дублирует счёт и статус (работает и без шрифта).
	void UpdateTitle()
	{
		if (!window) return;
		std::string t = "Game - Wave " + std::to_string(data.wave) + " - Score: " + std::to_string(data.score);
		if (state == GameState::GameOver) t += " - GAME OVER (press R to restart)";
		window->setTitle(t);
		shownScore = data.score;
		shownWave = data.wave;
	}

	void SetState(GameState s)
	{
		state = s;
		UpdateTitle();
	}

	void Reset()
	{
		resetWorld(reg);
		data.score = 0;
		data.wave = 0;
		hadEnemies = false;
		pendingSwitch = -1;
		SetState(GameState::Playing);
	}

public:
	GameScene(Registry& r, GameData& d, Hud& h, int clearIndex)
		: reg(r), data(d), hud(h), systems(r), clearSceneIndex(clearIndex)
	{
		systems.push_module(std::make_shared<InputSystem>(reg, data));
		systems.push_module(std::make_shared<EnemySystem>(reg));
		systems.push_module(std::make_shared<BossSystem>(reg));
		systems.push_module(std::make_shared<MoveSystem>(reg));
		systems.push_module(std::make_shared<CollisionSystem>(reg, data));
		systems.push_module(std::make_shared<ContactDamageSystem>(reg));
		systems.push_module(std::make_shared<BoundsSystem>(reg, 800.f, 600.f));
		systems.push_module(std::make_shared<WallCollisionSystem>(reg));
		systems.push_module(std::make_shared<BeamSystem>(reg));
		systems.push_module(std::make_shared<WaveSystem>(reg, data, 800.f, 600.f));
		systems.push_module(std::make_shared<BulletWallSystem>(reg, data));
		systems.push_module(std::make_shared<ExplosionSystem>(reg));
		systems.push_module(std::make_shared<FlameSystem>(reg));
	}

	Registry& GetRegistry() override { return reg; }

	void SetWindow(sf::RenderWindow* win) override
	{
		window = win;
		systems.push_module(std::make_shared<RenderSystem>(reg, *window));
	}

	// Вызывается и в первый раз, и каждый раз при возврате из WaveClearScene.
	// Мир пересоздаём только один раз: возврат должен продолжить игру, а не начать её заново.
	void Init() override
	{
		if (!started)
		{
			Reset();
			systems.Init();
			systems.Start();
			started = true;
		}
		pendingSwitch = -1;
	}

	void Update(float dt, const InputState& input) override
	{
		pendingSwitch = -1;

		if (state == GameState::Playing)
		{
			systems.SetInput(input);
			systems.Update(dt);

			GameState next = evaluateState(reg);
			if (next != GameState::Playing)
			{
				SetState(next);
				return;
			}

			bool anyEnemy = false;
			reg.each<Enemy>([&](Entity, Enemy&) { anyEnemy = true; });
			// Волна только что зачищена (враги были - и вот их не стало) -> смена сцены.
			if (hadEnemies && !anyEnemy && data.wave > 0)
				pendingSwitch = clearSceneIndex;
			hadEnemies = anyEnemy;

			if (data.score != shownScore || data.wave != shownWave)
				UpdateTitle();
		}
		static bool spawned2 = false;
		if (!spawned2 && sf::Keyboard::isKeyPressed(sf::Keyboard::Key::P))
		{
			spawned2 = true;
			Entity p2 = reg.create();
			reg.add<Player>(p2);
			reg.add<Position>(p2, 100.f, 100.f);
			reg.add<Velocity>(p2);
			reg.add<Health>(p2, 5, 5);
			reg.add<Collider>(p2);
			reg.add<Weapon>(p2);
			reg.add<MoveStats>(p2);
		}
		else if (input.restart)
		{
			Reset();
		}
	}

	void Render(float dt) override
	{
		systems.Render(dt);
		if (window)
			hud.Draw(*window, reg, state, data);
	}

	void Stop() override { systems.Stop(); }

	int WantsSwitchTo() const override { return pendingSwitch; }
};

//int main()
//{
	//Для соло игры
	//Registry reg;
	//GameData data;
	//Hud hud;

	//SceneManager scenes;
	//auto gameScene = std::make_shared<GameScene>(reg, data, hud, /*clearSceneIndex=*/1);
	//scenes.push_scene(gameScene);                                            // index 0
	//scenes.push_scene(std::make_shared<WaveClearScene>(reg, data, hud, 0));   // index 1

	//scenes.Run();
	//scenes.Stop();
//}
	int main(int argc, char** argv)
	{
		
		std::string token = argc > 1 ? argv[1] : "5c855de7e78e524771af8e3a85e84292d1f3fa2386d94bef39b1dbe6e336c0d7";
		std::string party_code = argc > 2 ? argv[2] : "";// пока токен берём из командной строки

		if (!party_code.empty() && party_code.size() != proto::PARTY_CODE_LEN)
		{
			std::cout << "Party code must be 6 chars\n";
			return 1;                       // иначе сервер кикнет за неверную длину
		}
	
		
		Registry reg;
		GameData data;
		Hud hud;

		
		NetClient::Config cfg;
		cfg.host = "localhost";
		cfg.port = "8765";              
		cfg.party_code = party_code;

		SceneManager scenes;
		scenes.push_scene(std::make_shared<NetScene>(reg, data, hud, cfg, token));

		scenes.Run();
		scenes.Stop();
	}
