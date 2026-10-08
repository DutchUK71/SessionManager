cmake_minimum_required(VERSION 4.3)
project(SessionManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(${PROJECT_NAME}
    Rules/Condition.cpp
    Rules/Instance.cpp
    Rules/Rule.cpp
    Rules/Attribute.cpp
    DataManager.cpp
    EmailCommandResolver.cpp
    Rules/Date.cpp
    Rules/Entity.cpp
    Rules/FunctionRule.cpp
    Functions.cpp
    JSONFile.cpp
    PdfTemplateResolver.cpp
    Main.cpp
    RuleServer.cpp
    Rules/RuleSet.cpp
    ScreenData.cpp
    ScreenFile.cpp
    SessionModelImporter.cpp
    SessionModelExpressionParserSupport.cpp
    SessionModelRuleEvaluator.cpp
    SessionModelTemporalDateTimeSupport.cpp
    SessionModelTemporalHistorySupport.cpp
    SessionModelTemporalSupport.cpp
    SessionModelTemporalFunctionCatalog.cpp
    Summary.cpp
    Util.cpp
)

target_include_directories(
    ${PROJECT_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}
        #Rules
)
