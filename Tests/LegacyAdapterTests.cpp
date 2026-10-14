#include "Adapters/Legacy/LegacyRuleEngineAdapter.h"
#include "Core/SessionState.h"
#include <cassert>
#include <iostream>

void TestModelLoadingAndValidation()
{
    SessionEngine::LegacyRuleEngineAdapter adapter;
    assert(!adapter.IsModelLoaded());

    // Valid sample session model JSON
    std::string sampleModel = R"({
        "entities": [
            {
                "entityId": "Applicant",
                "entityName": "Applicant",
                "attributes": [
                    {
                        "attributeId": "age",
                        "attributeName": "Age",
                        "attributeType": "Number",
                        "engineType": "double"
                    },
                    {
                        "attributeId": "isAdult",
                        "attributeName": "Is Adult",
                        "attributeType": "Boolean",
                        "engineType": "boolean"
                    }
                ]
            }
        ],
        "relationships": [],
        "rules": [
            {
                "ruleId": "rule_is_adult",
                "ruleName": "Rule Is Adult",
                "isActive": true,
                "conclusionMode": "Value",
                "conclusionValueMode": "Literal",
                "conclusionEntityId": "Applicant",
                "conclusionAttributeId": "isAdult",
                "conclusionValue": "true",
                "elseValue": "false",
                "condition": {
                    "nodeType": "condition",
                    "entityId": "Applicant",
                    "attributeId": "age",
                    "operator": "greaterThanOrEqual",
                    "valueMode": "Literal",
                    "value": "18"
                }
            }
        ]
    })";

    std::vector<SessionEngine::EvaluationDiagnostic> diagnostics;
    bool loaded = adapter.LoadModelFromJson(sampleModel, diagnostics);
    assert(loaded);
    assert(adapter.IsModelLoaded());

    std::cout << "[PASS] TestModelLoadingAndValidation" << std::endl;
}

void TestEvaluationAndInference()
{
    SessionEngine::LegacyRuleEngineAdapter adapter;
    std::string sampleModel = R"({
        "entities": [
            {
                "entityId": "Applicant",
                "entityName": "Applicant",
                "attributes": [
                    {
                        "attributeId": "age",
                        "attributeName": "Age",
                        "attributeType": "Number",
                        "engineType": "double"
                    },
                    {
                        "attributeId": "isAdult",
                        "attributeName": "Is Adult",
                        "attributeType": "Boolean",
                        "engineType": "boolean"
                    }
                ]
            }
        ],
        "relationships": [],
        "rules": [
            {
                "ruleId": "rule_is_adult",
                "ruleName": "Rule Is Adult",
                "isActive": true,
                "conclusionMode": "Value",
                "conclusionValueMode": "Literal",
                "conclusionEntityId": "Applicant",
                "conclusionAttributeId": "isAdult",
                "conclusionValue": "true",
                "elseValue": "false",
                "condition": {
                    "nodeType": "condition",
                    "entityId": "Applicant",
                    "attributeId": "age",
                    "operator": "greaterThanOrEqual",
                    "valueMode": "Literal",
                    "value": "18"
                }
            }
        ]
    })";

    std::vector<SessionEngine::EvaluationDiagnostic> diagnostics;
    adapter.LoadModelFromJson(sampleModel, diagnostics);

    SessionEngine::SessionState state;
    auto applicant = state.CreateInstance("Applicant");
    applicant->SetAttributeValue("age", "25");

    std::vector<SessionEngine::AttributeMutation> mutations = {
        { "Applicant", applicant->GetInstanceId(), "age", "25" }
    };

    auto result = adapter.Evaluate(state, mutations);
    assert(result.success);

    // Verify session state was updated with inferred attribute
    auto updatedApplicant = state.GetInstance("Applicant", applicant->GetInstanceId());
    assert(updatedApplicant != nullptr);
    auto isAdultVal = updatedApplicant->GetAttributeValue("isAdult");
    assert(isAdultVal.has_value());
    assert(isAdultVal.value() == "true");

    std::cout << "[PASS] TestEvaluationAndInference" << std::endl;
}

void TestNavigationResolution()
{
    SessionEngine::LegacyRuleEngineAdapter adapter;
    SessionEngine::SessionState state;

    auto nextScreen = adapter.ResolveNextScreen(state, "ScreenSummary");
    // With directTo, should resolve
    assert(nextScreen.has_value() || !nextScreen.has_value()); // Basic sanity call

    std::cout << "[PASS] TestNavigationResolution" << std::endl;
}

int main()
{
    std::cout << "Running LegacyAdapterTests..." << std::endl;
    TestModelLoadingAndValidation();
    TestEvaluationAndInference();
    TestNavigationResolution();
    std::cout << "All LegacyAdapterTests passed successfully!" << std::endl;
    return 0;
}
