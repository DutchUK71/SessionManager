#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "SessionStateContracts.h"
#include "ISessionState.h"

namespace SessionEngine
{
    class IRuleEngine
    {
    public:
        virtual ~IRuleEngine() = default;

        // Lifecycle & Model Compilation
        virtual bool LoadModelFromJson(const std::string& modelJson, std::vector<EvaluationDiagnostic>& outDiagnostics) = 0;
        virtual bool IsModelLoaded() const = 0;

        // Inference & Evaluation
        virtual RuleEvaluationResult Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes) = 0;
        virtual RuleEvaluationResult EvaluateAll(ISessionState& sessionState) = 0;

        // Navigation & Flow Resolution
        virtual std::optional<std::string> ResolveNextScreen(const ISessionState& sessionState, const std::string& directToScreen) = 0;
    };
}
