#pragma once

#include "JSONFile.h"
#include "Attribute.h"
#include "Rule.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "Instance.h"
#include "Relationship.h"
#include "Util.h"
#include "json.hpp"

using json = nlohmann::json;

class Entity
{
	public:
	// Constructor
	Entity()
	{
	};

	// Constructor to initialize an attribute from a JSON attribute
	Entity(JSONFile::Entity);

	// Destructor
	~Entity()
	{
	};

	std::string entityid;
	std::string description;

	std::optional<bool> root;
	std::vector<shared_ptr<Instance>> instances;
	std::vector<shared_ptr<Attribute>> attributes;
	std::vector<shared_ptr<Rule>> rules;
	std::shared_ptr<Attribute> identifyingAttribute;
	std::vector<shared_ptr<Relationship>> relationships;
	bool useRuntimeDefinitionsForInstances = false;

	mutable std::unordered_map<string, size_t> instanceIndexById;
	mutable size_t instanceIndexSize = static_cast<size_t>(-1);
	mutable std::shared_ptr<std::mutex> instanceIndexMutex = std::make_shared<std::mutex>();

	string AddInstance();
	string AddInstance(shared_ptr<Instance> parentInstance, shared_ptr<vector<shared_ptr<Entity>>> entities, const string& relationshipId = "");
	shared_ptr<Instance> GetCleanInstance();
	std::vector<std::shared_ptr<Attribute>> CreateAttributesDeepCopy();
	std::vector<std::shared_ptr<Rule>> CreateRulesDeepCopy();
	void RemoveInstanceById(string instanceId);
	void RemoveInstanceById(string instanceId, shared_ptr<vector<shared_ptr<Entity>>> entities, string entityId);
	void CascadeDelete(const shared_ptr<Instance>& instance, const shared_ptr<Instance>& deletionInstance, shared_ptr<vector<shared_ptr<Entity>>> entities, bool original = false);
	void RebuildInstanceIndex() const;
	shared_ptr<Instance> GetInstanceById(string instanceId);

	// Create a function to translate the current state of the entity to a JSON object that can be sent to the client
	json CreateJSON();
};
