#include "NewRuleEngineAdapter.h"

namespace SessionEngine
{
    bool NewRuleEngineAdapter::LoadModelFromJson(const std::string& /*modelJson*/, std::vector<EvaluationDiagnostic>& /*outDiagnostics*/)
    {
        // Integration point: compile modelJson into third-party rule engine AST / Rete network
        isModelLoaded_ = true;
        return true;
    }

    bool NewRuleEngineAdapter::IsModelLoaded() const
    {
        return isModelLoaded_;
    }

    RuleEvaluationResult NewRuleEngineAdapter::Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes)
    {
        RuleEvaluationResult result;
        result.success = true;

        // Integration point: feed changes into third-party engine working memory
        for (const auto& change : changes)
        {
            auto inst = sessionState.GetInstance(change.entityId, change.instanceId);
            if (inst)
            {
                inst->SetAttributeValue(change.attributeId, change.rawValue);
            }
        }

        // Integration point: fire rules and extract inferred facts back to sessionState

        return result;
    }

    RuleEvaluationResult NewRuleEngineAdapter::EvaluateAll(ISessionState& /*sessionState*/)
    {
        RuleEvaluationResult result;
        result.success = true;
        // Integration point: full evaluation
        return result;
    }

    std::optional<std::string> NewRuleEngineAdapter::ResolveNextScreen(const ISessionState& /*sessionState*/, const std::string& directToScreen)
    {
        if (!directToScreen.empty())
        {
            return directToScreen;
        }
        return std::nullopt;
    }
}
