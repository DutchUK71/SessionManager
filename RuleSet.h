#pragma once

#include "Attribute.h"
#include "Rule.h"
#include <vector>
#include <unordered_map>
#include "Entity.h"
#include "Relationship.h"
#include "json.hpp"

using json = nlohmann::json;

struct ScreenLocation
{
	string screenId;
	string entityId;
	string instanceId;
};

class RuleSet
{
	public:
	struct UploadInfo
	{
		string fileId;
		string uploadControlId;
		string fileName;
		string fileExtension;
		string fileSize;
		unsigned long long fileSizeBytes = 0;
		string instanceId;
		string parentInstanceId;
		string entityId;
		string relationshipId;
		bool externallyUploaded = false;
	};

	struct PaginationInfo
	{
		string elementId;
		int currentPage = -1;
		string filter = "";
	};

	struct SessionContext
	{
		string sessionStartDateTime;
		string userId;
		string userName;
		string currentLocale = "en-GB";
		string deploymentName;
		string deploymentVersion;
		string asOfTransactionTime;
	};

	struct ExternalSecurityPolicy
	{
		bool enabled = false;
		string provider;
		string approvedProfileKey;
		string issuerUrl;
		string discoveryUrl;
		string authorizationUrl;
		string logoutUrl;
		string tokenUrl;
		string jwksUrl;
		string introspectionUrl;
		string tokenExchangeUrl;
		string userInfoUrl;
		string audience;
		vector<string> scopes;
		string returnRouteIdentifier;
	};

	struct NotifyPolicy
	{
		bool enabled = false;
		string provider;
		string approvedProfileKey;
		string defaultReplyToEmailId;
	};

	struct StartInputDefinition
	{
		string entityId;
		string attributeId;
		string attributeName;
		string coreName;
		string attributeType;
		bool required = true;
	};

	struct ValidationEvent
	{
		string eventId;
		string severity;
		string ruleId;
		string ruleName;
		string entityId;
		string instanceId;
		string message;
		bool messageKnown = true;
		string messageKnowledgeState = "Certain";
	};

	struct AttributeHistorySegment
	{
		string entityId;
		string instanceId;
		string attributeId;
		string effectiveStart;
		string effectiveEnd;
		string value;
		Attribute::AttributeType valueType = Attribute::UNKNOWN;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain;
		string source;
		string transactionStart;
		string transactionEnd;
		string assertedAt;
		string evaluationTime;
		string modelTimeZone;
		string provenance;
	};

	struct EntityExistenceHistorySegment
	{
		string entityId;
		string instanceId;
		string effectiveStart;
		string effectiveEnd;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain;
		string source;
		string transactionStart;
		string transactionEnd;
		string assertedAt;
		string evaluationTime;
		string modelTimeZone;
		string provenance;
	};

	struct RelationshipLinkHistorySegment
	{
		string relationshipId;
		string sourceEntityId;
		string sourceInstanceId;
		string targetEntityId;
		string targetInstanceId;
		string effectiveStart;
		string effectiveEnd;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain;
		string source;
		string transactionStart;
		string transactionEnd;
		string assertedAt;
		string evaluationTime;
		string modelTimeZone;
		string provenance;
	};

	struct RelationshipCoverageHistorySegment
	{
		string relationshipId;
		string sourceInstanceId;
		string effectiveStart;
		string effectiveEnd;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain;
		string source;
		string transactionStart;
		string transactionEnd;
		string assertedAt;
		string evaluationTime;
		string modelTimeZone;
		string provenance;
	};

	struct RelationshipDefinitionLocation
	{
		size_t entityIndex = 0;
		size_t relationshipIndex = 0;
	};

	struct SessionModelRuleReference
	{
		string source;
		string entityId;
		string attributeId;
		string relationshipId;
		string nodeType;
		string operatorName;
		string functionName;
	};

	struct DecisionTableConclusionCellMetadata
	{
		size_t cellIndex = 0;
		bool isObject = false;
		string attributeId;
		string relationshipId;
		string valueMode;
		string value;
	};

	struct DecisionTableRowMetadata
	{
		size_t rowIndex = 0;
		bool isObject = false;
		bool conditionCellsArray = false;
		bool conditionCellsEmpty = true;
		size_t conditionCellCount = 0;
		bool conclusionCellsArray = false;
		size_t conclusionCellCount = 0;
		vector<DecisionTableConclusionCellMetadata> conclusionCells;
	};

