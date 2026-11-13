#pragma once
#include <SFML/Graphics.hpp>
#include <algorithm>
#include <string>
#include "Registry.h"
#include "Components.h"
#include "Game.h"

// Рисует поверх мира: здоровье, счёт, номер волны и экраны Game Over / Victory.
// Текст рисуется, если нашёлся системный шрифт. Иначе счёт и волна
// показываются в заголовке окна (см. GameScene в main.cpp).
class Hud
{
	sf::Font font;
	bool hasFont = false;
	int lastMax = 5; // чтобы рисовать пустые ячейки, когда игрок уже удалён

	void drawText(sf::RenderWindow& w, const std::string& str, unsigned size, sf::Color color, float y)
	{
		sf::Text text(font, str, size);
		text.setFillColor(color);
		auto b = text.getLocalBounds();
		text.setOrigin({ b.position.x + b.size.x / 2.f, b.position.y + b.size.y / 2.f });
		text.setPosition({ w.getSize().x / 2.f, y });
		w.draw(text);
	}

	void drawScore(sf::RenderWindow& w, int score)
	{
		if (!hasFont) return;
		sf::Text text(font, "Score: " + std::to_string(score), 22);
		text.setFillColor(sf::Color::White);
		auto b = text.getLocalBounds();
		text.setOrigin({ b.position.x + b.size.x, 0.f }); // выравнивание по правому краю
		text.setPosition({ w.getSize().x - 12.f, 8.f });
		w.draw(text);
	}

	void drawWave(sf::RenderWindow& w, int wave)
	{
		if (!hasFont || wave <= 0) return;
		std::string label = "Wave " + std::to_string(wave);
		if (wave % 5 == 0) label += "  -  BOSS";
		drawText(w, label, 22, sf::Color(220, 220, 120), 20.f);
	}

	void drawBossBar(sf::RenderWindow& w, Registry& reg)
	{
		reg.each<BossAI, Health>([&](Entity, BossAI&, Health& h)
			{
				float ratio = h.max > 0 ? std::max(0.f, (float)h.hp / h.max) : 0.f;
				const float width = 400.f, height = 14.f;
				float x = (w.getSize().x - width) / 2.f, y = 40.f;

				sf::RectangleShape bg({ width, height });
				bg.setPosition({ x, y });
				bg.setFillColor(sf::Color(40, 40, 40));
				bg.setOutlineThickness(1.f);
				bg.setOutlineColor(sf::Color(200, 200, 200));
				w.draw(bg);

				sf::RectangleShape fg({ width * ratio, height });
				fg.setPosition({ x, y });
				fg.setFillColor(ratio <= 0.5f ? sf::Color(255, 40, 120) : sf::Color(160, 60, 200));
				w.draw(fg);
			});
	}

	void drawHealth(sf::RenderWindow& w, Registry& reg)
	{
		int hp = 0;
		Entity p = findPlayer(reg);
		if (auto* h = p.isNull() ? nullptr : reg.tryGet<Health>(p))
		{
			hp = h->hp;
			lastMax = h->max;
		}

		sf::RectangleShape cell({ 26.f, 14.f });
		cell.setOutlineThickness(1.f);
		cell.setOutlineColor(sf::Color(200, 200, 200));
		for (int i = 0; i < lastMax; ++i)
		{
			cell.setPosition({ 10.f + i * 32.f, 10.f });
			cell.setFillColor(i < hp ? sf::Color(60, 200, 60) : sf::Color(50, 50, 50));
			w.draw(cell);
		}
	}

	void drawOverlay(sf::RenderWindow& w, GameState state, const GameData& data)
	{
		bool win = state == GameState::Victory;
		sf::Vector2f size(w.getSize());

		sf::RectangleShape dim(size);
		dim.setFillColor(sf::Color(0, 0, 0, 170));
		w.draw(dim);

		sf::Color accent = win ? sf::Color(80, 220, 80) : sf::Color(220, 50, 50);

		if (hasFont)
		{
			drawText(w, win ? "YOU WIN" : "GAME OVER", 64, accent, size.y / 2.f - 50.f);
			drawText(w, "Wave " + std::to_string(data.wave) + "   Score: " + std::to_string(data.score),
				32, sf::Color::White, size.y / 2.f + 15.f);
			drawText(w, "Press R to restart", 24, sf::Color(200, 200, 200), size.y / 2.f + 65.f);
		}
		else
		{
			sf::RectangleShape bar({ 300.f, 10.f });
			bar.setOrigin({ 150.f, 5.f });
			bar.setPosition({ size.x / 2.f, size.y / 2.f });
			bar.setFillColor(accent);
			w.draw(bar);
		}
	}

public:
	void DrawUpgradePick(sf::RenderWindow& w, const GameData& data, UpgradeKind a, UpgradeKind b)
	{
		sf::Vector2f size(w.getSize());

		sf::RectangleShape dim(size);
		dim.setFillColor(sf::Color(0, 0, 0, 170));
		w.draw(dim);

		if (hasFont)
		{
			drawText(w, "Wave " + std::to_string(data.wave) + " cleared! Choose a buff:",
				28, sf::Color(120, 220, 120), size.y / 2.f - 70.f);
			drawText(w, "1) " + std::string(upgradeName(a)), 26, sf::Color::White, size.y / 2.f - 10.f);
			drawText(w, "2) " + std::string(upgradeName(b)), 26, sf::Color::White, size.y / 2.f + 30.f);
		}
		else
		{
			sf::RectangleShape bar({ 260.f, 10.f });
			bar.setOrigin({ 130.f, 5.f });
			bar.setPosition({ size.x / 2.f, size.y / 2.f });
			bar.setFillColor(sf::Color(120, 220, 120));
			w.draw(bar);
		}
	}

	Hud()
	{
		const char* paths[] = {
			"C:/Windows/Fonts/arial.ttf",
			"C:/Windows/Fonts/segoeui.ttf",
			"/System/Library/Fonts/Supplemental/Arial.ttf",
			"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
			"font.ttf" // можно положить свой шрифт рядом с exe
		};
		for (auto p : paths)
			if (font.openFromFile(p)) { hasFont = true; break; }
	}

	void Draw(sf::RenderWindow& w, Registry& reg, GameState state, const GameData& data)
	{
		drawHealth(w, reg);
		drawScore(w, data.score);
		drawWave(w, data.wave);
		drawBossBar(w, reg);
		if (state != GameState::Playing)
			drawOverlay(w, state, data);
	}
};