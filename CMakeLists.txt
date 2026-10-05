cmake_minimum_required(VERSION 4.3)
project(SessionManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

add_executable(${PROJECT_NAME}
    Condition.cpp
    Instance.cpp
    Relationship.cpp
    Rule.cpp
    Attribute.cpp
    DataManager.cpp
    EmailCommandResolver.cpp
    Date.cpp
    Entity.cpp
    FunctionRule.cpp
    Functions.cpp
    JSONFile.cpp
    PdfTemplateResolver.cpp
    Main.cpp
    RuleServer.cpp
    RuleSet.cpp
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
