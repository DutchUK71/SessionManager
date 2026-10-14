#pragma once

#include "Core/IRuleEngine.h"

namespace SessionEngine
{
    /**
     * @brief Reference Adapter Skeleton for integrating external third-party rule engine libraries.
     */
    class NewRuleEngineAdapter : public IRuleEngine
    {
    public:
        NewRuleEngineAdapter() = default;
        ~NewRuleEngineAdapter() override = default;

        bool LoadModelFromJson(const std::string& modelJson, std::vector<EvaluationDiagnostic>& outDiagnostics) override;
        bool IsModelLoaded() const override;

        RuleEvaluationResult Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes) override;
        RuleEvaluationResult EvaluateAll(ISessionState& sessionState) override;

        std::optional<std::string> ResolveNextScreen(const ISessionState& sessionState, const std::string& directToScreen) override;

    private:
        bool isModelLoaded_ = false;
    };
}
