#pragma once

#include "Core/IRuleEngine.h"

namespace SessionEngine
{
    class MockRuleEngineAdapter : public IRuleEngine
    {
    public:
        MockRuleEngineAdapter() = default;
        ~MockRuleEngineAdapter() override = default;

        bool LoadModelFromJson(const std::string& modelJson, std::vector<EvaluationDiagnostic>& outDiagnostics) override;
        bool IsModelLoaded() const override;

        RuleEvaluationResult Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes) override;
        RuleEvaluationResult EvaluateAll(ISessionState& sessionState) override;

        std::optional<std::string> ResolveNextScreen(const ISessionState& sessionState, const std::string& directToScreen) override;

    private:
        bool isLoaded_ = true;
    };
}
