#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SessionEngine
{
    class IInstanceRecord
    {
    public:
        virtual ~IInstanceRecord() = default;
        virtual std::string GetInstanceId() const = 0;
        virtual std::string GetEntityId() const = 0;
        virtual std::optional<std::string> GetAttributeValue(const std::string& attributeId) const = 0;
        virtual void SetAttributeValue(const std::string& attributeId, const std::string& value) = 0;
        virtual bool HasAttribute(const std::string& attributeId) const = 0;
        virtual std::unordered_map<std::string, std::string> GetAllAttributes() const = 0;
        virtual std::vector<std::string> GetLinkedInstanceIds(const std::string& relationshipId) const = 0;
        virtual void AddRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId) = 0;
        virtual void RemoveRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId) = 0;
    };

    class ISessionState
    {
    public:
        virtual ~ISessionState() = default;
        virtual std::shared_ptr<IInstanceRecord> GetInstance(const std::string& entityId, const std::string& instanceId) = 0;
        virtual std::shared_ptr<IInstanceRecord> CreateInstance(const std::string& entityId, const std::string& parentInstanceId = "", const std::string& relationshipId = "") = 0;
        virtual bool DeleteInstance(const std::string& entityId, const std::string& instanceId) = 0;
        virtual std::vector<std::shared_ptr<IInstanceRecord>> GetAllInstances(const std::string& entityId) const = 0;
        virtual std::vector<std::string> GetEntityIds() const = 0;
        virtual void CopyInstanceData(const std::string& sourceInstanceId, const std::string& targetInstanceId) = 0;
        virtual std::string SerializeToJson() const = 0;
        virtual bool DeserializeFromJson(const std::string& jsonString) = 0;
        virtual void Clear() = 0;
    };
}
