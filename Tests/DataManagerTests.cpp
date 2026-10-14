#include "DataManager.h"
#include "Adapters/Legacy/LegacyRuleEngineAdapter.h"
#include "Core/SessionState.h"
#include "ScreenFile.h"
#include <cassert>
#include <iostream>

void TestDataManagerScreenSubmission()
{
    SessionEngine::LegacyRuleEngineAdapter ruleEngine;
    SessionEngine::SessionState sessionState;

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
    ruleEngine.LoadModelFromJson(sampleModel, diagnostics);

    auto applicant = sessionState.CreateInstance("Applicant");

    DataManager dm;
    auto screens = std::make_shared<ScreenFile::ScreenData>();
    auto screen1 = std::make_shared<ScreenFile::Screen>();
    screen1->id = "Screen1";
    screen1->entityId = "Applicant";

    auto inputData = std::make_shared<ScreenData::DataRoot>();
    ScreenData::Data ageInput;
    ageInput.attributeId = "age";
    ageInput.value = "21";
    inputData->entityId = "Applicant";
    inputData->instanceId = applicant->GetInstanceId();
    inputData->nextScreenId = "ScreenSummary";
    inputData->data.push_back(ageInput);

    auto resultScreen = dm.HandleData(sessionState, ruleEngine, inputData, screen1, screens, "");

    auto appRec = sessionState.GetInstance("Applicant", applicant->GetInstanceId());
    assert(appRec != nullptr);
    assert(appRec->GetAttributeValue("isAdult").value() == "true");

    std::cout << "[PASS] TestDataManagerScreenSubmission" << std::endl;
}

void TestDataManagerSummaryGeneration()
{
    SessionEngine::SessionState sessionState;
    auto applicant = sessionState.CreateInstance("Applicant");
    applicant->SetAttributeValue("firstName", "John");
    applicant->SetAttributeValue("lastName", "Doe");

    DataManager dm;
    auto screens = std::make_shared<ScreenFile::ScreenData>();
    auto summary = dm.GetSummaryData(sessionState, screens.get());

    assert(summary.groups.has_value() || summary.entities.has_value());
    std::cout << "[PASS] TestDataManagerSummaryGeneration" << std::endl;
}

void TestDataManagerInstanceOperations()
{
    SessionEngine::SessionState sessionState;
    DataManager dm;

    auto p1 = sessionState.CreateInstance("Person");
    p1->SetAttributeValue("city", "Boston");

    auto p2 = sessionState.CreateInstance("Person");
    dm.Copy(p1->GetInstanceId(), p2->GetInstanceId(), sessionState);

    assert(p2->GetAttributeValue("city").value() == "Boston");

    dm.DeleteInstance("Person", p1->GetInstanceId(), sessionState);
    assert(sessionState.GetInstance("Person", p1->GetInstanceId()) == nullptr);

    std::cout << "[PASS] TestDataManagerInstanceOperations" << std::endl;
}

int main()
{
    std::cout << "Running DataManagerTests..." << std::endl;
    TestDataManagerScreenSubmission();
    TestDataManagerSummaryGeneration();
    TestDataManagerInstanceOperations();
    std::cout << "All DataManagerTests passed successfully!" << std::endl;
    return 0;
}
