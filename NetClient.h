#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include "Components.h"      // InputState
#include "Protocol.h"        // тот же файл, что на сервере (net/Protocol.h)
#include "WorldMirror.h"

// Сетевой слой клиента. Живёт в СВОЁМ потоке и ничего не знает про окно и игру.
//
//   главный поток                         сетевой поток (Asio)
//   -------------                         --------------------
//   send_input(InputState)  ───────────►  раз в тик отправляет ввод
//   state(), status(), ...  ◄───────────  состояние соединения (атомики)
//   mirror.apply_latest()   ◄───────────  mirror.push(снимок)  (только копирует байты)
//
// Правило: Registry и всё игровое трогает только главный поток. Отсюда в мир ничего не пишется,
// снимки лишь кладутся в WorldMirror::push().
//
// Жизненный цикл: Connecting -> Handshaking (TLS, Hello) -> Playing.
// Обрыв или тишина: Reconnecting (растущая пауза). Неверный токен: Failed (без повторов).
// Все надписи на английском, чтобы SFML показал их без кодировок.

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

class NetClient
{
public:
    enum class State : int { Idle, Connecting, Handshaking, Playing, Reconnecting, Failed };
    void cmd_ready(bool v) { post_cmd({ proto::C2S_SET_READY, std::uint8_t(v ? 1 : 0) }); }
    void cmd_start() { post_cmd({ proto::C2S_START }); }
    void cmd_kick(std::uint8_t num) { post_cmd({ proto::C2S_KICK, num }); }
    void cmd_leave() { post_cmd({ proto::C2S_LEAVE_PARTY }); }
    struct Config
    {
        std::string host = "localhost";
        std::string port = "8765";
        std::string party_code;
        bool verify_tls = false;           // ДЛЯ РАЗРАБОТКИ: у сервера самоподписанный сертификат. В релизе включить.
        std::string ca_file;               // свой корневой сертификат (если verify_tls)
        int connect_timeout_ms = 10000;    // на всё: TCP + TLS + Welcome
        int silence_timeout_ms = 5000;     // сервер молчит дольше: связь считаем мёртвой
        int reconnect_max_ms = 15000;      // максимум паузы между попытками
    };
    struct PartyMember { std::uint8_t num = 0; bool ready = false; bool offline = false; std::string name; };

    struct PartyInfo
    {
        bool in_party = false;
        bool resumed = false;          // <-- новое
        std::uint8_t state = 0;
        std::uint8_t countdown = 0;
        std::uint8_t leader = 0;
        std::uint8_t my_num = 0;
        std::string code;
        std::vector<PartyMember> members;
    };

    struct MatchResult
    {
        bool valid = false;
        std::uint16_t wave = 0;
        std::uint32_t score = 0;
        std::vector<std::pair<std::uint8_t, std::uint16_t>> points;   // номер, очки
    };

    PartyInfo party() const { std::lock_guard<std::mutex> lk(party_m_); return party_; }
    MatchResult results() const { std::lock_guard<std::mutex> lk(party_m_); return results_; }

    // true один раз: сцена должна вызвать mirror.clear()
    bool take_mirror_reset() { return mirror_reset_.exchange(false); }

    // 0 = ничего, иначе причина PARTY_LEFT (2 = выгнали); читается один раз
    std::uint8_t take_left_reason() { return left_reason_.exchange(0); }

    NetClient(Config cfg, WorldMirror& mirror)
        : cfg_(std::move(cfg)), mirror_(mirror), ssl_ctx_(ssl::context::tls_client),
        resolver_(io_), connect_timer_(io_), input_timer_(io_), ping_timer_(io_),
        watchdog_timer_(io_), retry_timer_(io_)
    {
        if (cfg_.verify_tls)
        {
            ssl_ctx_.set_verify_mode(ssl::verify_peer);
            if (!cfg_.ca_file.empty()) ssl_ctx_.load_verify_file(cfg_.ca_file);
            else                       ssl_ctx_.set_default_verify_paths();
        }
        else
        {
            ssl_ctx_.set_verify_mode(ssl::verify_none);
        }
    }

