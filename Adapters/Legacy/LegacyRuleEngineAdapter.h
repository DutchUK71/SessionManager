#pragma once

#include "Core/IRuleEngine.h"
#include "Core/ISessionState.h"
#include "Core/SessionStateContracts.h"
#include "Rules/RuleSet.h"
#include "SessionModelImporter.h"
#include "SessionModelRuleEvaluator.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SessionEngine
{
    class LegacyRuleEngineAdapter : public IRuleEngine
    {
    public:
        LegacyRuleEngineAdapter();
        ~LegacyRuleEngineAdapter() override = default;

        // IRuleEngine implementation
        bool LoadModelFromJson(const std::string& modelJson, std::vector<EvaluationDiagnostic>& outDiagnostics) override;
        bool IsModelLoaded() const override;

        RuleEvaluationResult Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes) override;
        RuleEvaluationResult EvaluateAll(ISessionState& sessionState) override;

        std::optional<std::string> ResolveNextScreen(const ISessionState& sessionState, const std::string& directToScreen) override;

        // Legacy adapter accessors & synchronization helpers
        RuleSet& GetRuleSet();
        const RuleSet& GetRuleSet() const;
        void SetRuleSet(const RuleSet& ruleSet);

        void SyncFromSessionState(const ISessionState& sessionState, RuleSet& ruleSet);
        void SyncToSessionState(const RuleSet& ruleSet, ISessionState& sessionState);

    private:
        void EnsureTemplatePopulated();

        mutable std::mutex mutex_;
        bool isModelLoaded_ = false;
        RuleSet runtimeTemplate_;
        SessionModelImporter::SessionModel sessionModel_;
        std::vector<SessionModelImporter::Diagnostic> modelDiagnostics_;
        SessionModelRuleEvaluator evaluator_;
    };
}
