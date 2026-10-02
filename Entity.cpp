#include "Entity.h"
#include "RuleServer.h"
#include "Functions.h"
#include "json.hpp"

//using json = nlohmann::json;

namespace
{
	void LinkInstances(const shared_ptr<Instance>& sourceInstance, const shared_ptr<Instance>& targetInstance, const shared_ptr<Relationship>& relationship = nullptr)
	{
		if(sourceInstance == nullptr || targetInstance == nullptr)
		{
			return;
		}

		sourceInstance->AddLinkedInstance(targetInstance);
		targetInstance->AddLinkedInstance(sourceInstance);

		if(relationship != nullptr)
		{
			sourceInstance->AddRelationshipLink(targetInstance, relationship, true);
			targetInstance->AddRelationshipLink(sourceInstance, relationship, false);
		}
	}

	bool RelationshipLinkReferencesInstance(const InstanceRelationshipLink& link, const shared_ptr<Instance>& instance)
	{
		if(instance == nullptr)
		{
			return false;
		}

		if(link.sourceInstanceId == instance->instanceId || link.targetInstanceId == instance->instanceId)
		{
			return true;
		}

		if(auto linkedInstance = link.linkedInstance.lock())
		{
			return linkedInstance == instance || linkedInstance->instanceId == instance->instanceId;
		}

		return false;
	}

	void UnlinkInstances(const shared_ptr<Instance>& instance, const shared_ptr<Instance>& linkedInstance)
	{
		if(instance == nullptr || linkedInstance == nullptr)
		{
			return;
		}

		instance->linkedInstances.erase(std::remove(instance->linkedInstances.begin(), instance->linkedInstances.end(), linkedInstance), instance->linkedInstances.end());
		instance->relationshipLinks.erase(std::remove_if(instance->relationshipLinks.begin(),
		                                                 instance->relationshipLinks.end(),
		                                                 [&linkedInstance](const InstanceRelationshipLink& link) {
			                                                 return RelationshipLinkReferencesInstance(link, linkedInstance);
		                                                 }),
		                                  instance->relationshipLinks.end());
		instance->InvalidateRelationshipLinkIndex();
	}

	shared_ptr<Relationship> FindRelationship(const shared_ptr<vector<shared_ptr<Entity>>>& entities,
	                                          const string& sourceEntityId,
	                                          const string& targetEntityId,
	                                          const string& relationshipId)
	{
		if(entities == nullptr)
		{
			return nullptr;
		}

		for(const shared_ptr<Entity>& entity : *entities)
		{
			if(entity == nullptr || entity->entityid != sourceEntityId)
			{
				continue;
			}

			for(const shared_ptr<Relationship>& relationship : entity->relationships)
			{
				if(relationship != nullptr &&
				   relationship->targetEntity == targetEntityId &&
				   (relationshipId.empty() || relationship->id == relationshipId))
				{
					return relationship;
				}
			}
		}

		return nullptr;
	}
}

Entity::Entity(JSONFile::Entity jsonEntity)
{
	entityid = jsonEntity.entityid;

	description = jsonEntity.description;
	
	root = jsonEntity.root;

	instances = std::vector<shared_ptr<Instance>>();
}

string Entity::AddInstance()
{
	shared_ptr<Instance> inst = GetCleanInstance();

	inst->entityId = entityid;

	inst->isRoot = true;
	
	instances.push_back(inst);

	inst->EvaluateFunctions();

	return inst->instanceId;
}

string Entity::AddInstance(shared_ptr<Instance> parentInstance,
	                       shared_ptr<vector<shared_ptr<Entity>>> entities,
	                       const string& relationshipId)
{
	shared_ptr<Instance> inst = GetCleanInstance();

	inst->entityId = entityid;

	instances.push_back(inst);

	shared_ptr<Relationship> parentRelationship = FindRelationship(
		entities,
		parentInstance != nullptr ? parentInstance->entityId : "",
		entityid,
		relationshipId);

	LinkInstances(parentInstance, inst, parentRelationship);

	inst->EvaluateFunctions();

	return inst->instanceId;
}

void Entity::CascadeDelete(const shared_ptr<Instance>& instance, const shared_ptr<Instance>& deletionInstance, shared_ptr<vector<shared_ptr<Entity>>> entities, bool original)
{
	if(instance == nullptr || deletionInstance == nullptr)
	{
		return;
	}

	vector<shared_ptr<Instance>> linkedInstances = instance->linkedInstances;

	for(const shared_ptr<Instance>& linkedInst : linkedInstances)
	{
		if(linkedInst == nullptr)
		{
			continue;
		}

		if(linkedInst == deletionInstance)
		{
			UnlinkInstances(instance, deletionInstance);

			if(instance->linkedInstances.size() == 0 && !instance->isRoot)
			{
				// If the linked instance has no more links and is not a root instance, it should be deleted as well as it is an orphan.
				RemoveInstanceById(instance->instanceId, entities, instance->entityId);
			}

			instance->EvaluateFunctions();

			break;
		}

		if(original == true)
		{
			CascadeDelete(linkedInst, deletionInstance, entities);
		}
	}

	if(instance == deletionInstance)
	{
		vector<shared_ptr<Instance>> deletionLinks = deletionInstance->linkedInstances;

		for(const shared_ptr<Instance>& linkedInst : deletionLinks)
		{
			UnlinkInstances(linkedInst, deletionInstance);

			if(linkedInst != nullptr)
			{
				linkedInst->EvaluateFunctions();
			}
		}

		instance->linkedInstances.clear();
		instance->relationshipLinks.clear();
		instance->InvalidateRelationshipLinkIndex();

		RemoveInstanceById(deletionInstance->instanceId);
	}
}