    ~NetClient() { stop(); }

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // ======== вызывается из главного потока ========

    void set_token(std::string t)
    {
        std::lock_guard<std::mutex> lk(tok_m_);
        token_ = std::move(t);
    }

    void start()
    {
        if (thread_.joinable()) return;
        want_stop_ = false;
        fatal_ = false;
        attempt_ = 0;
        io_.restart();
        guard_ = std::make_unique<Guard>(asio::make_work_guard(io_));
        asio::post(io_, [this] { begin_connect(); });
        thread_ = std::thread([this] { io_.run(); });

        if (!console_started_)
        {
            console_started_ = true;
            std::thread([this]
                {
                    std::string line;
                    while (std::getline(std::cin, line))
                    {
                        try
                        {
                            if (line == "r")       cmd_ready(true);
                            else if (line == "u")  cmd_ready(false);
                            else if (line == "s")  cmd_start();
                            else if (line == "l")  cmd_leave();
                            else if (line.size() > 2 && line[0] == 'k')
                                cmd_kick(static_cast<std::uint8_t>(std::stoi(line.substr(2))));
                        }
                        catch (...) {}      // кривой ввод вроде "k abc" не должен ронять клиент
                    }
                }).detach();
        }
    }

    void stop()
    {
        if (!thread_.joinable()) return;
        want_stop_ = true;
        asio::post(io_, [this]
            {
                ++gen_;                    // отменяет все ожидающие обработчики
                resolver_.cancel();
                close_connection();
                cancel_all_timers();
            });
        guard_.reset();                    // io_.run() завершится, когда обработчики отработают
        thread_.join();
        set_state(State::Idle, "Disconnected");
    }

    // Ручной повтор (например, после Failed, когда игрок снова вошёл и получил новый токен)
    void reconnect_now()
    {
        if (!thread_.joinable()) { start(); return; }
        asio::post(io_, [this]
            {
                fatal_ = false;
                attempt_ = 0;
                ++gen_;
                retry_timer_.cancel();
                close_connection();
                cancel_session_timers();
                begin_connect();
            });
    }

    // Вызывать КАЖДЫЙ кадр с текущим вводом. Нажатия, которые длились меньше тика сервера
    // (например, одно нажатие клавиши 1), копятся и не теряются.
    void send_input(const InputState& in)
    {
        std::lock_guard<std::mutex> lk(in_m_);
        latest_ = in;
        acc_flags_ |= pack_flags(in);
        acc_nums_ |= pack_nums(in);
    }

    State state() const { return state_.load(); }
    bool playing() const { return state_.load() == State::Playing; }
    std::uint8_t kick_reason() const { return kick_.load(); }      // 0, если сервер не отключал
    int rtt_ms() const { return rtt_.load(); }                      // -1, пока не измерен
    std::uint32_t player_id() const { return player_id_.load(); }
    int arena_w() const { return arena_w_.load(); }
    int arena_h() const { return arena_h_.load(); }

    std::string status() const
    {
        std::lock_guard<std::mutex> lk(status_m_);
        return status_;
    }

    // true один раз после того, как игровая сессия оборвалась: сцена должна очистить зеркало мира
    bool take_dropped() { return dropped_.exchange(false); }

private:
    using clock = std::chrono::steady_clock;
    using Guard = asio::executor_work_guard<asio::io_context::executor_type>;
    bool console_started_ = false;
    void post_cmd(std::vector<std::uint8_t> payload)
    {
        asio::post(io_, [this, payload = std::move(payload)]
            {
                if (conn_ && state_.load() == State::Playing) send_frame(conn_, payload);
            });
    }
    struct Conn
    {
        Conn(asio::io_context& io, ssl::context& ctx) : stream(io, ctx) {}

        ssl::stream<tcp::socket> stream;
        std::array<std::uint8_t, 4> hdr{};
        std::vector<std::uint8_t> body;
        std::deque<std::shared_ptr<std::vector<std::uint8_t>>> out;
        bool writing = false;
        bool closed = false;
    };

