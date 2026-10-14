#include "SessionState.h"
#include "json.hpp"
#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>

namespace SessionEngine
{
    // =========================================================================
    // InstanceRecord Implementation
    // =========================================================================

    InstanceRecord::InstanceRecord(std::string entityId, std::string instanceId)
        : entityId_(std::move(entityId)), instanceId_(std::move(instanceId))
    {
    }

    std::string InstanceRecord::GetInstanceId() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return instanceId_;
    }

    std::string InstanceRecord::GetEntityId() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return entityId_;
    }

    std::optional<std::string> InstanceRecord::GetAttributeValue(const std::string& attributeId) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = attributes_.find(attributeId);
        if (it != attributes_.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    void InstanceRecord::SetAttributeValue(const std::string& attributeId, const std::string& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        attributes_[attributeId] = value;
    }

    bool InstanceRecord::HasAttribute(const std::string& attributeId) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return attributes_.find(attributeId) != attributes_.end();
    }

    std::unordered_map<std::string, std::string> InstanceRecord::GetAllAttributes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return attributes_;
    }

    std::vector<std::string> InstanceRecord::GetLinkedInstanceIds(const std::string& relationshipId) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = relationshipLinks_.find(relationshipId);
        if (it != relationshipLinks_.end())
        {
            return it->second;
        }
        return {};
    }

    void InstanceRecord::AddRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& list = relationshipLinks_[relationshipId];
        if (std::find(list.begin(), list.end(), targetInstanceId) == list.end())
        {
            list.push_back(targetInstanceId);
        }
    }

    void InstanceRecord::RemoveRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = relationshipLinks_.find(relationshipId);
        if (it != relationshipLinks_.end())
        {
            auto& list = it->second;
            list.erase(std::remove(list.begin(), list.end(), targetInstanceId), list.end());
            if (list.empty())
            {
                relationshipLinks_.erase(it);
            }
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> InstanceRecord::GetAllRelationshipLinks() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return relationshipLinks_;
    }

    void InstanceRecord::RemoveAllLinksTo(const std::string& targetInstanceId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = relationshipLinks_.begin(); it != relationshipLinks_.end();)
        {
            auto& list = it->second;
            list.erase(std::remove(list.begin(), list.end(), targetInstanceId), list.end());
            if (list.empty())
            {
                it = relationshipLinks_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // =========================================================================
    // SessionState Implementation
    // =========================================================================

    std::string SessionState::GenerateId()
    {
        static thread_local std::random_device dev;
        static thread_local std::mt19937 gen(dev());
        static thread_local std::uniform_int_distribution<int> dis(0, 15);
        static thread_local std::uniform_int_distribution<int> dis2(8, 11);

        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; ++i) ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 4; ++i) ss << dis(gen);
        ss << "-4";
        for (int i = 0; i < 3; ++i) ss << dis(gen);
        ss << "-";
        ss << dis2(gen);
        for (int i = 0; i < 3; ++i) ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 12; ++i) ss << dis(gen);

        return ss.str();
    }

    std::shared_ptr<IInstanceRecord> SessionState::GetInstance(const std::string& entityId, const std::string& instanceId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instancesById_.find(instanceId);
        if (it != instancesById_.end())
        {
            if (entityId.empty() || it->second->GetEntityId() == entityId)
            {
                return it->second;
            }
        }
        return nullptr;
    }

    std::shared_ptr<IInstanceRecord> SessionState::CreateInstance(const std::string& entityId, const std::string& parentInstanceId, const std::string& relationshipId)
    {
        std::string newId = GenerateId();
        return CreateInstanceWithId(entityId, newId, parentInstanceId, relationshipId);
    }

    std::shared_ptr<IInstanceRecord> SessionState::CreateInstanceWithId(const std::string& entityId, const std::string& instanceId, const std::string& parentInstanceId, const std::string& relationshipId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto instance = std::make_shared<InstanceRecord>(entityId, instanceId);
        instancesById_[instanceId] = instance;
        instancesByEntity_[entityId].push_back(instance);

        if (!parentInstanceId.empty())
        {
            auto parentIt = instancesById_.find(parentInstanceId);
            if (parentIt != instancesById_.end())
            {
                parentIt->second->AddRelationshipLink(relationshipId, instanceId);
            }
        }

        return instance;
    }

    bool SessionState::DeleteInstance(const std::string& entityId, const std::string& instanceId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instancesById_.find(instanceId);
        if (it == instancesById_.end())
        {
            return false;
        }

        // Remove from entity map
        auto entIt = instancesByEntity_.find(entityId.empty() ? it->second->GetEntityId() : entityId);
        if (entIt != instancesByEntity_.end())
        {
            auto& vec = entIt->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                [&instanceId](const std::shared_ptr<InstanceRecord>& rec) {
                    return rec->GetInstanceId() == instanceId;
                }), vec.end());
            if (vec.empty())
            {
                instancesByEntity_.erase(entIt);
            }
        }

        instancesById_.erase(it);

        // Remove any links pointing to this instance in all other instances
        for (const auto& pair : instancesById_)
        {
            pair.second->RemoveAllLinksTo(instanceId);
        }

        return true;
    }

    std::vector<std::shared_ptr<IInstanceRecord>> SessionState::GetAllInstances(const std::string& entityId) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<IInstanceRecord>> result;
        auto it = instancesByEntity_.find(entityId);
        if (it != instancesByEntity_.end())
        {
            for (const auto& inst : it->second)
            {
                result.push_back(inst);
            }
        }
        return result;
    }

    std::vector<std::string> SessionState::GetEntityIds() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        for (const auto& pair : instancesByEntity_)
        {
            result.push_back(pair.first);
        }
        return result;
    }

    void SessionState::CopyInstanceData(const std::string& sourceInstanceId, const std::string& targetInstanceId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto srcIt = instancesById_.find(sourceInstanceId);
        auto tgtIt = instancesById_.find(targetInstanceId);
        if (srcIt != instancesById_.end() && tgtIt != instancesById_.end())
        {
            auto srcAttrs = srcIt->second->GetAllAttributes();
            for (const auto& [key, val] : srcAttrs)
            {
                tgtIt->second->SetAttributeValue(key, val);
            }
        }
    }

    std::string SessionState::SerializeToJson() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json root = nlohmann::json::object();
        nlohmann::json entitiesArray = nlohmann::json::array();

        for (const auto& [entityId, instances] : instancesByEntity_)
        {
            nlohmann::json entityObj = nlohmann::json::object();
            entityObj["entityId"] = entityId;
            nlohmann::json instancesArray = nlohmann::json::array();

            for (const auto& inst : instances)
            {
                nlohmann::json instObj = nlohmann::json::object();
                instObj["instanceId"] = inst->GetInstanceId();
                instObj["attributes"] = inst->GetAllAttributes();
                instObj["relationships"] = inst->GetAllRelationshipLinks();
                instancesArray.push_back(instObj);
            }

            entityObj["instances"] = instancesArray;
            entitiesArray.push_back(entityObj);
        }

        root["entities"] = entitiesArray;
        return root.dump();
    }

    bool SessionState::DeserializeFromJson(const std::string& jsonString)
    {
        try
        {
            nlohmann::json root = nlohmann::json::parse(jsonString);
            if (!root.contains("entities") || !root["entities"].is_array())
            {
                return false;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            instancesByEntity_.clear();
            instancesById_.clear();

            for (const auto& entityObj : root["entities"])
            {
                if (!entityObj.contains("entityId") || !entityObj.contains("instances"))
                {
                    continue;
                }

                std::string entityId = entityObj["entityId"].get<std::string>();
                for (const auto& instObj : entityObj["instances"])
                {
                    if (!instObj.contains("instanceId"))
                    {
                        continue;
                    }
                    std::string instanceId = instObj["instanceId"].get<std::string>();
                    auto instance = std::make_shared<InstanceRecord>(entityId, instanceId);

                    if (instObj.contains("attributes") && instObj["attributes"].is_object())
                    {
                        for (auto it = instObj["attributes"].begin(); it != instObj["attributes"].end(); ++it)
                        {
                            if (it.value().is_string())
                            {
                                instance->SetAttributeValue(it.key(), it.value().get<std::string>());
                            }
                        }
                    }

                    if (instObj.contains("relationships") && instObj["relationships"].is_object())
                    {
                        for (auto it = instObj["relationships"].begin(); it != instObj["relationships"].end(); ++it)
                        {
                            if (it.value().is_array())
                            {
                                for (const auto& targetId : it.value())
                                {
                                    if (targetId.is_string())
                                    {
                                        instance->AddRelationshipLink(it.key(), targetId.get<std::string>());
                                    }
                                }
                            }
                        }
                    }

                    instancesById_[instanceId] = instance;
                    instancesByEntity_[entityId].push_back(instance);
                }
            }

            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    void SessionState::Clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        instancesByEntity_.clear();
        instancesById_.clear();
    }
}