	struct SessionModelRuleMetadata
	{
		string ruleId;
		string ruleName;
		bool isActive = false;
		string conclusionMode;
		string conclusionEntityId;
		string conclusionAttributeId;
		string conclusionValue;
		string conclusionValueMode;
		json conclusionExpression;
		string elseValue;
		string relationshipId;
		bool hasExecutableDecisionTable = false;
		int decisionTableRows = 0;
		json condition;
		json executableDecisionTable;
		bool decisionTableMetadataBuilt = false;
		vector<DecisionTableRowMetadata> decisionTableRowMetadata;
		vector<SessionModelRuleReference> references;

		void RebuildDecisionTableMetadata();
	};

	struct DataConnectionSource
	{
		string sourceId;
		string sourceName;
		string sourceType;
		string externalObjectName;
		string databaseProvider;
		string serverConnectionKey;
	};

	struct DataConnectionAttributeMapping
	{
		string attributeId;
		string attributeName;
		string attributeCoreName;
		string attributeDataType;
		string sourceFieldName;
		bool input = false;
		bool output = false;
		bool loadFilter = false;
		bool hasLoadFilterMetadata = false;
		string loadFilterOperator = "Equals";
		string loadFilterValueSource = "CurrentRuntimeValue";
		bool saveConflictCheck = false;
		string valueGeneration;
		bool generatedOnCreate = false;
	};

	struct DataConnectionRelationshipFieldMapping
	{
		string relationshipId;
		string relationshipName;
		string sourceFieldName;
		string relatedEntityId;
		string relatedEntityName;
		string relatedAttributeId;
		string relatedAttributeName;
		string relatedAttributeCoreName;
		string relatedAttributeDataType;
		bool output = true;
	};

	struct DataConnectionEntityMapping
	{
		string entityId;
		string entityName;
		string sourceId;
		string sourceName;
		string sourceType;
		string sourceObjectName;
		string saveOperationPolicy = "UpdateOnly";
		vector<DataConnectionAttributeMapping> attributeMappings;
		vector<DataConnectionRelationshipFieldMapping> relationshipFieldMappings;
	};

	struct DataConnectionSaveDiagnostic
	{
		string severity;
		string code;
		string message;
		string context;
	};

	struct DataConnectionSaveField
	{
		string attributeId;
		string attributeName;
		string attributeCoreName;
		string attributeDataType;
		string sourceFieldName;
		string valueSource = "CurrentRuntimeValue";
		string entityId;
		string entityName;
		string instanceId;
		string relationshipId;
		string relationshipName;
		string valueType;
		json value;
		bool isKnown = false;
		bool isCertain = false;
		bool isUncertain = false;
		string knowledgeState;
		string valueGeneration;
		bool generatedOnCreate = false;
	};

	struct DataConnectionSaveFilter
	{
		string sourceFieldName;
		string operatorName = "Equals";
		string filterRole = "RecordIdentity";
		string valueSource = "CurrentRuntimeValue";
		string entityId;
		string entityName;
		string instanceId;
		string attributeId;
		string attributeName;
		string attributeCoreName;
		string attributeDataType;
		string valueType;
		json value;
		bool isKnown = false;
		bool isCertain = false;
		bool isUncertain = false;
		string knowledgeState;
	};

	struct DataConnectionRecordIdentitySnapshot
	{
		string sourceId;
		string sourceName;
		string serverConnectionKey;
		string sourceObjectName;
		string snapshotRole = "RecordIdentity";
		string entityId;
		string entityName;
		string instanceId;
		string attributeId;
		string attributeName;
		string attributeCoreName;
		string attributeDataType;
		string sourceFieldName;
		string operatorName = "Equals";
		string valueType;
		json value;
		bool isKnown = false;
		bool isCertain = false;
		bool isUncertain = false;
		string knowledgeState;
	};

	struct DataConnectionSaveRow
	{
		string entityId;
		string entityName;
		string instanceId;
		string saveOperationPolicy = "UpdateOnly";
		bool createIfMissing = false;
		vector<DataConnectionSaveField> fields;
		vector<DataConnectionSaveFilter> filters;
	};

	struct DataConnectionSaveTable
	{
		string sourceId;
		string sourceName;
		string sourceType;
		string databaseProvider;
		string serverConnectionKey;
		string sourceObjectName;
		vector<DataConnectionSaveRow> rows;
	};