    // ======== всё ниже работает только в сетевом потоке ========

    void begin_connect()
    {
        if (want_stop_) return;

        cancel_session_timers();
        close_connection();

        std::string token;
        {
            std::lock_guard<std::mutex> lk(tok_m_);
            token = token_;
        }
        if (token.empty())
        {
            fatal_ = true;
            set_state(State::Failed, "No token: sign in first");
            return;
        }

        auto c = std::make_shared<Conn>(io_, ssl_ctx_);
        conn_ = c;
        const std::uint64_t gen = ++gen_;

        set_state(attempt_ == 0 ? State::Connecting : State::Reconnecting, "Connecting...");

        if (cfg_.verify_tls)
            c->stream.set_verify_callback(ssl::host_name_verification(cfg_.host));
        SSL_set_tlsext_host_name(c->stream.native_handle(), cfg_.host.c_str());

        // Общий дедлайн на подключение: TCP + TLS + Welcome
        connect_timer_.expires_after(std::chrono::milliseconds(cfg_.connect_timeout_ms));
        connect_timer_.async_wait([this, gen](const boost::system::error_code& ec)
            {
                if (ec || gen != gen_) return;
                on_lost(gen, "Connection timed out", "connect timeout");
            });

        resolver_.async_resolve(cfg_.host, cfg_.port,
            [this, c, gen, token](const boost::system::error_code& ec, tcp::resolver::results_type results)
            {
                if (gen != gen_) return;
                if (ec) { on_lost(gen, "Server address not found", ec.message()); return; }

                asio::async_connect(c->stream.next_layer(), results,
                    [this, c, gen, token](const boost::system::error_code& ec2, const tcp::endpoint&)
                    {
                        if (gen != gen_) return;
                        if (ec2) std::cerr << "[net] connect ec=" << ec2.value()
                            << " host=[" << cfg_.host << "] port=[" << cfg_.port << "]" << std::endl;
                        if (ec2) { on_lost(gen, "Cannot reach server", ec2.message()); return; }

                        boost::system::error_code nec;
                        c->stream.next_layer().set_option(tcp::no_delay(true), nec);   // не склеивать мелкие пакеты

                        set_state(State::Handshaking, "Secure handshake...");
                        c->stream.async_handshake(ssl::stream_base::client,
                            [this, c, gen, token](const boost::system::error_code& ec3)
                            {
                                if (gen != gen_) return;
                                if (ec3) { on_lost(gen, "Secure handshake failed", ec3.message()); return; }

                                last_rx_ = clock::now();

                                std::vector<std::uint8_t> hello;       // Hello: [тип][токен]
                                hello.push_back(proto::C2S_HELLO);
                                hello.push_back(proto::PROTOCOL_VERSION);
                                hello.insert(hello.end(), token.begin(), token.end());
                                send_frame(c, hello);

                                set_state(State::Handshaking, "Signing in...");
                                read_header(c, gen);
                            });
                    });
            });
    }

    // ---------- чтение ----------

