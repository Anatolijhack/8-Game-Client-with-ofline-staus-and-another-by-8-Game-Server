#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "Registry.h"
#include "Components.h"
#include "Protocol.h"      // тот же файл, что и на сервере (net/Protocol.h)
#include <unordered_set>

// Зеркало мира: копия серверного мира в обычном Registry клиента.
// Игровые системы (враги, волны, столкновения) на клиенте НЕ запускаются: всё считает сервер.
// Каждый снимок просто перезаписывает мир, а RenderSystem и Hud рисуют его как обычный.
//
// Потоки:
//   push()         вызывается из сетевого потока (кладёт самый свежий снимок, старые отбрасываются);
//   apply_latest() вызывается из главного потока раз в кадр (Registry не потокобезопасен).

class WorldMirror
{
public:
    WorldMirror(Registry& reg, GameData& data) : reg_(reg), data_(data) {}

    // ---- из сетевого потока ----
    void push(std::vector<std::uint8_t>&& packet)
    {
        std::lock_guard<std::mutex> lk(m_);
        latest_ = std::move(packet);
        has_ = true;
    }

    // ---- из главного потока ----
    // true, если применён новый снимок. Повреждённый пакет игнорируется целиком.
    bool apply_latest()
    {
        std::vector<std::uint8_t> p;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!has_) return false;
            p.swap(latest_);
            has_ = false;
        }
        return apply(p.data(), p.size());
    }

    // Что нужно сцене: фаза (0 игра, 1 выбор бонуса, 2 победа, 3 поражение) и два бонуса
    std::uint8_t phase() const { return phase_; }
    std::uint8_t optionA() const { return optA_; }
    std::uint8_t optionB() const { return optB_; }
    std::uint32_t lastTick() const { return tick_; }
    std::uint32_t lastAck() const { return ack_; }
    bool received_any() const { return received_; }

    // Удалить всё созданное зеркалом (например, при потере связи или перед переподключением)
    void clear()
    {
        clear_entities();

        received_ = false;

        phase_ = 0;
        optA_ = 0;
        optB_ = 0;

        tick_ = 0;
        ack_ = 0;

        data_.score = 0;
        data_.wave = 0;
        my_num_ = 0;
        have_match_ = false;

        {
            std::lock_guard<std::mutex> lk(m_);
            latest_.clear();
            has_ = false;
        }
    }
    void set_my_num(std::uint32_t n) { my_num_ = n; }


