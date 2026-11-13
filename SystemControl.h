#pragma once
#include <vector>
#include <memory>
#include "System.h"

enum class SystemType
{
    Input,
    Logic,
    Physics,
    Render
};
class ModuleRegistry
{
private:
    std::vector<std::shared_ptr<System>> modules;
    ThreadPool pool{ 4 }; 
    InputState input;
    World& world;

public:
    ModuleRegistry(World& w) : world(w) {}
    std::vector <std::shared_ptr<System>>& get_modules()
    {
        return modules;
    }
    void SetInput(const InputState& in)
    {
        input = in;

        for (auto& m : modules)
            m->SetInput(in);
    }
    void push_module(std::shared_ptr<System> module)
    {
        modules.emplace_back(std::move(module));
    }

    void Init()
    {
        for (auto& m : modules)
            m->Init();
    }

    void Start()
    {
        for (auto& m : modules)
            m->Start();
    }

    void Update(float dt)
    {
        int threadCount = pool.thread_count();

        std::vector<CommandBuffer> buffers(threadCount);

        // 1️⃣ Input (один поток)
        for (auto& m : modules)
        {
            if (m->GetType() == SystemType::Input)
                m->Update(dt, pool, buffers[0]);
        }

        // 2️⃣ Параллельные системы
        for (int i = 0; i < modules.size(); i++)
        {
            auto& m = modules[i];

            if (m->GetType() == SystemType::Logic)
            {
                pool.submit(0, [&, i]()
                    {
                        int tid = i % buffers.size(); // 👈 БЕЗОПАСНО
                        m->Update(dt, pool, buffers[tid]);
                    });
            }
        }

        pool.wait();

        // 3️⃣ Merge
        CommandBuffer final;

        for (auto& b : buffers)
        {
            for (auto& c : b.commands)
                final.commands.push_back(c);
        }

        // 4️⃣ Применение
        final.apply(world);

        // 5️⃣ Очистка
        world.cleanup();
    }
    void Render(float dt)
    {
        for (auto& m : modules)
            m->Render(dt);
    }

    void Stop()
    {
        for (auto& m : modules)
            m->Stop();
    }
};

