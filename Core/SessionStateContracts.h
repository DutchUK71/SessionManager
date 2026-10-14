#pragma once

#include <optional>
#include <string>
#include <vector>

namespace SessionEngine
{
    struct AttributeMutation
    {
        std::string entityId;
        std::string instanceId;
        std::string attributeId;
        std::string rawValue;
    };

    struct EvaluationDiagnostic
    {
        enum class Severity
        {
            Info,
            Warning,
            Error
        };

        Severity severity = Severity::Info;
        std::string code;
        std::string message;
        std::string location;
    };

    struct ValidationEvent
    {
        std::string entityId;
        std::string instanceId;
        std::string ruleId;
        std::string message;
        bool isBlocking = false;
    };

    struct RuleEvaluationResult
    {
        bool success = true;
        std::vector<AttributeMutation> inferredChanges;
        std::vector<ValidationEvent> validationEvents;
        std::vector<EvaluationDiagnostic> diagnostics;
        std::optional<std::string> targetScreenId;
    };
}