	struct DataConnectionSavePlan
	{
		vector<DataConnectionSaveTable> tables;
		vector<DataConnectionSaveDiagnostic> diagnostics;

		bool HasErrors() const
		{
			for(const DataConnectionSaveDiagnostic& diagnostic : diagnostics)
			{
				if(diagnostic.severity == "error")
				{
					return true;
				}
			}

			return false;
		}

		int CountDiagnostics(const string& code) const
		{
			int count = 0;

			for(const DataConnectionSaveDiagnostic& diagnostic : diagnostics)
			{
				if(diagnostic.code == code)
				{
					count++;
				}
			}

			return count;
		}

		json ToJSON() const;
	};

	struct DataConnectionLoadDiagnostic
	{
		string severity;
		string code;
		string message;
		string context;
	};

	struct DataConnectionLoadTarget
	{
		string entityId;
		string entityName;
		string attributeId;
		string attributeName;
		string attributeCoreName;
		string attributeDataType;
		string valueType;
	};

	struct DataConnectionLoadField
	{
		string sourceFieldName;
		vector<DataConnectionLoadTarget> targets;
	};

	struct DataConnectionLoadFilter
	{
		string sourceFieldName;
		string operatorName = "Equals";
		string valueSource = "CurrentRuntimeValue";
		string entityId;
		string entityName;
		string instanceId;
		string attributeId;
		string attributeName;
		string attributeCoreName;
		string attributeDataType;
		string valueType;
		json value;
		bool isKnown = false;
		bool isCertain = false;
		bool isUncertain = false;
		string knowledgeState;
	};

	struct DataConnectionLoadTable
	{
		string sourceId;
		string sourceName;
		string sourceType;
		string databaseProvider;
		string serverConnectionKey;
		string sourceObjectName;
		vector<DataConnectionLoadField> fields;
		vector<DataConnectionLoadFilter> filters;
	};

	struct DataConnectionLoadPlan
	{
		vector<DataConnectionLoadTable> tables;
		vector<DataConnectionLoadDiagnostic> diagnostics;

		bool HasErrors() const
		{
			for(const DataConnectionLoadDiagnostic& diagnostic : diagnostics)
			{
				if(diagnostic.severity == "error")
				{
					return true;
				}
			}

			return false;
		}

		int CountDiagnostics(const string& code) const
		{
			int count = 0;

			for(const DataConnectionLoadDiagnostic& diagnostic : diagnostics)
			{
				if(diagnostic.code == code)
				{
					count++;
				}
			}

			return count;
		}

		json ToJSON() const;
	};

	shared_ptr<vector<ScreenLocation>> screenLocations = make_shared<vector<ScreenLocation>>();

	shared_ptr<vector<shared_ptr<Entity>>> entities = make_shared<vector<shared_ptr<Entity>>>();

	shared_ptr<vector<StartInputDefinition>> startInputs = make_shared<vector<StartInputDefinition>>();

	shared_ptr<vector<UploadInfo>> uploads = make_shared<vector<UploadInfo>>();

	shared_ptr<vector<PaginationInfo>> paginations = make_shared<vector<PaginationInfo>>();

	shared_ptr<vector<DataConnectionSource>> dataConnectionSources = make_shared<vector<DataConnectionSource>>();

	shared_ptr<vector<DataConnectionEntityMapping>> dataConnectionEntityMappings = make_shared<vector<DataConnectionEntityMapping>>();

	shared_ptr<vector<DataConnectionRecordIdentitySnapshot>> dataConnectionRecordIdentitySnapshots = make_shared<vector<DataConnectionRecordIdentitySnapshot>>();

	shared_ptr<vector<SessionModelRuleMetadata>> sessionModelRules = make_shared<vector<SessionModelRuleMetadata>>();

	shared_ptr<vector<json>> pdfTemplates = make_shared<vector<json>>();

	shared_ptr<vector<json>> emailTemplates = make_shared<vector<json>>();

	bool forgetPolicyEnabled = false;

	shared_ptr<unordered_map<string, string>> attributeForgetPolicyModeByKey = make_shared<unordered_map<string, string>>();

	shared_ptr<unordered_map<string, string>> relationshipForgetPolicyModeById = make_shared<unordered_map<string, string>>();

	// Retained values are deliberately isolated from active instances/rules. Keys are stable and make repeated cleanup idempotent.
	shared_ptr<unordered_map<string, json>> retainedForgetDataByKey = make_shared<unordered_map<string, json>>();