void Entity::RebuildInstanceIndex() const
{
	instanceIndexById.clear();
	instanceIndexById.reserve(instances.size());

	for(size_t index = 0; index < instances.size(); ++index)
	{
		if(instances[index] != nullptr && !instances[index]->instanceId.empty())
		{
			instanceIndexById[instances[index]->instanceId] = index;
		}
	}

	instanceIndexSize = instances.size();
}

shared_ptr<Instance> Entity::GetInstanceById(string instanceId)
{
	if(instanceId.empty())
	{
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(*instanceIndexMutex);

	if(instanceIndexSize != instances.size())
	{
		RebuildInstanceIndex();
	}

	auto it = instanceIndexById.find(instanceId);

	if(it == instanceIndexById.end())
	{
		RebuildInstanceIndex();
		it = instanceIndexById.find(instanceId);
	}

	if(it != instanceIndexById.end() &&
	   it->second < instances.size() &&
	   instances[it->second] != nullptr &&
	   instances[it->second]->instanceId == instanceId)
	{
		return instances[it->second];
	}

	RebuildInstanceIndex();
	it = instanceIndexById.find(instanceId);

	if(it != instanceIndexById.end() &&
	   it->second < instances.size() &&
	   instances[it->second] != nullptr &&
	   instances[it->second]->instanceId == instanceId)
	{
		return instances[it->second];
	}

	return nullptr;
}

void Entity::RemoveInstanceById(string instanceId)
{
		instances.erase(std::remove_if(instances.begin(), instances.end(),
		[&instanceId](const shared_ptr<Instance>& inst) {
			return inst->instanceId == instanceId;
		}), instances.end());
}

void Entity::RemoveInstanceById(string instanceId, shared_ptr<vector<shared_ptr<Entity>>> entities, string childEntityId)
{
	for(const shared_ptr<Entity>& ent : *entities)
	{
		if(ent->entityid == childEntityId)
		{
			ent->instances.erase(std::remove_if(ent->instances.begin(), ent->instances.end(),
				[&instanceId](const shared_ptr<Instance>& inst) {
				return inst->instanceId == instanceId;
			}), ent->instances.end());
		}
	}
}

shared_ptr<Instance> Entity::GetCleanInstance()
{
	if(useRuntimeDefinitionsForInstances)
	{
		shared_ptr<Instance> instance = make_shared<Instance>();

		instance->entityId = entityid;

		instance->attributes = CreateAttributesDeepCopy();

		for(const shared_ptr<Attribute>& attribute : instance->attributes)
		{
			if(attribute != nullptr)
			{
				attribute->instance = instance;
			}
		}

		return instance;
	}

	shared_ptr<Instance> instance = RuleServer::GetCleanInstance(entityid);

	return instance;
}

std::vector<std::shared_ptr<Attribute>> Entity::CreateAttributesDeepCopy()
{
	std::vector<std::shared_ptr<Attribute>> dst;

	dst.reserve(attributes.size());

	for(const auto& att : attributes)
	{
		if(att)
		{
			dst.push_back(std::make_shared<Attribute>(*att));
		}
	}

	return dst;
}

std::vector<std::shared_ptr<Rule>> Entity::CreateRulesDeepCopy()
{
	std::vector<std::shared_ptr<Rule>> dst;

	dst.reserve(rules.size());

	for(const auto& rule : rules)
	{
		if(rule)
		{
			dst.push_back(std::make_shared<Rule>(*rule));
		}
	}

	return dst;
}

// std::optional<bool> root;
// std::vector<shared_ptr<Instance>> instances;
// std::vector<shared_ptr<Attribute>> attributes;
// std::vector<shared_ptr<Rule>> rules;
// std::shared_ptr<Attribute> identifyingAttribute;
// std::vector<shared_ptr<Relationship>> relationships;
// Create a function to take all the elements from the entity and translate it to a JSON structure
json Entity::CreateJSON()
{
	json j;

	j["entityid"] = entityid;

	j["instances"] = nlohmann::json::array();

	for(const shared_ptr<Instance>& inst : instances)
	{
		if(inst)
		{
			j["instances"].push_back(inst->CreateJSON());
		}
	}
	
	return j;
}
