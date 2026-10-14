#include "MockRuleEngineAdapter.h"

namespace SessionEngine
{
    bool MockRuleEngineAdapter::LoadModelFromJson(const std::string& /*modelJson*/, std::vector<EvaluationDiagnostic>& /*outDiagnostics*/)
    {
        isLoaded_ = true;
        return true;
    }

    bool MockRuleEngineAdapter::IsModelLoaded() const
    {
        return isLoaded_;
    }

    RuleEvaluationResult MockRuleEngineAdapter::Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes)
    {
        RuleEvaluationResult result;
        result.success = true;

        for (const auto& change : changes)
        {
            auto inst = sessionState.GetInstance(change.entityId, change.instanceId);
            if (inst)
            {
                inst->SetAttributeValue(change.attributeId, change.rawValue);
            }
        }

        return result;
    }

    RuleEvaluationResult MockRuleEngineAdapter::EvaluateAll(ISessionState& /*sessionState*/)
    {
        RuleEvaluationResult result;
        result.success = true;
        return result;
    }

    std::optional<std::string> MockRuleEngineAdapter::ResolveNextScreen(const ISessionState& /*sessionState*/, const std::string& directToScreen)
    {
        if (!directToScreen.empty())
        {
            return directToScreen;
        }
        return std::nullopt;
    }
}
