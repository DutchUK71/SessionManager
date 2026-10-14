#include "LegacyRuleEngineAdapter.h"
#include "json.hpp"
#include <algorithm>

namespace SessionEngine
{
    LegacyRuleEngineAdapter::LegacyRuleEngineAdapter()
    {
    }

    bool LegacyRuleEngineAdapter::LoadModelFromJson(const std::string& modelJson, std::vector<EvaluationDiagnostic>& outDiagnostics)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try
        {
            nlohmann::json root = nlohmann::json::parse(modelJson);
            SessionModelImporter importer;
            auto importResult = importer.LoadJson(root);

            for (const auto& d : importResult.diagnostics)
            {
                EvaluationDiagnostic diag;
                diag.code = d.code;
                diag.message = d.message;
                diag.location = d.context;
                diag.severity = (d.severity == SessionModelImporter::DiagnosticSeverity::Error)
                    ? EvaluationDiagnostic::Severity::Error
                    : (d.severity == SessionModelImporter::DiagnosticSeverity::Warning)
                    ? EvaluationDiagnostic::Severity::Warning
                    : EvaluationDiagnostic::Severity::Info;
                outDiagnostics.push_back(diag);
            }

            modelDiagnostics_ = importResult.diagnostics;

            if (importResult.HasErrors())
            {
                isModelLoaded_ = false;
                return false;
            }

            sessionModel_ = importResult.model;
            auto convResult = importer.ConvertToRuntimeRuleSet(sessionModel_);

            for (const auto& d : convResult.diagnostics)
            {
                EvaluationDiagnostic diag;
                diag.code = d.code;
                diag.message = d.message;
                diag.location = d.context;
                diag.severity = (d.severity == SessionModelImporter::DiagnosticSeverity::Error)
                    ? EvaluationDiagnostic::Severity::Error
                    : (d.severity == SessionModelImporter::DiagnosticSeverity::Warning)
                    ? EvaluationDiagnostic::Severity::Warning
                    : EvaluationDiagnostic::Severity::Info;
                outDiagnostics.push_back(diag);
            }

            if (convResult.HasErrors())
            {
                isModelLoaded_ = false;
                return false;
            }

            runtimeTemplate_ = convResult.ruleSet;
            isModelLoaded_ = true;
            return true;
        }
        catch (const std::exception& e)
        {
            EvaluationDiagnostic diag;
            diag.severity = EvaluationDiagnostic::Severity::Error;
            diag.code = "json_parse_error";
            diag.message = e.what();
            outDiagnostics.push_back(diag);
            isModelLoaded_ = false;
            return false;
        }
    }

    bool LegacyRuleEngineAdapter::IsModelLoaded() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return isModelLoaded_;
    }

    RuleSet& LegacyRuleEngineAdapter::GetRuleSet()
    {
        return runtimeTemplate_;
    }

    const RuleSet& LegacyRuleEngineAdapter::GetRuleSet() const
    {
        return runtimeTemplate_;
    }

    void LegacyRuleEngineAdapter::SetRuleSet(const RuleSet& ruleSet)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimeTemplate_ = ruleSet;
        isModelLoaded_ = true;
    }

    void LegacyRuleEngineAdapter::SyncFromSessionState(const ISessionState& sessionState, RuleSet& ruleSet)
    {
        if (ruleSet.entities == nullptr)
        {
            return;
        }

        for (auto& entity : *ruleSet.entities)
        {
            if (entity == nullptr) continue;

            const std::string& entityId = entity->entityid;
            auto stateInstances = sessionState.GetAllInstances(entityId);

            // Reconcile instances
            std::vector<std::shared_ptr<Instance>> updatedInstances;

            for (const auto& stateInst : stateInstances)
            {
                std::string instId = stateInst->GetInstanceId();
                std::shared_ptr<Instance> inst = entity->GetInstanceById(instId);

                if (inst == nullptr)
                {
                    inst = entity->GetCleanInstance();
                    inst->instanceId = instId;
                    inst->entityId = entityId;
                }

                // Update attributes
                for (auto& att : inst->attributes)
                {
                    if (att == nullptr) continue;
                    auto valOpt = stateInst->GetAttributeValue(att->GetID());
                    if (valOpt.has_value())
                    {
                        att->SetValueFromString(valOpt.value(), false);
                    }
                    else
                    {
                        att->MakeUnknown(false);
                    }
                }

                updatedInstances.push_back(inst);
            }

            entity->instances = std::move(updatedInstances);
            entity->RebuildInstanceIndex();
        }
    }

    void LegacyRuleEngineAdapter::SyncToSessionState(const RuleSet& ruleSet, ISessionState& sessionState)
    {
        if (ruleSet.entities == nullptr)
        {
            return;
        }

        for (const auto& entity : *ruleSet.entities)
        {
            if (entity == nullptr) continue;

            for (const auto& inst : entity->instances)
            {
                if (inst == nullptr) continue;

                auto rec = sessionState.GetInstance(entity->entityid, inst->instanceId);
                if (rec == nullptr)
                {
                    rec = sessionState.CreateInstance(entity->entityid);
                }

                for (const auto& att : inst->attributes)
                {
                    if (att == nullptr) continue;
                    if (att->IsKnown())
                    {
                        rec->SetAttributeValue(att->GetID(), att->GetStringValue());
                    }
                }

                for (const auto& link : inst->relationshipLinks)
                {
                    if (!link.relationshipId.empty() && !link.targetInstanceId.empty())
                    {
                        rec->AddRelationshipLink(link.relationshipId, link.targetInstanceId);
                    }
                }
            }
        }
    }

    RuleEvaluationResult LegacyRuleEngineAdapter::Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuleEvaluationResult result;

        SyncFromSessionState(sessionState, runtimeTemplate_);

        std::vector<SessionModelRuleEvaluator::ChangedAttribute> changedAttrs;
        for (const auto& change : changes)
        {
            changedAttrs.push_back({ change.entityId, change.instanceId, change.attributeId });

            if (runtimeTemplate_.entities != nullptr)
            {
                for (auto& entity : *runtimeTemplate_.entities)
                {
                    if (entity && entity->entityid == change.entityId)
                    {
                        auto inst = entity->GetInstanceById(change.instanceId);
                        if (inst)
                        {
                            auto att = inst->GetAttributeByID(change.attributeId);
                            if (att)
                            {
                                att->SetValueFromString(change.rawValue, false);
                            }
                        }
                    }
                }
            }
        }

        auto stats = evaluator_.EvaluateChangedAttributes(runtimeTemplate_, changedAttrs);
        SyncToSessionState(runtimeTemplate_, sessionState);

        // Collect inferred attribute changes
        if (runtimeTemplate_.entities != nullptr)
        {
            for (const auto& entity : *runtimeTemplate_.entities)
            {
                if (!entity) continue;
                for (const auto& inst : entity->instances)
                {
                    if (!inst) continue;
                    for (const auto& att : inst->attributes)
                    {
                        if (att && att->IsInferred() && att->IsKnown())
                        {
                            result.inferredChanges.push_back({
                                entity->entityid,
                                inst->instanceId,
                                att->GetID(),
                                att->GetStringValue()
                            });
                        }
                    }
                }
            }
        }

        // Collect validation events
        for (const auto& valEv : stats.createdValidationEvents)
        {
            result.validationEvents.push_back({ valEv.entityId, valEv.instanceId, valEv.ruleId, valEv.message, valEv.severity == "Error" });
        }
        for (const auto& valEv : stats.updatedValidationEvents)
        {
            result.validationEvents.push_back({ valEv.entityId, valEv.instanceId, valEv.ruleId, valEv.message, valEv.severity == "Error" });
        }

        // Collect diagnostics
        for (const auto& d : stats.diagnostics)
        {
            result.diagnostics.push_back({ EvaluationDiagnostic::Severity::Warning, d.code, d.message, d.context });
        }

        result.success = true;
        return result;
    }

    RuleEvaluationResult LegacyRuleEngineAdapter::EvaluateAll(ISessionState& sessionState)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuleEvaluationResult result;

        SyncFromSessionState(sessionState, runtimeTemplate_);
        auto stats = evaluator_.EvaluateAllRules(runtimeTemplate_);
        SyncToSessionState(runtimeTemplate_, sessionState);

        if (runtimeTemplate_.entities != nullptr)
        {
            for (const auto& entity : *runtimeTemplate_.entities)
            {
                if (!entity) continue;
                for (const auto& inst : entity->instances)
                {
                    if (!inst) continue;
                    for (const auto& att : inst->attributes)
                    {
                        if (att && att->IsInferred() && att->IsKnown())
                        {
                            result.inferredChanges.push_back({
                                entity->entityid,
                                inst->instanceId,
                                att->GetID(),
                                att->GetStringValue()
                            });
                        }
                    }
                }
            }
        }

        for (const auto& valEv : stats.createdValidationEvents)
        {
            result.validationEvents.push_back({ valEv.entityId, valEv.instanceId, valEv.ruleId, valEv.message, valEv.severity == "Error" });
        }
        for (const auto& valEv : stats.updatedValidationEvents)
        {
            result.validationEvents.push_back({ valEv.entityId, valEv.instanceId, valEv.ruleId, valEv.message, valEv.severity == "Error" });
        }

        for (const auto& d : stats.diagnostics)
        {
            result.diagnostics.push_back({ EvaluationDiagnostic::Severity::Warning, d.code, d.message, d.context });
        }

        result.success = true;
        return result;
    }

    std::optional<std::string> LegacyRuleEngineAdapter::ResolveNextScreen(const ISessionState& sessionState, const std::string& directToScreen)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SyncFromSessionState(sessionState, runtimeTemplate_);

        auto loc = runtimeTemplate_.GetLatestScreenLocation(directToScreen);
        if (!loc.screenId.empty())
        {
            return loc.screenId;
        }

        return std::nullopt;
    }
}
