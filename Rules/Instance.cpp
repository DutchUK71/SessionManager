#include "Instance.h"
#include "Functions.h"
#include <memory>

void Instance::EvaluateFunctions()
{
	shared_ptr<Functions> functions = make_shared<Functions>();

	for (const shared_ptr<Rule>& rule : rules)
	{
		if(rule->GetRuleType() == FUNCTION)
		{
			functions->HandleFunction(rule, shared_from_this());
		}
	}
}

void Instance::AddLinkedInstance(const shared_ptr<Instance>& instance)
{
	if(instance == nullptr)
	{
		return;
	}

	for(const shared_ptr<Instance>& linkedInstance : linkedInstances)
	{
		if(linkedInstance == instance)
		{
			return;
		}
	}

	linkedInstances.push_back(instance);
}

void Instance::AddRelationshipLink(const shared_ptr<Instance>& instance, const shared_ptr<Relationship>& relationship, bool outgoing)
{
	if(instance == nullptr || relationship == nullptr || relationship->id.empty())
	{
		return;
	}

	const string sourceInstanceId = outgoing ? instanceId : instance->instanceId;
	const string targetInstanceId = outgoing ? instance->instanceId : instanceId;

	for(const InstanceRelationshipLink& existingLink : relationshipLinks)
	{
		if(existingLink.relationshipId == relationship->id &&
		   existingLink.sourceInstanceId == sourceInstanceId &&
		   existingLink.targetInstanceId == targetInstanceId &&
		   existingLink.outgoing == outgoing)
		{
			return;
		}
	}

	InstanceRelationshipLink link;

	link.relationshipId = relationship->id;
	link.sourceEntityId = relationship->sourceEntity;
	link.sourceInstanceId = sourceInstanceId;
	link.targetEntityId = relationship->targetEntity;
	link.targetInstanceId = targetInstanceId;
	link.outgoing = outgoing;
	link.linkedInstance = instance;

	relationshipLinks.push_back(link);
	InvalidateRelationshipLinkIndex();
}

void Instance::RebuildAttributeIndex() const
{
	attributeIndexById.clear();
	attributeIndexById.reserve(attributes.size());

	for(size_t index = 0; index < attributes.size(); ++index)
	{
		if(attributes[index] != nullptr)
		{
			const string attributeId = attributes[index]->GetID();

			if(!attributeId.empty())
			{
				attributeIndexById[attributeId] = index;
			}
		}
	}

	attributeIndexSize = attributes.size();
}

shared_ptr<Attribute> Instance::GetAttributeByID(const string& attributeId)
{
	if(attributeId.empty())
	{
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(*attributeIndexMutex);

	if(attributeIndexSize != attributes.size())
	{
		RebuildAttributeIndex();
	}

	auto it = attributeIndexById.find(attributeId);

	if(it == attributeIndexById.end())
	{
		RebuildAttributeIndex();
		it = attributeIndexById.find(attributeId);
	}

	if(it != attributeIndexById.end() &&
	   it->second < attributes.size() &&
	   attributes[it->second] != nullptr &&
	   attributes[it->second]->GetID() == attributeId)
	{
		return attributes[it->second];
	}

	RebuildAttributeIndex();
	it = attributeIndexById.find(attributeId);

	if(it != attributeIndexById.end() &&
	   it->second < attributes.size() &&
	   attributes[it->second] != nullptr &&
	   attributes[it->second]->GetID() == attributeId)
	{
		return attributes[it->second];
	}

	return nullptr;
}

void Instance::InvalidateRelationshipLinkIndex() const
{
	std::lock_guard<std::mutex> lock(*relationshipLinkIndexMutex);

	relationshipLinkIndexesByRelationshipId.clear();
	relationshipLinkIndexSize = static_cast<size_t>(-1);
}

void Instance::RebuildRelationshipLinkIndex() const
{
	relationshipLinkIndexesByRelationshipId.clear();
	relationshipLinkIndexesByRelationshipId.reserve(relationshipLinks.size());

	for(size_t index = 0; index < relationshipLinks.size(); ++index)
	{
		const string& relationshipId = relationshipLinks[index].relationshipId;

		if(!relationshipId.empty())
		{
			relationshipLinkIndexesByRelationshipId[relationshipId].push_back(index);
		}
	}

	relationshipLinkIndexSize = relationshipLinks.size();
}

std::vector<size_t> Instance::FindRelationshipLinkIndexesByRelationshipId(const string& relationshipId) const
{
	if(relationshipId.empty())
	{
		return {};
	}

	std::lock_guard<std::mutex> lock(*relationshipLinkIndexMutex);

	if(relationshipLinkIndexSize != relationshipLinks.size())
	{
		RebuildRelationshipLinkIndex();
	}

	auto it = relationshipLinkIndexesByRelationshipId.find(relationshipId);

	if(it == relationshipLinkIndexesByRelationshipId.end())
	{
		return {};
	}

	return it->second;
}

json Instance::CreateJSON()
{
	json j;

	j["entityId"] = entityId;

	j["instanceId"] = instanceId;

	j["isInferred"] = isInferred;

	if(isInferred)
	{
		j["inferredInstanceKey"] = inferredInstanceKey;
		j["createdByRuleId"] = createdByRuleId;
		j["createdFromSourceInstanceId"] = createdFromSourceInstanceId;
		j["createdFromDrivingInstanceId"] = createdFromDrivingInstanceId;
		j["createdThroughRelationshipId"] = createdThroughRelationshipId;
		j["lastEvaluationState"] = lastEvaluationState;
	}
	
	j["attributes"] = nlohmann::json::array();

	for(const shared_ptr<Attribute>& att : attributes)
	{
		if(att && !att->IsUnknown())
		{
			j["attributes"].push_back(att->CreateJSON());
		}
	}

	j["linkedInstances"] = nlohmann::json::array();

	for(const shared_ptr<Instance>& linkedInst : linkedInstances)
	{
		j["linkedInstances"].push_back(linkedInst->instanceId);
	}

	j["relationshipLinks"] = nlohmann::json::array();

	for(const InstanceRelationshipLink& link : relationshipLinks)
	{
		json linkJson;

		linkJson["relationshipId"] = link.relationshipId;
		linkJson["sourceEntityId"] = link.sourceEntityId;
		linkJson["sourceInstanceId"] = link.sourceInstanceId;
		linkJson["targetEntityId"] = link.targetEntityId;
		linkJson["targetInstanceId"] = link.targetInstanceId;
		linkJson["direction"] = link.outgoing ? "outgoing" : "incoming";

		if(auto linkedInstance = link.linkedInstance.lock())
		{
			linkJson["linkedInstanceId"] = linkedInstance->instanceId;
		}

		j["relationshipLinks"].push_back(linkJson);
	}

	return j;
}

void Instance::GetInstanceFromJSON(const json& j)
{
	entityId = j.at("entityId").get<string>();
	
	instanceId = j.at("instanceId").get<string>();
	
	for(const json& attJson : j.at("attributes"))
	{
		shared_ptr<Attribute> att = GetAttributeByID(attJson.value("id", std::string()));
		
		//att->GetAttributeFromJSON(attJson);
	}
}
