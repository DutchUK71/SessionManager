#include "RuleEngineFactory.h"
#include <mutex>
#include <unordered_map>

namespace SessionEngine
{
    namespace
    {
        std::mutex g_registryMutex;
        std::unordered_map<std::string, RuleEngineFactory::EngineCreator> g_creators;

        std::string ToString(EngineType type)
        {
            switch (type)
            {
                case EngineType::Legacy: return "Legacy";
                case EngineType::ExternalLibrary: return "ExternalLibrary";
                case EngineType::Mock: return "Mock";
            }
            return "Legacy";
        }
    }

    std::unique_ptr<IRuleEngine> RuleEngineFactory::CreateEngine(EngineType type)
    {
        return CreateEngine(ToString(type));
    }

    std::unique_ptr<IRuleEngine> RuleEngineFactory::CreateEngine(const std::string& typeName)
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        auto it = g_creators.find(typeName);
        if (it != g_creators.end())
        {
            return it->second();
        }
        return nullptr;
    }

    void RuleEngineFactory::RegisterEngine(EngineType type, EngineCreator creator)
    {
        RegisterEngine(ToString(type), std::move(creator));
    }

    void RuleEngineFactory::RegisterEngine(const std::string& typeName, EngineCreator creator)
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        g_creators[typeName] = std::move(creator);
    }
}