	shared_ptr<unordered_map<string, size_t>> sessionModelRuleIndexById = make_shared<unordered_map<string, size_t>>();

	mutable shared_ptr<unordered_map<string, RelationshipDefinitionLocation>> relationshipDefinitionIndexById = make_shared<unordered_map<string, RelationshipDefinitionLocation>>();

	mutable shared_ptr<unordered_map<string, RelationshipDefinitionLocation>> relationshipDefinitionIndexByScope = make_shared<unordered_map<string, RelationshipDefinitionLocation>>();

	mutable size_t relationshipDefinitionIndexEntityCount = static_cast<size_t>(-1);

	mutable size_t relationshipDefinitionIndexRelationshipCount = static_cast<size_t>(-1);

	shared_ptr<unordered_map<string, vector<string>>> activeSessionModelRuleIdsByReferencedAttribute = make_shared<unordered_map<string, vector<string>>>();

	shared_ptr<unordered_map<string, vector<string>>> activeSessionModelRuleIdsByReferencedRelationship = make_shared<unordered_map<string, vector<string>>>();

	shared_ptr<unordered_map<string, vector<string>>> activeSessionModelRuleIdsByReferencedFunction = make_shared<unordered_map<string, vector<string>>>();

	shared_ptr<unordered_map<string, vector<string>>> activeSessionModelRuleIdsByConcludedAttribute = make_shared<unordered_map<string, vector<string>>>();

	shared_ptr<unordered_map<string, string>> inferredInstanceIdsByKey = make_shared<unordered_map<string, string>>();

	shared_ptr<unordered_map<string, ValidationEvent>> activeValidationEventsByKey = make_shared<unordered_map<string, ValidationEvent>>();

	shared_ptr<vector<AttributeHistorySegment>> attributeHistorySegments = make_shared<vector<AttributeHistorySegment>>();

	shared_ptr<unordered_map<string, vector<size_t>>> attributeHistorySegmentIndexesByAttribute = make_shared<unordered_map<string, vector<size_t>>>();

	shared_ptr<vector<EntityExistenceHistorySegment>> entityExistenceHistorySegments = make_shared<vector<EntityExistenceHistorySegment>>();

	shared_ptr<unordered_map<string, vector<size_t>>> entityExistenceHistorySegmentIndexesByInstance = make_shared<unordered_map<string, vector<size_t>>>();

	shared_ptr<vector<RelationshipLinkHistorySegment>> relationshipLinkHistorySegments = make_shared<vector<RelationshipLinkHistorySegment>>();

	shared_ptr<unordered_map<string, vector<size_t>>> relationshipLinkHistorySegmentIndexesBySource = make_shared<unordered_map<string, vector<size_t>>>();

	shared_ptr<unordered_map<string, vector<size_t>>> relationshipLinkHistorySegmentIndexesByTarget = make_shared<unordered_map<string, vector<size_t>>>();

	shared_ptr<unordered_map<string, vector<size_t>>> relationshipLinkHistorySegmentIndexesByLink = make_shared<unordered_map<string, vector<size_t>>>();

	shared_ptr<vector<RelationshipCoverageHistorySegment>> relationshipCoverageHistorySegments = make_shared<vector<RelationshipCoverageHistorySegment>>();

	shared_ptr<unordered_map<string, vector<size_t>>> relationshipCoverageHistorySegmentIndexesBySource = make_shared<unordered_map<string, vector<size_t>>>();

	SessionContext sessionContext;

	ExternalSecurityPolicy externalSecurity;

	NotifyPolicy notify;

	RuleSet();

	void AddScreenLocation(const ScreenLocation& location);

	void RemoveLastScreenLocation();

	ScreenLocation GetLatestScreenLocation(string screenId);

	ScreenLocation GetPreviousScreenLocation();

	// Create a function to translate the current state of the ruleset to a JSON object that can be sent to the client
	json ToJSON() const;

	DataConnectionSavePlan BuildDataConnectionSavePlan() const;

	DataConnectionLoadPlan BuildDataConnectionLoadPlan() const;

	DataConnectionLoadPlan BuildDataConnectionDeletedInstanceDeletePlan() const;

	int ClearDataConnectionSnapshotsForMissingInstances();

	void ResetSessionStartDateTime();

	void SetSessionStartDateTime(const string& value);

