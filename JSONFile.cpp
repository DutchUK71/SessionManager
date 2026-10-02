#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include "json.hpp"
#include "JSONFile.h"

using json = nlohmann::json;
using namespace std;

void from_json(const json& j, JSONFile::JSONCopyFrom& c)
{
    j.at("sourceentityid").get_to(c.sourceEntityId);
    j.at("sourceattributeid").get_to(c.sourceAttributeId);
}


///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: from_json (JSON to JSONAttribute)
//
// Description: Converts a JSON object to a JSONAttribute struct.
//
// Parameters:
//    const json& j: The JSON object to be converted.
//    JSONFile::JSONAttribute& a: The JSONAttribute struct to store the converted data.
//
// Returns: None.
//
/////////////////////////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, JSONFile::JSONAttribute& a) 
{
    j.at("attributeid").get_to(a.attributeid);
    j.at("type").get_to(a.type);
    j.at("text").get_to(a.text);

    if (j.contains("coreName"))
    {
        j.at("coreName").get_to(a.coreName);
    }
    else if (j.contains("corename"))
    {
        j.at("corename").get_to(a.coreName);
    }

    if (j.contains("inferredbyrule"))
    {
        j.at("inferredbyrule").get_to(a.inferredbyrule);
    }

    if (j.contains("defaultValue"))
    {
        j.at("defaultValue").get_to(a.defaultvalue);
	}
    else if (j.contains("defaultvalue"))
    {
        j.at("defaultvalue").get_to(a.defaultvalue);
	}
    else if (j.contains("default"))
    {
        j.at("default").get_to(a.defaultvalue);
	}

    if (j.contains("mainIdentifier"))
    {
        j.at("mainIdentifier").get_to(a.mainIdentifier);
    }
    else if (j.contains("mainidentifier"))
    {
        j.at("mainidentifier").get_to(a.mainIdentifier);
    }
    else if (j.contains("isMainIdentifier"))
    {
        j.at("isMainIdentifier").get_to(a.mainIdentifier);
    }
    
    if (j.contains("copyfrom"))
    {
        j.at("copyfrom").get_to(a.copyfrom);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: from_json (JSON to JSONCondition)
//
// Description: Converts a JSON object to a JSONCondition struct.
//
// Parameters:
//    const json& j: The JSON object to be converted.
//    JSONFile::JSONCondition& c: The JSONCondition struct to store the converted data.
//
// Returns: None.
//
/////////////////////////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, JSONFile::JSONCondition& c) 
{
    j.at("attributeid").get_to(c.attributeid);
    j.at("operant").get_to(c.operant);
    j.at("expectedvalue").get_to(c.expectedvalue);
}

void from_json(const json& j, JSONFile::JSONFunction& f)
{
	j.at("id").get_to(f.id);
    j.at("name").get_to(f.name);
    j.at("targetentityid").get_to(f.targetentityid);

    if (j.contains("targetattribute"))
    {
        j.at("targetattribute").get_to(f.targetattribute);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: from_json (JSON to JSONRule)
//
// Description: Converts a JSON object to a JSONRule struct.
//
// Parameters:
//    const json& j: The JSON object to be converted.
//    JSONFile::JSONRule& r: The JSONRule struct to store the converted data.
//
// Returns: None.
//
/////////////////////////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, JSONFile::JSONRule& r) 
{
    j.at("ruleid").get_to(r.ruleid);
    j.at("ruletype").get_to(r.ruletype);
    j.at("attributeid").get_to(r.attributeid);
    
    if(j.contains("conditions"))
    {
        j.at("conditions").get_to(r.conditions);
    }
    
    if(j.contains("function"))
    {
        j.at("function").get_to(r.function);
    }
}

void from_json(const json& j, JSONFile::Relationship& r)
{
    j.at("id").get_to(r.id);
    j.at("description").get_to(r.description);
    j.at("targetentity").get_to(r.targetentity);
    j.at("type").get_to(r.type);
}

void from_json(const json& j, JSONFile::Entity& e)
{
    if(j.contains("root"))
    {
        j.at("root").get_to(e.root);
    }

    j.at("entityid").get_to(e.entityid);

    j.at("description").get_to(e.description);

    j.at("attributes").get_to(e.attributes);
    j.at("rules").get_to(e.rules);
    
    if(j.contains("relationships"))
    {
        j.at("relationships").get_to(e.relationships);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: from_json (JSON to DataModel)
//
// Description: Converts a JSON object to a DataModel struct.
//
// Parameters:
//    const json& j: The JSON object to be converted.
//    JSONFile::DataModel& d: The DataModel struct to store the converted data.
//
// Returns: None.
//
/////////////////////////////////////////////////////////////////////////////////////////////////
//void from_json(const json& j, JSONFile::DataModel& d) 
//{
//    j.at("attributes").get_to(d.attributes);
//    j.at("rules").get_to(d.rules);
//}

void from_json(const json& j, JSONFile::DataModel& d)
{
	j.at("entities").get_to(d.entities);
}

////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: JSONFile (Constructor)
//
// Description: Initializes a JSONFile object with the specified file path.
//
// Parameters:
//    std::string fi: The file path of the JSON file.
//
// Returns: None.
//
////////////////////////////////////////////////////////////////////////////////////////////////
JSONFile::JSONFile(std::string fi)
{
	f = fi;
}

////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: LoadFile
//
// Description: Loads and parses the JSON file into a DataModel.
//
// Parameters: None.
//
// Returns:
//    JSONFile::DataModel: The parsed data model containing attributes and rules.
//
////////////////////////////////////////////////////////////////////////////////////////////////
JSONFile::DataModel JSONFile::LoadFile()
{
	// Open the JSON file
    std::ifstream file(f);

	// Data model to store parsed data
	DataModel data = {};

	// Check if the file is open
    if (!file.is_open()) 
    {
        std::cerr << "Failed to open example.json\n";

        // Return empty data model
        return data;
    }

	// Parse the JSON file
    json j;
    
    file >> j;

    data = j.get<DataModel>();

    for(JSONFile::Entity& entity : data.entities)
    {
		for(JSONFile::JSONRule& rule : entity.rules)
        {
            for(JSONFile::JSONAttribute& att : entity.attributes)
            {
                if(att.attributeid == rule.attributeid)
                {
                    att.inferredbyrule = rule.ruleid;
                }
            }
        }
    }

	// Analyse the data model
	Analyse(std::make_shared<DataModel>(data));

	// Determine circular references in the data model
    for(const auto& ent : data.entities)
    {
        DetermineCircularReference(make_shared<Entity>(ent));
    }

	// Return the parsed data model
    return data;
}

////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: Analyse
//
// Description: Analyzes the data model for attribute usage and conclusions.
//
// Parameters:
//    std::shared_ptr<DataModel> data: The data model to be analyzed.
//
// Returns: None.
//
////////////////////////////////////////////////////////////////////////////////////////////////
/*
void JSONFile::Analyse(std::shared_ptr<DataModel> data)
{
	// Iterate through each attribute in the data model
    for (const auto& att : data->attributes)
    {
		// Get usage statistics for the attribute
        AttributeUsage usage = CountRuleAttribute(data, att.attributeid);

		// Check if the attribute is neither used nor concluded
        if (usage.usageCount == 0 && usage.concludedCount == 0)
        {
            std::cout << "Attribute " << att.attributeid << " (\"" << att.text << "\") is not concluded or used\n";
        }

		// Check if the attribute is concluded multiple times
        if (usage.concludedCount > 1)
        {
            std::cout << "Attribute " << att.attributeid << " (\"" << att.text << "\") is concluded multiple times" << usage.concludedCount << ")x.\n";
        }
    }
} 
*/
void JSONFile::Analyse(std::shared_ptr<DataModel> data)
{
    for(const auto& ent : data->entities)
    {
        // Iterate through each attribute in the data model
        for(const auto& att : ent.attributes)
        {
            // Get usage statistics for the attribute
            AttributeUsage usage = CountRuleAttribute(data, ent.entityid, att.attributeid);

            // Check if the attribute is neither used nor concluded
            if(usage.usageCount == 0 && usage.concludedCount == 0)
            {
                std::cout << "Attribute " << att.attributeid << " (\"" << att.text << "\") is not concluded or used\n";
            }

            // Check if the attribute is concluded multiple times
            if(usage.concludedCount > 1)
            {
                std::cout << "Attribute " << att.attributeid << " (\"" << att.text << "\") is concluded multiple times" << usage.concludedCount << ")x.\n";
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: CountRuleAttribute
//
// Description: Counts how many times an attribute is concluded and used in rules.
//
// Parameters:
//    std::shared_ptr<DataModel> data: The data model containing rules.
//    std::string attributeID: The ID of the attribute to be counted.
//
// Returns:
//    JSONFile::AttributeUsage: The usage statistics for the specified attribute.
//
////////////////////////////////////////////////////////////////////////////////////////////////
JSONFile::AttributeUsage JSONFile::CountRuleAttribute(std::shared_ptr<DataModel> data, std::string entityId, std::string attributeID)
{
	// Initialize usage statistics
	AttributeUsage usage;

    for(const auto& ent : data->entities)
    {
        if(ent.entityid == entityId)
        {
            // Iterate through each rule in the data model
            for(const auto& rule : ent.rules)
            {
                // Check if the rule concludes the specified attribute
                if(rule.attributeid == attributeID)
                {
                    // Increment concluded count
                    usage.concludedCount++;
                }

                // Iterate through each condition in the rule
                for(const auto& condition : rule.conditions)
                {
                    // Check if the condition uses the specified attribute
                    if(condition.attributeid == attributeID)
                    {
                        // Increment usage count
                        usage.usageCount++;
                    }
                }
            }

            break;
        }
    }

	// Return the usage statistics
    return usage;
}

////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: DetermineCircularReference
//
// Description: Determines if there are any circular references in the rules of the data model.
//
// Parameters:
//    std::shared_ptr<DataModel> data: The data model to be checked for circular references.
//
// Returns: None.
//
////////////////////////////////////////////////////////////////////////////////////////////////
void JSONFile::DetermineCircularReference(std::shared_ptr<Entity> ent)
{
    // Iterate through each rule in the data model
    for(const auto& rule : ent->rules)
    {
        // Check for circular reference in the rule's conditions
        for(const auto& condition : rule.conditions)
        {
            // If the rule's attribute ID matches the condition's attribute ID, a circular reference is detected
            if(rule.attributeid == condition.attributeid)
            {
                std::cout << "Circular reference detected in rule " << rule.ruleid << " for attribute " << rule.attributeid << " in entity " + ent->entityid + "\n";
            }
        }
    }
}
