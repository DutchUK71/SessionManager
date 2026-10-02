#pragma once

#include <optional>
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

class Summary
{
	public:
	Summary() = default;
	~Summary() = default;
	
	struct SummaryAttribute
	{
		std::string attributeId;
		std::string name;
		std::string value;
		std::string screenId;
		std::string entityId;
		std::string instanceId;
		std::string elementId;
	};

	struct SummaryInstance
	{
		std::string instanceId;
		std::string instanceName;
		std::optional<std::vector<SummaryAttribute>> attributes = std::vector<Summary::SummaryAttribute>();
	};
	
	struct SummaryEntity
	{
		std::string entityId;
		std::string entityName;
		std::optional<std::vector<SummaryInstance>> instances = std::vector<Summary::SummaryInstance>();
	};

	struct SummaryGroup
	{
		std::string entityId;
		std::string entityName;
		std::string instanceId;
		std::string instanceName;
		bool displayHeading = true;
		std::string parentEntityId;
		std::string parentInstanceId;
		std::string relationshipId;
		std::optional<std::vector<SummaryAttribute>> attributes = std::vector<Summary::SummaryAttribute>();
	};

	struct SummaryData
	{
		std::optional<std::vector<SummaryGroup>> groups = std::vector<Summary::SummaryGroup>();
		std::optional<std::vector<SummaryEntity>> entities = std::vector<Summary::SummaryEntity>();
	};
};

// nlohmann::json (de)serialization
// These must be visible in any translation unit that converts between `json` and these types.
inline void from_json(const json& j, Summary::SummaryAttribute& e)
{
	e.attributeId = j.at("attributeid").get<std::string>();
	e.name = j.at("name").get<std::string>();
	e.value = j.at("value").get<std::string>();
	e.screenId = j.value("screenid", std::string());
	e.entityId = j.value("entityid", std::string());
	e.instanceId = j.value("instanceid", std::string());
	e.elementId = j.value("elementid", std::string());
}

inline void to_json(json& j, const Summary::SummaryAttribute& e)
{
	j["attributeid"] = e.attributeId;
	j["name"] = e.name;
	j["value"] = e.value;
	j["screenid"] = e.screenId;
	j["entityid"] = e.entityId;
	j["instanceid"] = e.instanceId;
	j["elementid"] = e.elementId;
	j["target"] = {
		{"screenId", e.screenId},
		{"entityId", e.entityId},
		{"instanceId", e.instanceId},
		{"elementId", e.elementId}
	};
}

inline void from_json(const json& j, Summary::SummaryInstance& e)
{
	e.instanceId = j.at("instanceid").get<std::string>();
	e.instanceName = j.value("instancename", e.instanceId);

	if (j.contains("attributes"))
	{
		e.attributes = j.at("attributes").get<std::vector<Summary::SummaryAttribute>>();
	}
}

inline void to_json(json& j, const Summary::SummaryInstance& e)
{
	j["instanceid"] = e.instanceId;
	j["instancename"] = e.instanceName.empty() ? e.instanceId : e.instanceName;

	if (e.attributes.has_value())
	{
		j["attributes"] = e.attributes;
	}
}

inline void from_json(const json& j, Summary::SummaryEntity& e)
{
	e.entityId = j.at("entityid").get<std::string>();
	e.entityName = j.value("entityname", e.entityId);

	if (j.contains("instances"))
	{
		e.instances = j.at("instances").get<std::vector<Summary::SummaryInstance>>();
	}
}

inline void to_json(json& j, const Summary::SummaryEntity& e)
{
	j["entityid"] = e.entityId;
	j["entityname"] = e.entityName.empty() ? e.entityId : e.entityName;

	if (e.instances.has_value())
	{
		j["instances"] = e.instances;
	}
}

inline void from_json(const json& j, Summary::SummaryGroup& e)
{
	e.entityId = j.at("entityid").get<std::string>();
	e.entityName = j.value("entityname", e.entityId);
	e.instanceId = j.at("instanceid").get<std::string>();
	e.instanceName = j.value("instancename", e.instanceId);
	e.displayHeading = j.value("displayheading", j.value("displayHeading", true));
	e.parentEntityId = j.value("parententityid", std::string());
	e.parentInstanceId = j.value("parentinstanceid", std::string());
	e.relationshipId = j.value("relationshipid", std::string());

	if (j.contains("attributes"))
	{
		e.attributes = j.at("attributes").get<std::vector<Summary::SummaryAttribute>>();
	}
}

inline void to_json(json& j, const Summary::SummaryGroup& e)
{
	j["entityid"] = e.entityId;
	j["entityname"] = e.entityName.empty() ? e.entityId : e.entityName;
	j["instanceid"] = e.instanceId;
	j["instancename"] = e.instanceName.empty() ? e.instanceId : e.instanceName;
	j["displayheading"] = e.displayHeading;

	if(!e.parentEntityId.empty())
	{
		j["parententityid"] = e.parentEntityId;
	}

	if(!e.parentInstanceId.empty())
	{
		j["parentinstanceid"] = e.parentInstanceId;
	}

	if(!e.relationshipId.empty())
	{
		j["relationshipid"] = e.relationshipId;
	}

	if (e.attributes.has_value())
	{
		j["attributes"] = e.attributes;
	}
}

inline void from_json(const json& j, Summary::SummaryData& e)
{
	if (j.contains("groups"))
	{
		e.groups = j.at("groups").get<std::vector<Summary::SummaryGroup>>();
	}

	if (j.contains("entities"))
	{
		e.entities = j.at("entities").get<std::vector<Summary::SummaryEntity>>();
	}
}

inline void to_json(json& j, const Summary::SummaryData& e)
{
	if (e.groups.has_value())
	{
		j["groups"] = e.groups;
	}

	if (e.entities.has_value())
	{
		j["entities"] = e.entities;
	}
}
