#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "Attribute.h"
#include "Util.h"
#include "FunctionRule.h"
#include "Rule.h"
#include "Relationship.h"

using json = nlohmann::json;

class Entity;
class Instance;

struct InstanceRelationshipLink
{
	string relationshipId;
	string sourceEntityId;
	string sourceInstanceId;
	string targetEntityId;
	string targetInstanceId;
	bool outgoing = false;
	weak_ptr<Instance> linkedInstance;
};

class Instance : public std::enable_shared_from_this<Instance>
{
	public:
	Instance()
	{
		instanceId = Util::GenerateGUID();
	}

	~Instance() = default;

	void SetInstanceId(string newId)
	{
		instanceId = newId;
	}	

	string instanceId;

	//string parentInstanceId;
	vector<shared_ptr<Instance>> linkedInstances = {};

	vector<InstanceRelationshipLink> relationshipLinks = {};

	std::vector<shared_ptr<Attribute>> attributes;

	std::vector<shared_ptr<Rule>> rules;

	mutable std::unordered_map<string, size_t> attributeIndexById;
	mutable size_t attributeIndexSize = static_cast<size_t>(-1);
	mutable std::shared_ptr<std::mutex> attributeIndexMutex = std::make_shared<std::mutex>();

	mutable std::unordered_map<string, std::vector<size_t>> relationshipLinkIndexesByRelationshipId;
	mutable size_t relationshipLinkIndexSize = static_cast<size_t>(-1);
	mutable std::shared_ptr<std::mutex> relationshipLinkIndexMutex = std::make_shared<std::mutex>();
	
	string entityId;

	bool isRoot = false;

	bool isInferred = false;

	string inferredInstanceKey;

	string createdByRuleId;

	string createdFromSourceInstanceId;

	string createdFromDrivingInstanceId;

	string createdThroughRelationshipId;

	string lastEvaluationState;

	json CreateJSON();

	void GetInstanceFromJSON(const json& j);
	
	void EvaluateFunctions();

	void AddLinkedInstance(const shared_ptr<Instance>& instance);

	void AddRelationshipLink(const shared_ptr<Instance>& instance, const shared_ptr<Relationship>& relationship, bool outgoing);
	void RebuildAttributeIndex() const;
	void InvalidateRelationshipLinkIndex() const;
	void RebuildRelationshipLinkIndex() const;
	std::vector<size_t> FindRelationshipLinkIndexesByRelationshipId(const string& relationshipId) const;
	shared_ptr<Attribute> GetAttributeByID(const string& attributeId);
};
