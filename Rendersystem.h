#pragma once
#include <SFML/Graphics.hpp>
#include "System.h"

class RenderSystem : public System
{
	Registry& reg;
	sf::RenderWindow& window;

	void drawBar(float x, float y, float w, float ratio)
	{
		sf::RectangleShape bg({ w, 3.f });
		bg.setPosition({ x, y });
		bg.setFillColor(sf::Color(40, 40, 40));
		window.draw(bg);

		sf::RectangleShape fg({ w * ratio, 3.f });
		fg.setPosition({ x, y });
		fg.setFillColor(sf::Color(220, 40, 40));
		window.draw(fg);
	}

public:
	RenderSystem(Registry& r, sf::RenderWindow& w) : reg(r), window(w) {}

	void Render(float) override
	{
		sf::RectangleShape rect;

		// стены рисуем отдельно: они не проходят по общей each<Position>-ветке ниже
		reg.each<Wall, Position>([&](Entity, Wall& wall, Position& pos)
			{
				sf::RectangleShape w({ wall.w, wall.h });
				w.setPosition({ pos.x, pos.y });
				w.setFillColor(sf::Color(110, 110, 120));
				window.draw(w);
			});

		// луч лазера: без поворотов, просто ряд точек вдоль линии
		reg.each<Beam>([&](Entity, Beam& b)
			{
				sf::RectangleShape dot({ 4.f, 4.f });
				dot.setFillColor(sf::Color(255, 60, 60));
				const int steps = 10;
				for (int i = 0; i <= steps; ++i)
				{
					float t = (float)i / steps;
					dot.setPosition({ b.x1 + (b.x2 - b.x1) * t - 2.f, b.y1 + (b.y2 - b.y1) * t - 2.f });
					window.draw(dot);
				}
			});
		reg.each<Explosion>([&](Entity, Explosion& x)
			{
				float t = x.life / x.maxLife;           // 1 -> 0 по мере затухания
				float r = x.radius * (1.f - t * 0.5f);  // круг растёт от 50% до 100%
				sf::CircleShape c(r);
				c.setOrigin({ r, r });
				c.setPosition({ x.x, x.y });
				c.setFillColor(sf::Color(255, 140, 0, static_cast<std::uint8_t>(200 * t)));
				window.draw(c);
			});
		reg.each<Flame, Position>([&](Entity, Flame& f, Position& p)
			{
				float t = f.life / f.maxLife;          // 1 -> 0 по мере затухания
				float size = 6.f + (1.f - t) * 8.f;    // частица растёт
				sf::RectangleShape sq({ size, size });
				sq.setOrigin({ size / 2.f, size / 2.f });
				sq.setPosition({ p.x, p.y });
				sq.setFillColor(sf::Color(255, static_cast<std::uint8_t>(60 + 170 * t), 0,
					static_cast<std::uint8_t>(220 * t)));
				window.draw(sq);
			});

		reg.each<Position>([&](Entity e, Position& pos)
			{
				if (reg.has<Wall>(e)) return; // уже нарисована выше
				rect.setPosition(sf::Vector2f(pos.x, pos.y));

				if (reg.has<Player>(e))
				{
					const Health* ph = reg.tryGet<Health>(e);
					const bool dead = ph && ph->hp <= 0;
					rect.setSize({ 20.f, 20.f });
					rect.setFillColor(dead ? sf::Color(90, 90, 90) : sf::Color::Green);
					window.draw(rect);
				}
				else if (auto* en = reg.tryGet<Enemy>(e))
				{
					float w = 20.f, h = 20.f;
					if (auto* c = reg.tryGet<Collider>(e)) { w = c->w; h = c->h; }
					rect.setSize({ w, h });

					switch (en->kind)
					{
					case EnemyKind::Fast:      rect.setFillColor(sf::Color(255, 165, 0)); break;
					case EnemyKind::Tank:      rect.setFillColor(sf::Color(140, 20, 20)); break;
					case EnemyKind::Rocketeer: rect.setFillColor(sf::Color(170, 60, 220)); break;
					case EnemyKind::Boss:
					{
						auto* bh = reg.tryGet<Health>(e);
						bool enraged = bh && bh->hp * 2 <= bh->max;
						rect.setFillColor(enraged ? sf::Color(255, 40, 120) : sf::Color(120, 40, 160));
						break;
					}
					case EnemyKind::Flamer: rect.setFillColor(sf::Color(200, 200, 40)); break;
					default:                   rect.setFillColor(sf::Color::Red); break;
					}
					window.draw(rect);

					// маленькая полоска над врагом (у босса своя, большая, вверху экрана)
					if (en->kind != EnemyKind::Boss)
						if (auto* hh = reg.tryGet<Health>(e))
							drawBar(pos.x, pos.y - 6.f, w, hh->max > 0 ? (float)hh->hp / hh->max : 0.f);
				}
				else if (auto* b = reg.tryGet<Bullet>(e))
				{
					rect.setSize({ 6.f, 6.f });
					rect.setFillColor(b->fromPlayer ? sf::Color::Yellow : sf::Color(255, 140, 0));
					window.draw(rect);
				}
				else if (auto* b = reg.tryGet<Bullet>(e))
				{
					if (reg.has<Explosive>(e))
					{
						rect.setSize({ 10.f, 10.f });
						rect.setFillColor(sf::Color(255, 80, 0));
					}
					else
					{
						rect.setSize({ 6.f, 6.f });
						rect.setFillColor(b->fromPlayer ? sf::Color::Yellow : sf::Color(255, 140, 0));
					}
					window.draw(rect);
				}
			});
	}
};