    void read_header(const std::shared_ptr<Conn>& c, std::uint64_t gen)
    {
        asio::async_read(c->stream, asio::buffer(c->hdr),
            [this, c, gen](const boost::system::error_code& ec, std::size_t)
            {
                if (gen != gen_) return;
                if (ec) { on_lost(gen, "Connection lost", ec.message()); return; }

                last_rx_ = clock::now();

                const std::uint32_t len =
                    (std::uint32_t(c->hdr[0]) << 24) | (std::uint32_t(c->hdr[1]) << 16) |
                    (std::uint32_t(c->hdr[2]) << 8) | std::uint32_t(c->hdr[3]);

                if (len == 0) { read_header(c, gen); return; }         // heartbeat сервера

                if (len > proto::MAX_PACKET)
                {
                    auto pc = [](std::uint8_t b) { return (b >= 32 && b < 127) ? static_cast<char>(b) : '.'; };
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "frame too large: len=%u, bytes=%02X %02X %02X %02X  '%c%c%c%c'",
                        len, c->hdr[0], c->hdr[1], c->hdr[2], c->hdr[3],
                        pc(c->hdr[0]), pc(c->hdr[1]), pc(c->hdr[2]), pc(c->hdr[3]));
                    on_lost(gen, "Bad data from server", buf);
                    return;
                }

                c->body.resize(len);
                read_body(c, gen);
            });
    }

    void read_body(const std::shared_ptr<Conn>& c, std::uint64_t gen)
    {
        asio::async_read(c->stream, asio::buffer(c->body),
            [this, c, gen](const boost::system::error_code& ec, std::size_t)
            {
                if (gen != gen_) return;
                if (ec) { on_lost(gen, "Connection lost", ec.message()); return; }

                last_rx_ = clock::now();
                handle_packet(*c, gen);
                if (gen == gen_) read_header(c, gen);                  // handle_packet мог оборвать связь
            });
    }

    void handle_packet(Conn& c, std::uint64_t gen)
    {
        const std::vector<std::uint8_t>& p = c.body;
        switch (p[0])
        {
        case proto::S2C_WELCOME:
        {
            proto::Reader r(p.data(), p.size());
            std::uint8_t type = 0, ver = 0, rate = 0, inParty = 0;
            std::uint32_t id = 0;
            if (!r.u8(type) || !r.u8(ver) || !r.u32(id) || !r.u8(rate) || !r.u8(inParty)
                || rate == 0 || inParty > 1 || ver != proto::PROTOCOL_VERSION || r.left() != 0)
            {
                fatal_ = true;
                on_lost(gen, "Update the game", "version mismatch");
                return;
            }
            player_id_ = id;
            tick_rate_ = rate;
            attempt_ = 0;
            kick_ = 0;
            connect_timer_.cancel();
            set_state(State::Playing, "In game");
            start_session_timers(gen);

            if (!inParty)                       // вернули в пати: создавать и входить не надо
            {
                std::vector<std::uint8_t> out;
                const std::string& code = cfg_.party_code;
                if (code.empty())
                    out = { proto::C2S_CREATE_PARTY };
                else
                {
                    out.push_back(proto::C2S_JOIN_PARTY);
                    out.insert(out.end(), code.begin(), code.end());
                }
                send_frame(conn_, out);
            }
            break;
        }
        case proto::S2C_SNAPSHOT:
            if (state_.load() == State::Playing)
                mirror_.push(std::vector<std::uint8_t>(p.begin(), p.end()));   // только копия байтов
            break;

        case proto::S2C_PONG:
            if (ping_sent_ != clock::time_point{})
                rtt_ = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    clock::now() - ping_sent_).count());
            break;

        case proto::S2C_KICK:
        {
            const std::uint8_t reason = p.size() >= 2 ? p[1] : 0;
            kick_ = reason;
            if (reason == KICK_AUTH || reason == KICK_VERSION || KICK_REPLACED) fatal_ = true;                 // повторять с тем же токеном бессмысленно
            on_lost(gen, kick_text(reason), "kicked by server");
            break;
        }
        case proto::S2C_PARTY_JOINED:
        {
            proto::PartyJoinedMsg m;
            if (!proto::decode(p.data(), p.size(), m))
            {
                on_lost(gen, "Bad data from server", "bad party joined"); return;
            }
            {
                std::lock_guard<std::mutex> lk(party_m_);
                party_ = PartyInfo{};
                party_.in_party = true;
                party_.my_num = m.num;
                party_.code = m.code;
                party_.resumed = m.resumed;
                results_ = MatchResult{};
            }
            std::cout << "PARTY CODE: " << m.code << "\n";
            break;
        }
        case proto::S2C_PARTY_UPDATE:
        {
            proto::PartyUpdateMsg m;
            if (!proto::decode(p.data(), p.size(), m))
            {
                on_lost(gen, "Bad data from server", "bad party update"); return;
            }
            std::lock_guard<std::mutex> lk(party_m_);
            party_.state = m.state;
            party_.countdown = m.countdown;
            party_.leader = m.leader;
            party_.members.clear();
            for (auto& x : m.members)
            {
                PartyMember pm;
                pm.num = x.num;
                pm.ready = x.ready;
                pm.offline = x.offline;
                pm.name = x.name;
                party_.members.push_back(std::move(pm));
            }
            std::cout << "[party] state=" << int(party_.state) << " countdown=" << int(party_.countdown)
                << " members=" << party_.members.size() << std::endl;
            break;
        }

        case proto::S2C_GAME_STARTED:
        {
            proto::GameStartedMsg m;
            if (!proto::decode(p.data(), p.size(), m))
            {
                on_lost(gen, "Bad data from server", "bad game started"); return;
            }
            arena_w_ = m.arena_w;
            arena_h_ = m.arena_h;
            mirror_reset_ = true;
            break;
        }

        case proto::S2C_GAME_ENDED:
        {
            proto::GameEndedMsg m;
            if (!proto::decode(p.data(), p.size(), m))
            {
                on_lost(gen, "Bad data from server", "bad game ended");
                return;
            }

            MatchResult res;
            res.valid = true;
            res.wave = m.wave;
            res.score = m.score;
            res.points = std::move(m.points);

            {
                std::lock_guard<std::mutex> lk(party_m_);
                results_ = std::move(res);
            }
            mirror_reset_ = true;
            break;
        }

        case proto::S2C_PARTY_LEFT:
        {
            const std::uint8_t reason = p.size() >= 2 ? p[1] : 0;
            {
                std::lock_guard<std::mutex> lk(party_m_);
                party_ = PartyInfo{};             // in_party = false, значит, меню
            }
            left_reason_ = static_cast<std::uint8_t>(reason + 1);
            mirror_reset_ = true;
            break;
        }

        case proto::S2C_ERROR:
            last_error_ = p.size() > 1 ? p[1] : 0;
            std::cout << "[party] error " << int(last_error_.load()) << std::endl;
            break;
        default:
            break;                                                     // неизвестный тип: игнорируем
        }
    }

    // ---------- запись ----------

    void send_frame(const std::shared_ptr<Conn>& c, const std::vector<std::uint8_t>& payload)
    {
        if (!c || c->closed) return;
        if (c->out.size() > 64) return;            // сервер не читает: не копим, разберётся таймер тишины

        auto f = std::make_shared<std::vector<std::uint8_t>>();
        f->reserve(payload.size() + 4);
        const std::uint32_t n = static_cast<std::uint32_t>(payload.size());
        f->push_back(static_cast<std::uint8_t>((n >> 24) & 0xFF));
        f->push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
        f->push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        f->push_back(static_cast<std::uint8_t>(n & 0xFF));
        f->insert(f->end(), payload.begin(), payload.end());

        c->out.push_back(std::move(f));
        if (!c->writing) write_next(c, gen_);
    }

    void write_next(const std::shared_ptr<Conn>& c, std::uint64_t gen)
    {
        c->writing = true;
        auto f = c->out.front();
        asio::async_write(c->stream, asio::buffer(*f),
            [this, c, gen, f](const boost::system::error_code& ec, std::size_t)
            {
                if (gen != gen_) return;
                if (ec) { on_lost(gen, "Connection lost", ec.message()); return; }

                c->out.pop_front();
                if (c->out.empty()) c->writing = false;
                else                write_next(c, gen);
            });
    }

    // ---------- таймеры игровой сессии ----------

    void start_session_timers(std::uint64_t gen)
    {
        tick_period_ = std::chrono::nanoseconds(1'000'000'000 / std::max(1, tick_rate_));
        next_input_ = clock::now();
        arm_input(gen);
        arm_ping(gen);
        arm_watchdog(gen);
    }

    // Ввод уходит с частотой тика сервера (не с частотой кадров): сервер берёт одну команду за тик
    void arm_input(std::uint64_t gen)
    {
        next_input_ += tick_period_;
        const auto now = clock::now();
        if (next_input_ < now) next_input_ = now;                      // отстали: не догоняем пачкой

        input_timer_.expires_at(next_input_);
        input_timer_.async_wait([this, gen](const boost::system::error_code& ec)
            {
                if (ec || gen != gen_ || state_.load() != State::Playing) return;
                send_input_packet();
                arm_input(gen);
            });
    }

    void send_input_packet()
    {
        InputState cur;
        std::uint8_t flags = 0, nums = 0;
        {
            std::lock_guard<std::mutex> lk(in_m_);
            cur = latest_;
            flags = static_cast<std::uint8_t>(acc_flags_ | pack_flags(latest_));
            nums = static_cast<std::uint8_t>(acc_nums_ | pack_nums(latest_));
            acc_flags_ = 0;
            acc_nums_ = 0;
        }

        proto::Writer w;
        w.u8(proto::C2S_INPUT);
        w.u32(++seq_);
        w.u8(flags);
        w.u8(nums);
        w.i16(static_cast<std::int16_t>(std::clamp(cur.aimX, -32768.f, 32767.f)));
        w.i16(static_cast<std::int16_t>(std::clamp(cur.aimY, -32768.f, 32767.f)));
        send_frame(conn_, w.b);
    }

    void arm_ping(std::uint64_t gen)
    {
        ping_timer_.expires_after(std::chrono::seconds(2));
        ping_timer_.async_wait([this, gen](const boost::system::error_code& ec)
            {
                if (ec || gen != gen_ || state_.load() != State::Playing) return;
                ping_sent_ = clock::now();
                send_frame(conn_, std::vector<std::uint8_t>{ proto::C2S_PING });
                arm_ping(gen);
            });
    }

    // Сервер шлёт снимки 30 раз в секунду: долгая тишина значит, что связь мертва
    void arm_watchdog(std::uint64_t gen)
    {
        watchdog_timer_.expires_after(std::chrono::milliseconds(500));
        watchdog_timer_.async_wait([this, gen](const boost::system::error_code& ec)
            {
                if (ec || gen != gen_) return;
                const auto silent = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - last_rx_);
                if (silent.count() > cfg_.silence_timeout_ms)
                {
                    on_lost(gen, "No data from server", "silence timeout");
                    return;
                }
                arm_watchdog(gen);
            });
    }

    // ---------- потеря связи и переподключение ----------

    // Единственное место, где соединение считается потерянным
    void on_lost(std::uint64_t gen, const std::string& why, const std::string& details)
    {
        if (gen != gen_) return;                   // это уже старое соединение
        ++gen_;                                    // отменяет все ожидающие обработчики этого соединения

        if (!details.empty()) std::cerr << "[net] " << why << ": " << details << std::endl;

        if (state_.load() == State::Playing) dropped_ = true;

        {
            std::lock_guard<std::mutex> lk(party_m_);
            party_ = PartyInfo{};            // после обрыва в пати мы уже не состоим
        }
        mirror_reset_ = true;

        close_connection();
        cancel_session_timers();
        connect_timer_.cancel();

        if (want_stop_) return;

        if (fatal_) { set_state(State::Failed, why); return; }

        schedule_reconnect(why);
    }

    void schedule_reconnect(const std::string& why)
    {
        ++attempt_;
        const int base = std::min(cfg_.reconnect_max_ms, 1000 * (1 << std::min(attempt_ - 1, 4)));   // 1, 2, 4, 8, 16 с
        const int jitter = std::uniform_int_distribution<int>(0, base / 4)(rng_);                       // чтобы клиенты не приходили разом
        const int wait_ms = base + jitter;

        set_state(State::Reconnecting, why + ". Retry in " + std::to_string((wait_ms + 999) / 1000) +
            " s (attempt " + std::to_string(attempt_) + ")");

        retry_timer_.expires_after(std::chrono::milliseconds(wait_ms));
        retry_timer_.async_wait([this](const boost::system::error_code& ec)
            {
                if (!ec && !want_stop_) begin_connect();
            });
    }

    void close_connection()
    {
        if (!conn_) return;
        conn_->closed = true;
        boost::system::error_code ec;
        conn_->stream.next_layer().close(ec);
        conn_.reset();
    }

    void cancel_session_timers()
    {
        input_timer_.cancel();
        ping_timer_.cancel();
        watchdog_timer_.cancel();
    }

    void cancel_all_timers()
    {
        cancel_session_timers();
        connect_timer_.cancel();
        retry_timer_.cancel();
    }

    void set_state(State s, std::string text)
    {
        {
            std::lock_guard<std::mutex> lk(status_m_);
            status_ = std::move(text);
        }
        state_ = s;
    }

    // ---------- мелочи ----------

    static constexpr std::uint8_t KICK_AUTH = 2;       // коды причин как в GameNet.h на сервере
    static constexpr std::uint8_t KICK_VERSION = 5;
    static constexpr std::uint8_t KICK_REPLACED = 6;
    static std::string kick_text(std::uint8_t r)
    {
        switch (r)
        {
        case 1:  return "Server rejected the connection (protocol error)";
        case 2:  return "Invalid or expired token: sign in again";
        case 3:  return "Server is busy";
        case 4:  return "Disconnected by server (duplicate login, idle or no free slots)";
        default: return "Disconnected by server";
        }
    }

    static std::uint8_t pack_flags(const InputState& in)
    {
        std::uint8_t f = 0;
        if (in.left)    f |= proto::IN_LEFT;
        if (in.right)   f |= proto::IN_RIGHT;
        if (in.up)      f |= proto::IN_UP;
        if (in.down)    f |= proto::IN_DOWN;
        if (in.shoot)   f |= proto::IN_SHOOT;
        if (in.restart) f |= proto::IN_RESTART;
        return f;
    }

    static std::uint8_t pack_nums(const InputState& in)
    {
        std::uint8_t n = 0;
        if (in.num1) n |= 1;
        if (in.num2) n |= 2;
        if (in.num3) n |= 4;
        if (in.num4) n |= 8;
        if (in.num5) n |= 16;
        return n;
    }

    // ---------- данные ----------

    Config cfg_;
    WorldMirror& mirror_;

    asio::io_context io_;                      // io_ объявлен раньше всего, что на нём работает
    ssl::context ssl_ctx_;
    tcp::resolver resolver_;
    asio::steady_timer connect_timer_, input_timer_, ping_timer_, watchdog_timer_, retry_timer_;
    std::unique_ptr<Guard> guard_;
    std::thread thread_;

    // только сетевой поток
    std::shared_ptr<Conn> conn_;
    std::uint64_t gen_ = 0;
    int attempt_ = 0;
    bool fatal_ = false;
    int tick_rate_ = 30;
    std::uint32_t seq_ = 0;
    clock::time_point last_rx_{}, next_input_{}, ping_sent_{};
    clock::duration tick_period_{};
    std::mt19937 rng_{ std::random_device{}() };

    // из любых потоков
    std::atomic<bool> want_stop_{ false };
    std::atomic<State> state_{ State::Idle };
    std::atomic<bool> dropped_{ false };
    std::atomic<std::uint8_t> kick_{ 0 };
    std::atomic<int> rtt_{ -1 };
    std::atomic<std::uint32_t> player_id_{ 0 };
    std::atomic<int> arena_w_{ 800 };
    std::atomic<int> arena_h_{ 600 };

    mutable std::mutex status_m_;
    std::string status_ = "Idle";

    std::mutex tok_m_;
    std::string token_;

    std::mutex in_m_;
    InputState latest_;
    std::uint8_t acc_flags_ = 0;
    std::uint8_t acc_nums_ = 0;
    mutable std::mutex party_m_;
    PartyInfo party_;
    MatchResult results_;
    std::atomic<bool> mirror_reset_{ false };
    std::atomic<std::uint8_t> left_reason_{ 0 };
    std::atomic<std::uint8_t> last_error_{ 0 };
};