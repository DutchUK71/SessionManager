#include "Adapters/Legacy/LegacyRuleEngineAdapter.h"
#include "Adapters/Mock/MockRuleEngineAdapter.h"
#include "Adapters/External/NewRuleEngineAdapter.h"
#include "Core/RuleEngineFactory.h"
#include "Core/SessionState.h"
#include "DataManager.h"
#include "ScreenFile.h"
#include <cassert>
#include <iostream>

void RegisterEngines()
{
    SessionEngine::RuleEngineFactory::RegisterEngine(SessionEngine::EngineType::Legacy, []() {
        return std::make_unique<SessionEngine::LegacyRuleEngineAdapter>();
    });
    SessionEngine::RuleEngineFactory::RegisterEngine(SessionEngine::EngineType::Mock, []() {
        return std::make_unique<SessionEngine::MockRuleEngineAdapter>();
    });
    SessionEngine::RuleEngineFactory::RegisterEngine(SessionEngine::EngineType::ExternalLibrary, []() {
        return std::make_unique<SessionEngine::NewRuleEngineAdapter>();
    });
}

void TestFactoryInstantiation()
{
    auto legacyEngine = SessionEngine::RuleEngineFactory::CreateEngine(SessionEngine::EngineType::Legacy);
    assert(legacyEngine != nullptr);

    auto mockEngine = SessionEngine::RuleEngineFactory::CreateEngine(SessionEngine::EngineType::Mock);
    assert(mockEngine != nullptr);

    auto extEngine = SessionEngine::RuleEngineFactory::CreateEngine(SessionEngine::EngineType::ExternalLibrary);
    assert(extEngine != nullptr);

    std::cout << "[PASS] TestFactoryInstantiation" << std::endl;
}

void TestEndToEndWorkflowParity()
{
    auto legacyEngine = SessionEngine::RuleEngineFactory::CreateEngine(SessionEngine::EngineType::Legacy);
    auto mockEngine = SessionEngine::RuleEngineFactory::CreateEngine(SessionEngine::EngineType::Mock);

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

    std::vector<SessionEngine::EvaluationDiagnostic> diag;
    legacyEngine->LoadModelFromJson(sampleModel, diag);
    mockEngine->LoadModelFromJson(sampleModel, diag);

    // Run workflow with legacy engine
    SessionEngine::SessionState stateLegacy;
    auto appLegacy = stateLegacy.CreateInstance("Applicant");
    DataManager dmL;

    auto screens = std::make_shared<ScreenFile::ScreenData>();
    auto screen1 = std::make_shared<ScreenFile::Screen>();
    screen1->id = "Screen1";
    screen1->entityId = "Applicant";

    auto inputDataL = std::make_shared<ScreenData::DataRoot>();
    ScreenData::Data ageData;
    ageData.attributeId = "age";
    ageData.value = "28";
    inputDataL->entityId = "Applicant";
    inputDataL->instanceId = appLegacy->GetInstanceId();
    inputDataL->data.push_back(ageData);

    dmL.HandleData(stateLegacy, *legacyEngine, inputDataL, screen1, screens, "ScreenSummary");

    auto recLegacy = stateLegacy.GetInstance("Applicant", appLegacy->GetInstanceId());
    assert(recLegacy != nullptr);
    assert(recLegacy->GetAttributeValue("age").has_value());
    assert(std::stod(recLegacy->GetAttributeValue("age").value()) == 28.0);
    assert(recLegacy->GetAttributeValue("isAdult").value() == "true");

    // Run workflow with mock engine
    SessionEngine::SessionState stateMock;
    auto appMock = stateMock.CreateInstance("Applicant");
    DataManager dmM;

    auto inputDataM = std::make_shared<ScreenData::DataRoot>();
    inputDataM->entityId = "Applicant";
    inputDataM->instanceId = appMock->GetInstanceId();
    inputDataM->data.push_back(ageData);

    dmM.HandleData(stateMock, *mockEngine, inputDataM, screen1, screens, "ScreenSummary");

    auto recMock = stateMock.GetInstance("Applicant", appMock->GetInstanceId());
    assert(recMock != nullptr);
    assert(recMock->GetAttributeValue("age").has_value());
    assert(std::stod(recMock->GetAttributeValue("age").value()) == 28.0);

    std::cout << "[PASS] TestEndToEndWorkflowParity" << std::endl;
}

int main()
{
    std::cout << "Running EndToEndParityTests..." << std::endl;
    RegisterEngines();
    TestFactoryInstantiation();
    TestEndToEndWorkflowParity();
    std::cout << "All EndToEndParityTests passed successfully!" << std::endl;
    return 0;
}
