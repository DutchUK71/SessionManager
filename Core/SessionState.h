#pragma once

#include "ISessionState.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SessionEngine
{
    class InstanceRecord : public IInstanceRecord
    {
    public:
        InstanceRecord(std::string entityId, std::string instanceId);
        ~InstanceRecord() override = default;

        std::string GetInstanceId() const override;
        std::string GetEntityId() const override;
        std::optional<std::string> GetAttributeValue(const std::string& attributeId) const override;
        void SetAttributeValue(const std::string& attributeId, const std::string& value) override;
        bool HasAttribute(const std::string& attributeId) const override;
        std::unordered_map<std::string, std::string> GetAllAttributes() const override;
        std::vector<std::string> GetLinkedInstanceIds(const std::string& relationshipId) const override;
        void AddRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId) override;
        void RemoveRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId) override;

        std::unordered_map<std::string, std::vector<std::string>> GetAllRelationshipLinks() const;
        void RemoveAllLinksTo(const std::string& targetInstanceId);

    private:
        std::string entityId_;
        std::string instanceId_;
        std::unordered_map<std::string, std::string> attributes_;
        std::unordered_map<std::string, std::vector<std::string>> relationshipLinks_;
        mutable std::mutex mutex_;
    };

    class SessionState : public ISessionState
    {
    public:
        SessionState() = default;
        ~SessionState() override = default;

        std::shared_ptr<IInstanceRecord> GetInstance(const std::string& entityId, const std::string& instanceId) override;
        std::shared_ptr<IInstanceRecord> CreateInstance(const std::string& entityId, const std::string& parentInstanceId = "", const std::string& relationshipId = "") override;
        std::shared_ptr<IInstanceRecord> CreateInstanceWithId(const std::string& entityId, const std::string& instanceId, const std::string& parentInstanceId = "", const std::string& relationshipId = "");
        bool DeleteInstance(const std::string& entityId, const std::string& instanceId) override;
        std::vector<std::shared_ptr<IInstanceRecord>> GetAllInstances(const std::string& entityId) const override;
        std::vector<std::string> GetEntityIds() const override;
        void CopyInstanceData(const std::string& sourceInstanceId, const std::string& targetInstanceId) override;
        std::string SerializeToJson() const override;
        bool DeserializeFromJson(const std::string& jsonString) override;
        void Clear() override;

    private:
        static std::string GenerateId();

        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::vector<std::shared_ptr<InstanceRecord>>> instancesByEntity_;
        std::unordered_map<std::string, std::shared_ptr<InstanceRecord>> instancesById_;
    };
}
