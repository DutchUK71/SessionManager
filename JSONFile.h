#pragma once

#include <string>
#include <optional>
#include <vector>
#include <memory>

class JSONFile
{
public:
    struct JSONCopyFrom
    {
        std::string sourceEntityId;
		std::string sourceAttributeId;
    };

	// Struct to represent a JSON attribute
    struct JSONAttribute {
        std::string attributeid;
        std::string type;
        std::string text;
        std::string coreName;
        std::string inferredbyrule;
        std::string defaultvalue;
        bool mainIdentifier = false;
		std::optional<JSONCopyFrom> copyfrom;
    };

	// Struct to represent a JSON condition
    struct JSONCondition {
        std::string attributeid;
        std::string operant;
        std::string expectedvalue;
    };

    struct JSONFunction
    {
		std::string id;
        std::string name;
        std::string targetentityid;
        std::optional<std::string> targetattribute;
    };

	// Struct to represent a JSON rule
    struct JSONRule {
        std::string ruleid;
        std::string ruletype;
        std::string attributeid;
        std::vector<JSONCondition> conditions;
        std::optional<JSONFunction> function;
    };

    struct Relationship
    {
        std::string id;
        std::string description;
        std::string targetentity;
        std::string type;
    };

    struct Entity
    {
        std::string entityid;
        std::string description;
        std::optional<bool> root;
        std::vector<JSONAttribute> attributes;
        std::vector<JSONRule> rules;
		std::vector<Relationship> relationships;
    };

    struct DataModel
    {
        std::vector<Entity> entities;
    };

    // Struct to represent attribute usage statistics
    struct AttributeUsage {
        int concludedCount = 0;
        int usageCount = 0;
	};

	// Constructor
    JSONFile(std::string fi);

	// Destructor
    ~JSONFile() {};

	// File path
    std::string f;

	// Method to load the JSON file and parse it into a DataModel
    DataModel LoadFile();

	// Method to analyse the data model
    void Analyse(std::shared_ptr<DataModel> data);

	// Method to get attribute usage statistics for a given attribute ID
    AttributeUsage CountRuleAttribute(std::shared_ptr<DataModel> data, std::string entityId, std::string attributeID);
    
	// Method to determine circular references in the data model
    void DetermineCircularReference(std::shared_ptr<Entity> ent);
};