private:
    // ---------- разобранный снимок ----------
    struct PPlayer {
        bool present = false; std::uint32_t id = 0; std::int16_t x = 0, y = 0;
        std::uint8_t hp = 0, max = 0, weapon = 0; std::uint8_t flags = 0; std::uint16_t points = 0;
    };
    struct PEnemy {
        std::uint32_t id = 0; std::uint8_t kind = 0; std::int16_t x = 0, y = 0;
        std::uint16_t hp = 0, max = 0;
    };
    struct PBullet { std::uint8_t flags = 0; std::int16_t x = 0, y = 0; };
    struct PBeam { std::int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0; };
    struct PExpl { std::int16_t x = 0, y = 0; std::uint16_t radius = 0; std::uint8_t t = 0; };
    struct PFlame { std::int16_t x = 0, y = 0; std::uint8_t t = 0; };
    struct PWall { std::int16_t x = 0, y = 0, w = 0, h = 0; };
    std::uint32_t my_num_ = 0;
    struct Parsed
    {
        std::uint32_t tick = 0, ack = 0, score = 0;
        std::uint8_t phase = 0, optA = 0, optB = 0;
        std::uint16_t wave = 0;
        std::uint8_t match = 0;
        std::vector<PPlayer> players;
        std::vector<PEnemy> enemies;
        std::vector<PBullet> bullets;
        std::vector<PBeam> beams;
        std::vector<PExpl> expl;
        std::vector<PFlame> flames;
        std::vector<PWall> walls;
        std::uint8_t pickTimer = 0;
    };

    void clear_entities()
    {
        for (auto& [id, e] : players_) if (reg_.alive(e)) reg_.destroy(e);
        players_.clear();

        for (auto& kv : enemies_) if (reg_.alive(kv.second.e)) reg_.destroy(kv.second.e);
        enemies_.clear();

        destroy_all(fx_);
        destroy_all(wallEntities_);
        lastWalls_.clear();
        epoch_ = 0;
    }
    // Сначала разбираем весь пакет, и только если он целый, меняем мир.
    static bool parse(const std::uint8_t* d, std::size_t n, Parsed& s)
    {
        proto::Reader r(d, n);
        std::uint8_t type = 0;
        if (!r.u8(type) || type != proto::S2C_SNAPSHOT) return false;

        if (!r.u32(s.tick) || !r.u32(s.ack) || !r.u8(s.match))
            return false;

        if (!r.u8(s.phase) || !r.u8(s.optA) || !r.u8(s.optB) || !r.u8(s.pickTimer))
            return false;

        if (!r.u16(s.wave) || !r.u32(s.score))
            return false;

        std::uint8_t playerCount = 0;
        if (!r.u8(playerCount))
            return false;

        for (std::uint8_t i = 0; i < playerCount; ++i)
        {
            PPlayer p;
            if (!r.u32(p.id) ||
                !r.i16(p.x) ||
                !r.i16(p.y) ||
                !r.u8(p.hp) ||
                !r.u8(p.max) ||
                !r.u8(p.weapon) ||
                !r.u8(p.flags) ||
                !r.u16(p.points))                 // <-- добавлено
                return false;

            p.present = true;
            s.players.push_back(p);
        }

        // дальше без изменений
        std::uint16_t n16 = 0;
        std::uint8_t n8 = 0;

        if (!r.u16(n16)) return false;
        for (std::uint16_t i = 0; i < n16; ++i)
        {
            PEnemy e;
            if (!r.u32(e.id) || !r.u8(e.kind) || !r.i16(e.x) || !r.i16(e.y) || !r.u16(e.hp) || !r.u16(e.max))
                return false;
            s.enemies.push_back(e);
        }

        if (!r.u16(n16)) return false;
        for (std::uint16_t i = 0; i < n16; ++i)
        {
            PBullet b;
            if (!r.u8(b.flags) || !r.i16(b.x) || !r.i16(b.y)) return false;
            s.bullets.push_back(b);
        }

        if (!r.u8(n8)) return false;
        for (std::uint8_t i = 0; i < n8; ++i)
        {
            PBeam b;
            if (!r.i16(b.x1) || !r.i16(b.y1) || !r.i16(b.x2) || !r.i16(b.y2)) return false;
            s.beams.push_back(b);
        }

        if (!r.u8(n8)) return false;
        for (std::uint8_t i = 0; i < n8; ++i)
        {
            PExpl x;
            if (!r.i16(x.x) || !r.i16(x.y) || !r.u16(x.radius) || !r.u8(x.t)) return false;
            s.expl.push_back(x);
        }

        if (!r.u16(n16)) return false;
        for (std::uint16_t i = 0; i < n16; ++i)
        {
            PFlame f;
            if (!r.i16(f.x) || !r.i16(f.y) || !r.u8(f.t)) return false;
            s.flames.push_back(f);
        }

        if (!r.u8(n8)) return false;
        for (std::uint8_t i = 0; i < n8; ++i)
        {
            PWall w;
            if (!r.i16(w.x) || !r.i16(w.y) || !r.i16(w.w) || !r.i16(w.h)) return false;
            s.walls.push_back(w);
        }
        return true;
    }

    bool apply(const std::uint8_t* d, std::size_t n)
    {
        Parsed s;
        if (!parse(d, n, s)) return false;

        // новый матч: старая картинка не нужна
        if (!have_match_ || s.match != match_id_)
        {
            clear_entities();
            match_id_ = s.match;
            have_match_ = true;
        }

        received_ = true;
        tick_ = s.tick;
        ack_ = s.ack;
        phase_ = s.phase;
        optA_ = s.optA;
        optB_ = s.optB;
        data_.wave = s.wave;
        data_.score = static_cast<int>(s.score);

        apply_players(s.players);
        apply_enemies(s.enemies);
        apply_effects(s);
        apply_walls(s.walls);
        return true;
    }

    // Игрок: одна сущность. Нет игрока в снимке значит "погиб": сущность удаляется (как в игре).
    void apply_players(const std::vector<PPlayer>& players)
    {
        std::unordered_set<uint32_t> activeIds;

        for (const PPlayer& p : players)
        {
            if (!p.present)
                continue;

            activeIds.insert(p.id);

            Entity& player = players_[p.id];

            if (!reg_.alive(player))
            {
                player = reg_.create();
                reg_.add<Player>(player);
                reg_.add<Position>(player);
                reg_.add<Health>(player);
                reg_.add<Weapon>(player);
            }

            Position& pos = reg_.get<Position>(player);
            pos.x = p.x;
            pos.y = p.y;

            Health& h = reg_.get<Health>(player);
            h.hp = p.hp;
            h.max = p.max;

            reg_.get<Weapon>(player).type =
                static_cast<WeaponType>(p.weapon);
        }

        for (auto it = players_.begin(); it != players_.end();)
        {
            if (activeIds.find(it->first) == activeIds.end())
            {
                if (reg_.alive(it->second))
                    reg_.destroy(it->second);

                it = players_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Враги: по сетевому id. Есть в снимке значит обновить или создать, нет значит удалить.
    void apply_enemies(const std::vector<PEnemy>& list)
    {
        ++epoch_;
        for (const PEnemy& pe : list)
        {
            auto it = enemies_.find(pe.id);
            if (it == enemies_.end() || !reg_.alive(it->second.e))
            {
                Entity e = reg_.create();
                const EnemyKind kind = static_cast<EnemyKind>(pe.kind);
                reg_.add<Enemy>(e, kind);
                reg_.add<Position>(e);
                reg_.add<Velocity>(e);
                reg_.add<Health>(e);
                if (kind == EnemyKind::Boss)
                {
                    reg_.add<Collider>(e, 40.f, 40.f);   // как WaveSystem создаёт босса
                    reg_.add<BossAI>(e);                  // по нему Hud рисует полосу босса
                }
                else
                {
                    reg_.add<Collider>(e);
                }
                it = enemies_.insert_or_assign(pe.id, Rec{ e, 0 }).first;
            }

            Rec& rec = it->second;
            rec.epoch = epoch_;

            Position& pos = reg_.get<Position>(rec.e);
            pos.x = pe.x;
            pos.y = pe.y;
            Health& h = reg_.get<Health>(rec.e);
            h.hp = pe.hp;
            h.max = pe.max;
        }

        for (auto it = enemies_.begin(); it != enemies_.end();)
        {
            if (it->second.epoch != epoch_)
            {
                if (reg_.alive(it->second.e)) reg_.destroy(it->second.e);
                it = enemies_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Пули, лучи, взрывы и огонь живут доли секунды, id им не нужны:
    // на каждый снимок прошлые удаляются, а новые создаются заново.
    void apply_effects(const Parsed& s)
    {
        destroy_all(fx_);

        for (const PBullet& b : s.bullets)
        {
            Entity e = reg_.create();
            reg_.add<Position>(e, static_cast<float>(b.x), static_cast<float>(b.y));
            reg_.add<Bullet>(e, (b.flags & 1) != 0);
            if (b.flags & 2) reg_.add<Explosive>(e);
            fx_.push_back(e);
        }

        for (const PBeam& b : s.beams)
        {
            Entity e = reg_.create();
            reg_.add<Beam>(e, static_cast<float>(b.x1), static_cast<float>(b.y1),
                static_cast<float>(b.x2), static_cast<float>(b.y2), 0.08f);
            fx_.push_back(e);
        }

        for (const PExpl& x : s.expl)
        {
            Entity e = reg_.create();
            Explosion& ex = reg_.add<Explosion>(e);
            ex.x = x.x;
            ex.y = x.y;
            ex.radius = x.radius;
            ex.maxLife = 1.f;
            ex.life = x.t / 255.f;       // RenderSystem берёт life / maxLife
            fx_.push_back(e);
        }

        for (const PFlame& f : s.flames)
        {
            Entity e = reg_.create();
            reg_.add<Position>(e, static_cast<float>(f.x), static_cast<float>(f.y));
            Flame& fl = reg_.add<Flame>(e);
            fl.maxLife = 1.f;
            fl.life = f.t / 255.f;
            fx_.push_back(e);
        }
    }

    // Стены меняются раз в волну: пересоздаём, только если изменились.
    void apply_walls(const std::vector<PWall>& walls)
    {
        if (same_walls(walls, lastWalls_)) return;

        destroy_all(wallEntities_);
        for (const PWall& w : walls)
        {
            Entity e = reg_.create();
            reg_.add<Position>(e, static_cast<float>(w.x), static_cast<float>(w.y));
            reg_.add<Wall>(e, static_cast<float>(w.w), static_cast<float>(w.h));
            wallEntities_.push_back(e);
        }
        lastWalls_ = walls;
    }

    static bool same_walls(const std::vector<PWall>& a, const std::vector<PWall>& b)
    {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h)
                return false;
        return true;
    }

    void destroy_all(std::vector<Entity>& list)
    {
        for (Entity e : list)
            if (reg_.alive(e)) reg_.destroy(e);
        list.clear();
    }

    struct Rec { Entity e; std::uint32_t epoch = 0; };

    Registry& reg_;
    GameData& data_;

    std::unordered_map<std::uint32_t, Entity> players_;
    std::unordered_map<std::uint32_t, Rec> enemies_;
    std::uint32_t epoch_ = 0;
    std::vector<Entity> fx_;
    std::vector<Entity> wallEntities_;
    std::vector<PWall> lastWalls_;

    std::uint8_t phase_ = 0, optA_ = 0, optB_ = 0;
    std::uint32_t tick_ = 0, ack_ = 0;
    bool received_ = false;

    std::mutex m_;
    std::vector<std::uint8_t> latest_;
    bool has_ = false;

    std::uint8_t match_id_ = 0;
    bool have_match_ = false;
};