	void SetSessionMetadata(const string& currentLocale, const string& deploymentName, const string& deploymentVersion);

	void SetHistorySnapshotTime(const string& value);

	string GetHistorySnapshotTime() const;

	string GetSessionCurrentDate() const;

	string GetSessionCurrentDateTime() const;

	string GetSessionCurrentTime() const;

	static string AttributeDependencyKey(const string& entityId, const string& attributeId);

	string GetAttributeForgetPolicyMode(const string& entityId, const string& attributeId) const;

	string GetRelationshipForgetPolicyMode(const string& relationshipId) const;

	static string AttributeHistoryDependencyKey(const string& instanceId, const string& attributeId);

	static bool IsValidTransactionTime(const string& value);

	static bool IsValidTransactionWindow(const string& transactionStart, const string& transactionEnd);

	static string EntityExistenceHistoryDependencyKey(const string& entityId, const string& instanceId);

	static string RelationshipLinkHistorySourceDependencyKey(const string& relationshipId, const string& sourceInstanceId);

	static string RelationshipLinkHistoryTargetDependencyKey(const string& relationshipId, const string& targetInstanceId);

	static string RelationshipLinkHistoryDependencyKey(const string& relationshipId, const string& sourceInstanceId, const string& targetInstanceId);

	static string RelationshipCoverageHistoryDependencyKey(const string& relationshipId, const string& sourceInstanceId);

	void AddAttributeHistorySegment(const AttributeHistorySegment& segment);

	void RebuildAttributeHistoryIndex();

	vector<const AttributeHistorySegment*> GetAttributeHistorySegments(const string& instanceId, const string& attributeId) const;

	void AddEntityExistenceHistorySegment(const EntityExistenceHistorySegment& segment);

	void RebuildEntityExistenceHistoryIndex();

	vector<const EntityExistenceHistorySegment*> GetEntityExistenceHistorySegments(const string& entityId, const string& instanceId) const;

	void AddRelationshipLinkHistorySegment(const RelationshipLinkHistorySegment& segment);

	void RebuildRelationshipLinkHistoryIndex();

	vector<const RelationshipLinkHistorySegment*> GetRelationshipLinkHistorySegments(const string& relationshipId, const string& sourceInstanceId) const;

	vector<const RelationshipLinkHistorySegment*> GetRelationshipLinkHistorySegmentsByTarget(const string& relationshipId, const string& targetInstanceId) const;

	vector<const RelationshipLinkHistorySegment*> GetRelationshipLinkHistorySegments(const string& relationshipId, const string& sourceInstanceId, const string& targetInstanceId) const;

	void AddRelationshipCoverageHistorySegment(const RelationshipCoverageHistorySegment& segment);

	void RebuildRelationshipCoverageHistoryIndex();

	vector<const RelationshipCoverageHistorySegment*> GetRelationshipCoverageHistorySegments(const string& relationshipId, const string& sourceInstanceId) const;

	void RebuildSessionModelRuleIndex();

	const SessionModelRuleMetadata* GetSessionModelRuleById(const string& ruleId) const;

	void RebuildRelationshipDefinitionIndex() const;

	shared_ptr<Relationship> GetRelationshipDefinitionById(const string& relationshipId) const;

	shared_ptr<Relationship> GetRelationshipDefinitionByScope(const string& scope) const;

	const vector<string>* FindActiveSessionModelRulesReferencingAttribute(const string& entityId, const string& attributeId) const;

	vector<string> GetActiveSessionModelRulesReferencingAttribute(const string& entityId, const string& attributeId) const;

	const vector<string>* FindActiveSessionModelRulesReferencingRelationship(const string& relationshipId) const;

	vector<string> GetActiveSessionModelRulesReferencingRelationship(const string& relationshipId) const;

	const vector<string>* FindActiveSessionModelRulesReferencingFunction(const string& functionName) const;

	vector<string> GetActiveSessionModelRulesReferencingFunction(const string& functionName) const;

	vector<string> GetActiveSessionModelRulesConcludingAttribute(const string& entityId, const string& attributeId) const;

	PaginationInfo& GetPaginationInfo(const string& elementId)
	{
		for(PaginationInfo& page : *paginations)
		{
			if(page.elementId == elementId)
			{
				return page;
			}
		}
		
		PaginationInfo newInfo;
		newInfo.elementId = elementId;

		paginations->push_back(newInfo);

		return paginations->back();
	}
};
