#pragma once

#include "IRuleEngine.h"
#include <functional>
#include <memory>
#include <string>

namespace SessionEngine
{
    enum class EngineType
    {
        Legacy,
        ExternalLibrary,
        Mock
    };

    class RuleEngineFactory
    {
    public:
        using EngineCreator = std::function<std::unique_ptr<IRuleEngine>()>;

        static std::unique_ptr<IRuleEngine> CreateEngine(EngineType type = EngineType::Legacy);
        static std::unique_ptr<IRuleEngine> CreateEngine(const std::string& typeName);

        static void RegisterEngine(EngineType type, EngineCreator creator);
        static void RegisterEngine(const std::string& typeName, EngineCreator creator);
    };
}
