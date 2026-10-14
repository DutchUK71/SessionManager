cmake_minimum_required(VERSION 4.3)
project(SessionManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_library(SessionCore STATIC
    Core/SessionState.cpp
    Core/RuleEngineFactory.cpp
)

target_include_directories(SessionCore PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Core
)

add_library(RuleEngineLegacy STATIC
    Adapters/Legacy/LegacyRuleEngineAdapter.cpp
    Rules/Condition.cpp
    Rules/Instance.cpp
    Rules/Rule.cpp
    Rules/Attribute.cpp
    Rules/Date.cpp
    Rules/Entity.cpp
    Rules/FunctionRule.cpp
    Rules/RuleSet.cpp
    Functions.cpp
    JSONFile.cpp
    SessionModelImporter.cpp
    SessionModelExpressionParserSupport.cpp
    SessionModelRuleEvaluator.cpp
    SessionModelTemporalDateTimeSupport.cpp
    SessionModelTemporalHistorySupport.cpp
    SessionModelTemporalSupport.cpp
    SessionModelTemporalFunctionCatalog.cpp
    Util.cpp
)

target_include_directories(RuleEngineLegacy PUBLIC
    ${CMAKE_SOURCE_DIR}/Adapters/Legacy
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Rules
)

target_link_libraries(RuleEngineLegacy PUBLIC
    SessionCore
)

add_library(SessionLogic STATIC
    DataManager.cpp
    ScreenData.cpp
    ScreenFile.cpp
    Summary.cpp
)

target_include_directories(SessionLogic PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/Core
)

target_link_libraries(SessionLogic PUBLIC
    SessionCore
    RuleEngineLegacy
)

include(CTest)

if(BUILD_TESTING)
    add_executable(SessionStateTests
        Tests/SessionStateTests.cpp
    )

    target_link_libraries(SessionStateTests PRIVATE
        SessionCore
    )

    add_executable(LegacyAdapterTests
        Tests/LegacyAdapterTests.cpp
    )

    target_link_libraries(LegacyAdapterTests PRIVATE
        RuleEngineLegacy
        SessionCore
    )

    add_executable(DataManagerTests
        Tests/DataManagerTests.cpp
    )

    target_link_libraries(DataManagerTests PRIVATE
        SessionLogic
        RuleEngineLegacy
        SessionCore
    )

    add_executable(EndToEndParityTests
        Tests/EndToEndParityTests.cpp
        Adapters/Mock/MockRuleEngineAdapter.cpp
        Adapters/External/NewRuleEngineAdapter.cpp
    )

    target_include_directories(EndToEndParityTests PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Adapters/Legacy
        ${CMAKE_SOURCE_DIR}/Adapters/Mock
        ${CMAKE_SOURCE_DIR}/Adapters/External
    )

    target_link_libraries(EndToEndParityTests PRIVATE
        SessionLogic
        RuleEngineLegacy
        SessionCore
    )

    add_test(NAME SessionStateTests COMMAND SessionStateTests)
    add_test(NAME LegacyAdapterTests COMMAND LegacyAdapterTests)
    add_test(NAME DataManagerTests COMMAND DataManagerTests)
    add_test(NAME EndToEndParityTests COMMAND EndToEndParityTests)
endif()

add_executable(${PROJECT_NAME}
    EmailCommandResolver.cpp
    PdfTemplateResolver.cpp
    Main.cpp
    RuleServer.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    SessionLogic
    RuleEngineLegacy
    SessionCore
)

target_include_directories(
    ${PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}
        #Rules
)
