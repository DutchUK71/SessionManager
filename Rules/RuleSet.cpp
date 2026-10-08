#include "RuleSet.h"
#include "SessionModelExpressionParserSupport.h"
#include "SessionModelTemporalSupport.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace
{
	std::string CurrentLocalDateTime()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t time = std::chrono::system_clock::to_time_t(now);
		std::tm localTime{};

#ifdef _WIN32
		localtime_s(&localTime, &time);
#else
		localtime_r(&time, &localTime);
#endif

		std::ostringstream stream;

		stream << std::put_time(&localTime, "%Y-%m-%dT%H:%M:%S");

		return stream.str();
	}

	std::string AttributeTypeName(int type)
	{
		switch(static_cast<Attribute::AttributeType>(type))
		{
			case Attribute::INTEGER:
				return "Integer";
			case Attribute::DOUBLE:
				return "Number";
			case Attribute::STRING:
				return "Text";
			case Attribute::BOOLEAN:
				return "Boolean";
			case Attribute::DATE:
				return "Date";
			case Attribute::DATETIME:
				return "DateTime";
			case Attribute::TIMEOFDAY:
				return "TimeOfDay";
			case Attribute::UNKNOWN:
			default:
				return "Unknown";
		}
	}

	size_t RelationshipDefinitionCount(const RuleSet& ruleSet)
	{
		if(ruleSet.entities == nullptr)
		{
			return 0;
		}

		size_t count = 0;

		for(const shared_ptr<Entity>& entity : *ruleSet.entities)
		{
			if(entity != nullptr)
			{
				count += entity->relationships.size();
			}
		}

		return count;
	}

	shared_ptr<Relationship> ResolveRelationshipDefinitionLocation(
		const RuleSet& ruleSet,
		const RuleSet::RelationshipDefinitionLocation& location)
	{
		if(ruleSet.entities == nullptr || location.entityIndex >= ruleSet.entities->size())
		{
			return nullptr;
		}

		const shared_ptr<Entity>& entity = ruleSet.entities->at(location.entityIndex);

		if(entity == nullptr || location.relationshipIndex >= entity->relationships.size())
		{
			return nullptr;
		}

		return entity->relationships[location.relationshipIndex];
	}

	bool RelationshipDefinitionMatchesScope(const shared_ptr<Relationship>& relationship, const string& normalizedScope)
	{
		if(relationship == nullptr || normalizedScope.empty())
		{
			return false;
		}

		return SessionModelExpressionParserSupport::NormalizeLookup(relationship->id) == normalizedScope
			|| SessionModelExpressionParserSupport::NormalizeLookup(relationship->description) == normalizedScope;
	}

	json AttributeValueToJson(const shared_ptr<Attribute>& attribute)
	{
		if(attribute == nullptr || attribute->IsUnknown())
		{
			return nullptr;
		}

		switch(static_cast<Attribute::AttributeType>(attribute->GetType()))
		{
			case Attribute::INTEGER:
				return std::get<int>(attribute->GetValue());
			case Attribute::DOUBLE:
				return std::get<double>(attribute->GetValue());
			case Attribute::BOOLEAN:
				return std::get<bool>(attribute->GetValue());
			case Attribute::STRING:
			case Attribute::DATE:
			case Attribute::DATETIME:
			case Attribute::TIMEOFDAY:
				return attribute->GetStringValue();
			case Attribute::UNKNOWN:
			default:
				return nullptr;
		}
	}

	void AddSavePlanDiagnostic(RuleSet::DataConnectionSavePlan& plan,
	                           const string& severity,
	                           const string& code,
	                           const string& message,
	                           const string& context)
	{
		RuleSet::DataConnectionSaveDiagnostic diagnostic;

		diagnostic.severity = severity;
		diagnostic.code = code;
		diagnostic.message = message;
		diagnostic.context = context;

		plan.diagnostics.push_back(diagnostic);
	}

	void AddLoadPlanDiagnostic(RuleSet::DataConnectionLoadPlan& plan,
	                           const string& severity,
	                           const string& code,
	                           const string& message,
	                           const string& context)
	{
		RuleSet::DataConnectionLoadDiagnostic diagnostic;

		diagnostic.severity = severity;
		diagnostic.code = code;
		diagnostic.message = message;
		diagnostic.context = context;

		plan.diagnostics.push_back(diagnostic);
	}

	const RuleSet::DataConnectionSource* FindDataConnectionSource(const vector<RuleSet::DataConnectionSource>& sources,
	                                                              const string& sourceId)
	{
		for(const RuleSet::DataConnectionSource& source : sources)
		{
			if(source.sourceId == sourceId)
			{
				return &source;
			}
		}

		return nullptr;
	}

	const RuleSet::DataConnectionEntityMapping* FindDataConnectionEntityMapping(
		const vector<RuleSet::DataConnectionEntityMapping>& mappings,
		const string& entityId)
	{
		for(const RuleSet::DataConnectionEntityMapping& mapping : mappings)
		{
			if(mapping.entityId == entityId)
			{
				return &mapping;
			}
		}

		return nullptr;
	}

	const RuleSet::DataConnectionAttributeMapping* FindDataConnectionAttributeMapping(
		const RuleSet::DataConnectionEntityMapping& mapping,
		const string& attributeId)
	{
		for(const RuleSet::DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
		{
			if(attributeMapping.attributeId == attributeId)
			{
				return &attributeMapping;
			}
		}

		return nullptr;
	}

	shared_ptr<Entity> FindEntityById(const shared_ptr<vector<shared_ptr<Entity>>>& entities,
	                                  const string& entityId)
	{
		if(entities == nullptr)
		{
			return nullptr;
		}

		for(const shared_ptr<Entity>& entity : *entities)
		{
			if(entity != nullptr && entity->entityid == entityId)
			{
				return entity;
			}
		}

		return nullptr;
	}

	shared_ptr<Attribute> FindEntityAttributeById(const shared_ptr<Entity>& entity,
	                                              const string& attributeId)
	{
		if(entity == nullptr)
		{
			return nullptr;
		}

		for(const shared_ptr<Attribute>& attribute : entity->attributes)
		{
			if(attribute != nullptr && attribute->GetID() == attributeId)
			{
				return attribute;
			}
		}

		return nullptr;
	}

	RuleSet::DataConnectionSaveTable& FindOrCreateSaveTable(RuleSet::DataConnectionSavePlan& plan,
	                                                        const RuleSet::DataConnectionEntityMapping& mapping,
	                                                        const RuleSet::DataConnectionSource* source)
	{
		for(RuleSet::DataConnectionSaveTable& table : plan.tables)
		{
			if(table.sourceId == mapping.sourceId && table.sourceObjectName == mapping.sourceObjectName)
			{
				return table;
			}
		}

		RuleSet::DataConnectionSaveTable table;

		table.sourceId = mapping.sourceId;
		table.sourceName = source != nullptr ? source->sourceName : mapping.sourceName;
		table.sourceType = source != nullptr ? source->sourceType : mapping.sourceType;
		table.databaseProvider = source != nullptr ? source->databaseProvider : "";
		table.serverConnectionKey = source != nullptr ? source->serverConnectionKey : "";
		table.sourceObjectName = mapping.sourceObjectName;

		plan.tables.push_back(table);

		return plan.tables.back();
	}

	RuleSet::DataConnectionLoadTable& FindOrCreateLoadTable(RuleSet::DataConnectionLoadPlan& plan,
	                                                        const RuleSet::DataConnectionEntityMapping& mapping,
	                                                        const RuleSet::DataConnectionSource* source)
	{
		for(RuleSet::DataConnectionLoadTable& table : plan.tables)
		{
			if(table.sourceId == mapping.sourceId && table.sourceObjectName == mapping.sourceObjectName)
			{
				return table;
			}
		}

		RuleSet::DataConnectionLoadTable table;

		table.sourceId = mapping.sourceId;
		table.sourceName = source != nullptr ? source->sourceName : mapping.sourceName;
		table.sourceType = source != nullptr ? source->sourceType : mapping.sourceType;
		table.databaseProvider = source != nullptr ? source->databaseProvider : "";
		table.serverConnectionKey = source != nullptr ? source->serverConnectionKey : "";
		table.sourceObjectName = mapping.sourceObjectName;

		plan.tables.push_back(table);

		return plan.tables.back();
	}

	RuleSet::DataConnectionLoadField& FindOrCreateLoadField(RuleSet::DataConnectionLoadTable& table,
	                                                        const string& sourceFieldName)
	{
		for(RuleSet::DataConnectionLoadField& field : table.fields)
		{
			if(field.sourceFieldName == sourceFieldName)
			{
				return field;
			}
		}

		RuleSet::DataConnectionLoadField field;

		field.sourceFieldName = sourceFieldName;

		table.fields.push_back(field);

		return table.fields.back();
	}

	bool LoadTableHasFilterForField(const RuleSet::DataConnectionLoadTable& table,
	                                const string& sourceFieldName)
	{
		for(const RuleSet::DataConnectionLoadFilter& filter : table.filters)
		{
			if(filter.sourceFieldName == sourceFieldName)
			{
				return true;
			}
		}

		return false;
	}

	bool AttributeMappingUsesLoadFilter(const RuleSet::DataConnectionAttributeMapping& mapping)
	{
		return mapping.loadFilter || (!mapping.hasLoadFilterMetadata && mapping.input && mapping.output);
	}

	std::string NormalizeDataConnectionLoadFilterValueSource(const string& value)
	{
		if(value == "SessionUserId"
			|| value == "AuthenticatedUserId"
			|| value == "CurrentUserId")
		{
			return "SessionUserId";
		}

		return "CurrentRuntimeValue";
	}

	bool DataConnectionMappingCanUseSessionUserId(const string& valueType)
	{
		return valueType == "Text";
	}

	std::string NormalizeSaveOperationPolicy(const string& policy)
	{
		return policy == "Upsert" ? "Upsert" : "UpdateOnly";
	}

	bool SaveOperationPolicyAllowsInsertFallback(const string& policy)
	{
		return NormalizeSaveOperationPolicy(policy) == "Upsert";
	}

	bool SaveRowHasFilterForField(const RuleSet::DataConnectionSaveRow& row,
	                              const string& sourceFieldName)
	{
		for(const RuleSet::DataConnectionSaveFilter& filter : row.filters)
		{
			if(filter.sourceFieldName == sourceFieldName)
			{
				return true;
			}
		}

		return false;
	}

	bool SaveRowHasFieldForField(const RuleSet::DataConnectionSaveRow& row,
	                             const string& sourceFieldName)
	{
		for(const RuleSet::DataConnectionSaveField& field : row.fields)
		{
			if(field.sourceFieldName == sourceFieldName)
			{
				return true;
			}
		}

		return false;
	}

	bool SaveRowCanInsertWithGeneratedIdentity(const RuleSet::DataConnectionSaveRow& row)
	{
		if(!row.createIfMissing)
		{
			return false;
		}

		for(const RuleSet::DataConnectionSaveField& field : row.fields)
		{
			if(field.generatedOnCreate
				|| field.valueGeneration == "DatabaseGenerated"
				|| field.valueGeneration == "GeneratedOnCreate"
				|| field.valueGeneration == "AutoGenerated")
			{
				return true;
			}
		}

		return false;
	}

	bool IsPendingGeneratedRelationshipField(const RuleSet::DataConnectionSaveField& field)
	{
		return field.valueSource == "PendingGeneratedRelationshipValue";
	}

	bool GeneratedFieldMatchesPendingRelationship(const RuleSet::DataConnectionSaveRow& row,
	                                             const RuleSet::DataConnectionSaveField& generatedField,
	                                             const RuleSet::DataConnectionSaveField& pendingField)
	{
		if(row.entityId != pendingField.entityId || row.instanceId != pendingField.instanceId)
		{
			return false;
		}

		if(!row.createIfMissing || !generatedField.generatedOnCreate)
		{
			return false;
		}

		if(!pendingField.attributeId.empty() && generatedField.attributeId == pendingField.attributeId)
		{
			return true;
		}

		return pendingField.attributeId.empty()
			&& !pendingField.attributeCoreName.empty()
			&& generatedField.attributeCoreName == pendingField.attributeCoreName;
	}

	bool SavePlanHasGeneratedDependencyProducer(const RuleSet::DataConnectionSavePlan& plan,
	                                           const RuleSet::DataConnectionSaveField& pendingField)
	{
		for(const RuleSet::DataConnectionSaveTable& candidateTable : plan.tables)
		{
			for(const RuleSet::DataConnectionSaveRow& candidateRow : candidateTable.rows)
			{
				for(const RuleSet::DataConnectionSaveField& candidateField : candidateRow.fields)
				{
					if(GeneratedFieldMatchesPendingRelationship(candidateRow, candidateField, pendingField))
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	void AddPendingGeneratedDependencyDiagnostics(RuleSet::DataConnectionSavePlan& plan)
	{
		for(const RuleSet::DataConnectionSaveTable& table : plan.tables)
		{
			for(const RuleSet::DataConnectionSaveRow& row : table.rows)
			{
				for(const RuleSet::DataConnectionSaveField& field : row.fields)
				{
					if(!IsPendingGeneratedRelationshipField(field) ||
					   SavePlanHasGeneratedDependencyProducer(plan, field))
					{
						continue;
					}

					const string context = "dataConnectionSavePlan:"
						+ (table.sourceId.empty() ? "<missing-source>" : table.sourceId)
						+ ":"
						+ (table.sourceObjectName.empty() ? "<missing-object>" : table.sourceObjectName)
						+ ":childEntity:" + (row.entityId.empty() ? "<missing-entity>" : row.entityId)
						+ ":childInstance:" + (row.instanceId.empty() ? "<missing-instance>" : row.instanceId)
						+ ":field:" + (field.sourceFieldName.empty() ? "<missing-field>" : field.sourceFieldName)
						+ ":parentEntity:" + (field.entityId.empty() ? "<missing-parent-entity>" : field.entityId)
						+ ":parentInstance:" + (field.instanceId.empty() ? "<missing-parent-instance>" : field.instanceId)
						+ ":parentAttribute:" + (field.attributeId.empty() ? "<missing-parent-attribute>" : field.attributeId);

					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_pending_generated_dependency_unresolved",
					                      "Save plan relationship field is waiting for a generated parent key, but the same save plan does not contain a matching create/upsert operation that can return that key.",
					                      context);
				}
			}
		}
	}

	shared_ptr<Instance> FindRelatedInstanceForRelationship(const shared_ptr<Instance>& instance,
	                                                        const RuleSet::DataConnectionRelationshipFieldMapping& mapping)
	{
		if(instance == nullptr || mapping.relationshipId.empty())
		{
			return nullptr;
		}

		shared_ptr<Instance> matchedInstance;
		const vector<size_t> linkIndexes = instance->FindRelationshipLinkIndexesByRelationshipId(mapping.relationshipId);

		for(size_t linkIndex : linkIndexes)
		{
			if(linkIndex >= instance->relationshipLinks.size())
			{
				continue;
			}

			const InstanceRelationshipLink& link = instance->relationshipLinks[linkIndex];

			if(link.relationshipId != mapping.relationshipId)
			{
				continue;
			}

			if(!mapping.relatedEntityId.empty()
				&& link.sourceEntityId != mapping.relatedEntityId
				&& link.targetEntityId != mapping.relatedEntityId)
			{
				continue;
			}

			shared_ptr<Instance> linkedInstance = link.linkedInstance.lock();

			if(linkedInstance == nullptr)
			{
				continue;
			}

			if(!mapping.relatedEntityId.empty() && linkedInstance->entityId != mapping.relatedEntityId)
			{
				continue;
			}

			if(matchedInstance != nullptr && matchedInstance->instanceId != linkedInstance->instanceId)
			{
				return nullptr;
			}

			matchedInstance = linkedInstance;
		}

		return matchedInstance;
	}

	const RuleSet::DataConnectionRecordIdentitySnapshot* FindOriginalValueSnapshot(
		const RuleSet& ruleSet,
		const RuleSet::DataConnectionEntityMapping& mapping,
		const RuleSet::DataConnectionAttributeMapping& attributeMapping,
		const string& instanceId,
		const string& snapshotRole)
	{
		if(ruleSet.dataConnectionRecordIdentitySnapshots == nullptr)
		{
			return nullptr;
		}

		for(auto snapshot = ruleSet.dataConnectionRecordIdentitySnapshots->rbegin();
		    snapshot != ruleSet.dataConnectionRecordIdentitySnapshots->rend();
		    ++snapshot)
		{
			if(snapshot->sourceId == mapping.sourceId
				&& snapshot->sourceObjectName == mapping.sourceObjectName
				&& snapshot->snapshotRole == snapshotRole
				&& snapshot->entityId == mapping.entityId
				&& snapshot->instanceId == instanceId
				&& snapshot->attributeId == attributeMapping.attributeId
				&& snapshot->sourceFieldName == attributeMapping.sourceFieldName)
			{
				return &(*snapshot);
			}
		}

		return nullptr;
	}

	const RuleSet::DataConnectionRecordIdentitySnapshot* FindRecordIdentitySnapshot(
		const RuleSet& ruleSet,
		const RuleSet::DataConnectionEntityMapping& mapping,
		const RuleSet::DataConnectionAttributeMapping& attributeMapping,
		const string& instanceId)
	{
		return FindOriginalValueSnapshot(ruleSet, mapping, attributeMapping, instanceId, "RecordIdentity");
	}

	const RuleSet::DataConnectionRecordIdentitySnapshot* FindOptimisticConcurrencySnapshot(
		const RuleSet& ruleSet,
		const RuleSet::DataConnectionEntityMapping& mapping,
		const RuleSet::DataConnectionAttributeMapping& attributeMapping,
		const string& instanceId)
	{
		return FindOriginalValueSnapshot(ruleSet, mapping, attributeMapping, instanceId, "OptimisticConcurrencyCheck");
	}

	const RuleSet::DataConnectionEntityMapping* FindDataConnectionEntityMappingForSnapshot(
		const RuleSet& ruleSet,
		const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot)
	{
		if(ruleSet.dataConnectionEntityMappings == nullptr)
		{
			return nullptr;
		}

		for(const RuleSet::DataConnectionEntityMapping& mapping : *ruleSet.dataConnectionEntityMappings)
		{
			if(mapping.sourceId == snapshot.sourceId
				&& mapping.sourceObjectName == snapshot.sourceObjectName
				&& mapping.entityId == snapshot.entityId)
			{
				return &mapping;
			}
		}

		return nullptr;
	}

	bool RuntimeInstanceExists(const RuleSet& ruleSet, const string& entityId, const string& instanceId)
	{
		if(entityId.empty() || instanceId.empty())
		{
			return false;
		}

		shared_ptr<Entity> entity = FindEntityById(ruleSet.entities, entityId);

		return entity != nullptr && entity->GetInstanceById(instanceId) != nullptr;
	}

	bool IsUsableDeletedRecordIdentitySnapshot(const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot)
	{
		return snapshot.snapshotRole == "RecordIdentity"
			&& !snapshot.sourceId.empty()
			&& !snapshot.sourceObjectName.empty()
			&& !snapshot.entityId.empty()
			&& !snapshot.instanceId.empty()
			&& !snapshot.attributeId.empty()
			&& !snapshot.sourceFieldName.empty()
			&& snapshot.isKnown
			&& snapshot.isCertain
			&& !snapshot.value.is_null();
	}

	string DeletedInstanceDeletePlanGroupKey(const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot)
	{
		return snapshot.sourceId + "|" + snapshot.sourceObjectName + "|" + snapshot.entityId + "|" + snapshot.instanceId;
	}

	RuleSet::DataConnectionLoadFilter DeletedInstanceDeleteFilterFromSnapshot(
		const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot,
		const RuleSet::DataConnectionEntityMapping& mapping,
		const shared_ptr<Entity>& entity)
	{
		RuleSet::DataConnectionLoadFilter filter;

		filter.sourceFieldName = snapshot.sourceFieldName;
		filter.operatorName = snapshot.operatorName.empty() ? "Equals" : snapshot.operatorName;
		filter.valueSource = "DeletedLoadedRecordIdentity";
		filter.entityId = snapshot.entityId;
		filter.entityName = snapshot.entityName.empty()
			? (mapping.entityName.empty() ? (entity != nullptr ? entity->description : string()) : mapping.entityName)
			: snapshot.entityName;
		filter.instanceId = snapshot.instanceId;
		filter.attributeId = snapshot.attributeId;
		filter.attributeName = snapshot.attributeName;
		filter.attributeCoreName = snapshot.attributeCoreName;
		filter.attributeDataType = snapshot.attributeDataType;
		filter.valueType = snapshot.valueType;
		filter.value = snapshot.value;
		filter.isKnown = snapshot.isKnown;
		filter.isCertain = snapshot.isCertain;
		filter.isUncertain = snapshot.isUncertain;
		filter.knowledgeState = snapshot.knowledgeState.empty() ? "Known" : snapshot.knowledgeState;

		if((filter.attributeName.empty() || filter.attributeCoreName.empty() || filter.valueType.empty()) && entity != nullptr)
		{
			shared_ptr<Attribute> attribute = FindEntityAttributeById(entity, snapshot.attributeId);

			if(attribute != nullptr)
			{
				if(filter.attributeName.empty())
				{
					filter.attributeName = attribute->GetAttributeText();
				}

				if(filter.attributeCoreName.empty())
				{
					filter.attributeCoreName = attribute->GetCoreName();
				}

				if(filter.valueType.empty())
				{
					filter.valueType = AttributeTypeName(attribute->GetType());
				}
			}
		}

		return filter;
	}
}

RuleSet::RuleSet()
{
	ResetSessionStartDateTime();
}

void to_json(json& j, const RuleSet::UploadInfo& info)
{
	j = json{
		{"fileId", info.fileId},
		{"uploadControlId", info.uploadControlId},
		{"fileName", info.fileName},
		{"fileExtension", info.fileExtension},
		{"fileSize", info.fileSize},
		{"fileSizeBytes", info.fileSizeBytes},
		{"instanceId", info.instanceId},
		{"parentInstanceId", info.parentInstanceId},
		{"entityId", info.entityId},
		{"relationshipId", info.relationshipId},
		{"externallyUploaded", info.externallyUploaded}
	};
}

void from_json(const json& j, RuleSet::UploadInfo& info)
{
	j.at("fileId").get_to(info.fileId);
	j.at("uploadControlId").get_to(info.uploadControlId);
	j.at("fileName").get_to(info.fileName);
	j.at("fileExtension").get_to(info.fileExtension);
	j.at("fileSize").get_to(info.fileSize);
	j.at("instanceId").get_to(info.instanceId);
	info.fileSizeBytes = j.value("fileSizeBytes", static_cast<unsigned long long>(0));
	info.parentInstanceId = j.value("parentInstanceId", "");
	info.entityId = j.value("entityId", "");
	info.relationshipId = j.value("relationshipId", "");
	info.externallyUploaded = j.value("externallyUploaded", false);
}

void to_json(json& j, const RuleSet::SessionModelRuleReference& reference)
{
	j = json{
		{"source", reference.source},
		{"entityId", reference.entityId},
		{"attributeId", reference.attributeId},
		{"relationshipId", reference.relationshipId},
		{"nodeType", reference.nodeType},
		{"operator", reference.operatorName},
		{"functionName", reference.functionName}
	};
}

void RuleSet::SessionModelRuleMetadata::RebuildDecisionTableMetadata()
{
	decisionTableMetadataBuilt = false;
	decisionTableRowMetadata.clear();

	if(!executableDecisionTable.is_object()
		|| !executableDecisionTable.contains("rows")
		|| !executableDecisionTable.at("rows").is_array())
	{
		return;
	}

	const json& rows = executableDecisionTable.at("rows");
	decisionTableRowMetadata.reserve(rows.size());

	for(size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
	{
		const json& row = rows.at(rowIndex);
		DecisionTableRowMetadata rowMetadata;

		rowMetadata.rowIndex = rowIndex;
		rowMetadata.isObject = row.is_object();

		if(rowMetadata.isObject)
		{
			if(row.contains("conditionCells") && row.at("conditionCells").is_array())
			{
				const json& conditionCells = row.at("conditionCells");

				rowMetadata.conditionCellsArray = true;
				rowMetadata.conditionCellCount = conditionCells.size();
				rowMetadata.conditionCellsEmpty = conditionCells.empty();
			}

			if(row.contains("conclusionCells") && row.at("conclusionCells").is_array())
			{
				const json& conclusionCells = row.at("conclusionCells");

				rowMetadata.conclusionCellsArray = true;
				rowMetadata.conclusionCellCount = conclusionCells.size();
				rowMetadata.conclusionCells.reserve(conclusionCells.size());

				for(size_t cellIndex = 0; cellIndex < conclusionCells.size(); ++cellIndex)
				{
					const json& cell = conclusionCells.at(cellIndex);
					DecisionTableConclusionCellMetadata cellMetadata;

					cellMetadata.cellIndex = cellIndex;
					cellMetadata.isObject = cell.is_object();

					if(cellMetadata.isObject)
					{
						cellMetadata.attributeId = SessionModelExpressionParserSupport::GetString(cell, "attributeId");
						cellMetadata.relationshipId = SessionModelExpressionParserSupport::GetString(cell, "relationshipId");
						cellMetadata.valueMode = SessionModelExpressionParserSupport::GetString(cell, "valueMode");
						cellMetadata.value = SessionModelExpressionParserSupport::GetString(cell, "value");
					}

					rowMetadata.conclusionCells.push_back(cellMetadata);
				}
			}
		}

		decisionTableRowMetadata.push_back(rowMetadata);
	}

	decisionTableMetadataBuilt = true;
}

void to_json(json& j, const RuleSet::SessionModelRuleMetadata& rule)
{
	j = json{
		{"ruleId", rule.ruleId},
		{"ruleName", rule.ruleName},
		{"isActive", rule.isActive},
		{"conclusionMode", rule.conclusionMode},
		{"conclusionEntityId", rule.conclusionEntityId},
		{"conclusionAttributeId", rule.conclusionAttributeId},
		{"conclusionValue", rule.conclusionValue},
		{"conclusionValueMode", rule.conclusionValueMode},
		{"elseValue", rule.elseValue},
		{"relationshipId", rule.relationshipId},
		{"hasExecutableDecisionTable", rule.hasExecutableDecisionTable},
		{"decisionTableRows", rule.decisionTableRows},
		{"hasDecisionTablePayload", rule.executableDecisionTable.is_object()},
		{"hasDecisionTableMetadata", rule.decisionTableMetadataBuilt},
		{"decisionTableMetadataRows", rule.decisionTableRowMetadata.size()},
		{"referenceCount", rule.references.size()},
		{"references", rule.references}
	};
}

void to_json(json& j, const RuleSet::ValidationEvent& event)
{
	j = json{
		{"eventId", event.eventId},
		{"severity", event.severity},
		{"ruleId", event.ruleId},
		{"ruleName", event.ruleName},
		{"entityId", event.entityId},
		{"instanceId", event.instanceId},
		{"message", event.message},
		{"messageKnown", event.messageKnown},
		{"messageKnowledgeState", event.messageKnowledgeState}
	};
}

void to_json(json& j, const RuleSet::AttributeHistorySegment& segment)
{
	j = json{
		{"entityId", segment.entityId},
		{"instanceId", segment.instanceId},
		{"attributeId", segment.attributeId},
		{"effectiveStart", segment.effectiveStart},
		{"effectiveEnd", segment.effectiveEnd.empty() ? json(nullptr) : json(segment.effectiveEnd)},
		{"value", segment.value},
		{"valueType", static_cast<int>(segment.valueType)},
		{"knownness", Attribute::KnowledgeStateToString(segment.knowledgeState)},
		{"source", segment.source},
		{"transactionStart", segment.transactionStart},
		{"transactionEnd", segment.transactionEnd},
		{"assertedAt", segment.assertedAt},
		{"evaluationTime", segment.evaluationTime},
		{"modelTimeZone", segment.modelTimeZone},
		{"provenance", segment.provenance}
	};
}

void to_json(json& j, const RuleSet::EntityExistenceHistorySegment& segment)
{
	j = json{
		{"entityId", segment.entityId},
		{"instanceId", segment.instanceId},
		{"effectiveStart", segment.effectiveStart},
		{"effectiveEnd", segment.effectiveEnd.empty() ? json(nullptr) : json(segment.effectiveEnd)},
		{"knownness", Attribute::KnowledgeStateToString(segment.knowledgeState)},
		{"source", segment.source},
		{"transactionStart", segment.transactionStart},
		{"transactionEnd", segment.transactionEnd},
		{"assertedAt", segment.assertedAt},
		{"evaluationTime", segment.evaluationTime},
		{"modelTimeZone", segment.modelTimeZone},
		{"provenance", segment.provenance}
	};
}

void to_json(json& j, const RuleSet::RelationshipLinkHistorySegment& segment)
{
	j = json{
		{"relationshipId", segment.relationshipId},
		{"sourceEntityId", segment.sourceEntityId},
		{"sourceInstanceId", segment.sourceInstanceId},
		{"targetEntityId", segment.targetEntityId},
		{"targetInstanceId", segment.targetInstanceId},
		{"effectiveStart", segment.effectiveStart},
		{"effectiveEnd", segment.effectiveEnd.empty() ? json(nullptr) : json(segment.effectiveEnd)},
		{"knownness", Attribute::KnowledgeStateToString(segment.knowledgeState)},
		{"source", segment.source},
		{"transactionStart", segment.transactionStart},
		{"transactionEnd", segment.transactionEnd},
		{"assertedAt", segment.assertedAt},
		{"evaluationTime", segment.evaluationTime},
		{"modelTimeZone", segment.modelTimeZone},
		{"provenance", segment.provenance}
	};
}

void to_json(json& j, const RuleSet::RelationshipCoverageHistorySegment& segment)
{
	j = json{
		{"relationshipId", segment.relationshipId},
		{"sourceInstanceId", segment.sourceInstanceId},
		{"effectiveStart", segment.effectiveStart},
		{"effectiveEnd", segment.effectiveEnd.empty() ? json(nullptr) : json(segment.effectiveEnd)},
		{"knownness", Attribute::KnowledgeStateToString(segment.knowledgeState)},
		{"source", segment.source},
		{"transactionStart", segment.transactionStart},
		{"transactionEnd", segment.transactionEnd},
		{"assertedAt", segment.assertedAt},
		{"evaluationTime", segment.evaluationTime},
		{"modelTimeZone", segment.modelTimeZone},
		{"provenance", segment.provenance}
	};
}

void to_json(json& j, const RuleSet::DataConnectionSaveDiagnostic& diagnostic)
{
	j = json{
		{"severity", diagnostic.severity},
		{"code", diagnostic.code},
		{"message", diagnostic.message},
		{"context", diagnostic.context}
	};
}

void to_json(json& j, const RuleSet::DataConnectionSaveField& field)
{
	j = json{
		{"attributeId", field.attributeId},
		{"attributeName", field.attributeName},
		{"attributeCoreName", field.attributeCoreName},
		{"attributeDataType", field.attributeDataType},
		{"sourceFieldName", field.sourceFieldName},
		{"valueSource", field.valueSource},
		{"entityId", field.entityId},
		{"entityName", field.entityName},
		{"instanceId", field.instanceId},
		{"relationshipId", field.relationshipId},
		{"relationshipName", field.relationshipName},
		{"valueType", field.valueType},
		{"value", field.value},
		{"isKnown", field.isKnown},
		{"isCertain", field.isCertain},
		{"isUncertain", field.isUncertain},
		{"knowledgeState", field.knowledgeState}
	};

	if(!field.valueGeneration.empty())
	{
		j["valueGeneration"] = field.valueGeneration;
	}

	if(field.generatedOnCreate)
	{
		j["generatedOnCreate"] = true;
	}
}

void to_json(json& j, const RuleSet::DataConnectionSaveFilter& filter)
{
	j = json{
		{"sourceFieldName", filter.sourceFieldName},
		{"operator", filter.operatorName},
		{"filterRole", filter.filterRole},
		{"valueSource", filter.valueSource},
		{"entityId", filter.entityId},
		{"entityName", filter.entityName},
		{"instanceId", filter.instanceId},
		{"attributeId", filter.attributeId},
		{"attributeName", filter.attributeName},
		{"attributeCoreName", filter.attributeCoreName},
		{"attributeDataType", filter.attributeDataType},
		{"valueType", filter.valueType},
		{"value", filter.value},
		{"isKnown", filter.isKnown},
		{"isCertain", filter.isCertain},
		{"isUncertain", filter.isUncertain},
		{"knowledgeState", filter.knowledgeState}
	};
}

void to_json(json& j, const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot)
{
	j = json{
		{"sourceId", snapshot.sourceId},
		{"sourceName", snapshot.sourceName},
		{"serverConnectionKey", snapshot.serverConnectionKey},
		{"sourceObjectName", snapshot.sourceObjectName},
		{"snapshotRole", snapshot.snapshotRole},
		{"entityId", snapshot.entityId},
		{"entityName", snapshot.entityName},
		{"instanceId", snapshot.instanceId},
		{"attributeId", snapshot.attributeId},
		{"attributeName", snapshot.attributeName},
		{"attributeCoreName", snapshot.attributeCoreName},
		{"attributeDataType", snapshot.attributeDataType},
		{"sourceFieldName", snapshot.sourceFieldName},
		{"operator", snapshot.operatorName},
		{"valueType", snapshot.valueType},
		{"value", snapshot.value},
		{"isKnown", snapshot.isKnown},
		{"isCertain", snapshot.isCertain},
		{"isUncertain", snapshot.isUncertain},
		{"knowledgeState", snapshot.knowledgeState}
	};
}

void to_json(json& j, const RuleSet::DataConnectionSaveRow& row)
{
	j = json{
		{"entityId", row.entityId},
		{"entityName", row.entityName},
		{"instanceId", row.instanceId},
		{"saveOperationPolicy", row.saveOperationPolicy},
		{"createIfMissing", row.createIfMissing},
		{"fields", row.fields},
		{"filters", row.filters}
	};
}

void to_json(json& j, const RuleSet::DataConnectionSaveTable& table)
{
	j = json{
		{"sourceId", table.sourceId},
		{"sourceName", table.sourceName},
		{"sourceType", table.sourceType},
		{"databaseProvider", table.databaseProvider},
		{"serverConnectionKey", table.serverConnectionKey},
		{"sourceObjectName", table.sourceObjectName},
		{"rows", table.rows}
	};
}

void to_json(json& j, const RuleSet::DataConnectionLoadDiagnostic& diagnostic)
{
	j = json{
		{"severity", diagnostic.severity},
		{"code", diagnostic.code},
		{"message", diagnostic.message},
		{"context", diagnostic.context}
	};
}

void to_json(json& j, const RuleSet::DataConnectionLoadTarget& target)
{
	j = json{
		{"entityId", target.entityId},
		{"entityName", target.entityName},
		{"attributeId", target.attributeId},
		{"attributeName", target.attributeName},
		{"attributeCoreName", target.attributeCoreName},
		{"attributeDataType", target.attributeDataType},
		{"valueType", target.valueType}
	};
}

void to_json(json& j, const RuleSet::DataConnectionLoadField& field)
{
	j = json{
		{"sourceFieldName", field.sourceFieldName},
		{"targets", field.targets}
	};
}

void to_json(json& j, const RuleSet::DataConnectionLoadFilter& filter)
{
	j = json{
		{"sourceFieldName", filter.sourceFieldName},
		{"operator", filter.operatorName},
		{"valueSource", filter.valueSource},
		{"entityId", filter.entityId},
		{"entityName", filter.entityName},
		{"instanceId", filter.instanceId},
		{"attributeId", filter.attributeId},
		{"attributeName", filter.attributeName},
		{"attributeCoreName", filter.attributeCoreName},
		{"attributeDataType", filter.attributeDataType},
		{"valueType", filter.valueType},
		{"value", filter.value},
		{"isKnown", filter.isKnown},
		{"isCertain", filter.isCertain},
		{"isUncertain", filter.isUncertain},
		{"knowledgeState", filter.knowledgeState}
	};
}

void to_json(json& j, const RuleSet::DataConnectionLoadTable& table)
{
	j = json{
		{"sourceId", table.sourceId},
		{"sourceName", table.sourceName},
		{"sourceType", table.sourceType},
		{"databaseProvider", table.databaseProvider},
		{"serverConnectionKey", table.serverConnectionKey},
		{"sourceObjectName", table.sourceObjectName},
		{"fields", table.fields},
		{"filters", table.filters}
	};
}

json RuleSet::DataConnectionSavePlan::ToJSON() const
{
	size_t filterCount = 0;

	for(const DataConnectionSaveTable& table : tables)
	{
		for(const DataConnectionSaveRow& row : table.rows)
		{
			filterCount += row.filters.size();
		}
	}

	return json{
		{"hasErrors", HasErrors()},
		{"tableCount", tables.size()},
		{"filterCount", filterCount},
		{"diagnosticCount", diagnostics.size()},
		{"tables", tables},
		{"diagnostics", diagnostics}
	};
}

json RuleSet::DataConnectionLoadPlan::ToJSON() const
{
	size_t fieldCount = 0;
	size_t targetCount = 0;
	size_t filterCount = 0;

	for(const DataConnectionLoadTable& table : tables)
	{
		fieldCount += table.fields.size();
		filterCount += table.filters.size();

		for(const DataConnectionLoadField& field : table.fields)
		{
			targetCount += field.targets.size();
		}
	}

	return json{
		{"hasErrors", HasErrors()},
		{"tableCount", tables.size()},
		{"fieldCount", fieldCount},
		{"targetCount", targetCount},
		{"filterCount", filterCount},
		{"diagnosticCount", diagnostics.size()},
		{"tables", tables},
		{"diagnostics", diagnostics}
	};
}

RuleSet::DataConnectionSavePlan RuleSet::BuildDataConnectionSavePlan() const
{
	DataConnectionSavePlan plan;

	if(dataConnectionEntityMappings == nullptr || dataConnectionEntityMappings->empty())
	{
		return plan;
	}

	const vector<DataConnectionSource> emptySources;
	const vector<DataConnectionSource>& sources = dataConnectionSources != nullptr ? *dataConnectionSources : emptySources;

	for(const DataConnectionEntityMapping& mapping : *dataConnectionEntityMappings)
	{
		const string mappingContext = "dataConnectionSavePlan:"
			+ (mapping.sourceId.empty() ? "<missing-source>" : mapping.sourceId)
			+ ":"
			+ (mapping.sourceObjectName.empty() ? "<missing-object>" : mapping.sourceObjectName)
			+ ":"
			+ (mapping.entityId.empty() ? "<missing-entity>" : mapping.entityId);

		const DataConnectionSource* source = FindDataConnectionSource(sources, mapping.sourceId);

		if(source == nullptr)
		{
			AddSavePlanDiagnostic(plan,
			                      "error",
			                      "data_connection_save_source_missing",
			                      "Save plan could not resolve the mapped data connection source.",
			                      mappingContext);
		}

		if(mapping.sourceObjectName.empty())
		{
			AddSavePlanDiagnostic(plan,
			                      "error",
			                      "data_connection_save_source_object_missing",
			                      "Save plan entity mapping is missing sourceObjectName.",
			                      mappingContext);
		}

		shared_ptr<Entity> entity = FindEntityById(entities, mapping.entityId);

		if(entity == nullptr)
		{
			AddSavePlanDiagnostic(plan,
			                      "error",
			                      "data_connection_save_entity_missing",
			                      "Save plan could not resolve the mapped runtime entity.",
			                      mappingContext);

			continue;
		}

		size_t outputMappingCount = 0;

		for(const DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
		{
			if(attributeMapping.output)
			{
				outputMappingCount++;
			}
		}

		if(outputMappingCount == 0)
		{
			AddSavePlanDiagnostic(plan,
			                      "warning",
			                      "data_connection_save_output_mappings_missing",
			                      "Save plan entity mapping has no output-enabled attribute mappings.",
			                      mappingContext);

			continue;
		}

		DataConnectionSaveTable& table = FindOrCreateSaveTable(plan, mapping, source);

		if(entity->instances.empty())
		{
			AddSavePlanDiagnostic(plan,
			                      "warning",
			                      "data_connection_save_entity_has_no_instances",
			                      "Save plan entity mapping has no current runtime instances to save.",
			                      mappingContext);
		}

		for(const shared_ptr<Instance>& instance : entity->instances)
		{
			if(instance == nullptr)
			{
				AddSavePlanDiagnostic(plan,
				                      "warning",
				                      "data_connection_save_instance_invalid",
				                      "Save plan skipped a null runtime instance.",
				                      mappingContext);

				continue;
			}

			DataConnectionSaveRow row;

			row.entityId = mapping.entityId;
			row.entityName = mapping.entityName.empty() ? entity->description : mapping.entityName;
			row.instanceId = instance->instanceId;
			row.saveOperationPolicy = NormalizeSaveOperationPolicy(mapping.saveOperationPolicy);
			row.createIfMissing = SaveOperationPolicyAllowsInsertFallback(row.saveOperationPolicy);

			std::unordered_set<string> sourceFieldNames;

			for(const DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
			{
				if(!attributeMapping.output)
				{
					continue;
				}

				const string fieldContext = mappingContext
					+ ":instance:" + instance->instanceId
					+ ":attribute:" + (attributeMapping.attributeId.empty() ? "<missing-attribute>" : attributeMapping.attributeId)
					+ ":field:" + (attributeMapping.sourceFieldName.empty() ? "<missing-field>" : attributeMapping.sourceFieldName);

				if(attributeMapping.sourceFieldName.empty())
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_field_missing",
					                      "Save plan output mapping is missing sourceFieldName.",
					                      fieldContext);

					continue;
				}

				if(!sourceFieldNames.insert(attributeMapping.sourceFieldName).second)
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_field_duplicate",
					                      "Save plan has duplicate output mappings for the same source field in one row.",
					                      fieldContext);
				}

				shared_ptr<Attribute> attribute = instance->GetAttributeByID(attributeMapping.attributeId);

				if(attribute == nullptr)
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_attribute_missing",
					                      "Save plan could not resolve the mapped runtime attribute on the instance.",
					                      fieldContext);

					continue;
				}

				const string mappingValueSource = AttributeMappingUsesLoadFilter(attributeMapping)
					? NormalizeDataConnectionLoadFilterValueSource(attributeMapping.loadFilterValueSource)
					: "CurrentRuntimeValue";
				const bool mappingUsesSessionUserId = mappingValueSource == "SessionUserId";
				const bool useSessionUserIdValue = mappingUsesSessionUserId && !attribute->IsKnown();
				const string attributeValueType = AttributeTypeName(attribute->GetType());

				if(useSessionUserIdValue)
				{
					if(sessionContext.userId.empty())
					{
						AddSavePlanDiagnostic(plan,
						                      "warning",
						                      "data_connection_save_session_user_id_missing",
						                      "Save plan skipped a signed-in user output value because the session user id is not available.",
						                      fieldContext);

						continue;
					}

					if(!DataConnectionMappingCanUseSessionUserId(attributeValueType))
					{
						AddSavePlanDiagnostic(plan,
						                      "error",
						                      "data_connection_save_session_user_field_type_mismatch",
						                      "Save plan signed-in user output mappings must target Text attributes because generated signed-in user ids are text.",
						                      fieldContext);

						continue;
					}
				}

				DataConnectionSaveField field;

				field.attributeId = attributeMapping.attributeId;
				field.attributeName = attributeMapping.attributeName.empty() ? attribute->GetAttributeText() : attributeMapping.attributeName;
				field.attributeCoreName = attributeMapping.attributeCoreName.empty() ? attribute->GetCoreName() : attributeMapping.attributeCoreName;
				field.attributeDataType = attributeMapping.attributeDataType;
				field.sourceFieldName = attributeMapping.sourceFieldName;
				field.valueSource = useSessionUserIdValue ? "SessionUserId" : "CurrentRuntimeValue";
				field.entityId = mapping.entityId;
				field.entityName = mapping.entityName.empty() ? entity->description : mapping.entityName;
				field.instanceId = instance->instanceId;
				field.valueType = useSessionUserIdValue ? "Text" : attributeValueType;
				field.value = useSessionUserIdValue ? json(sessionContext.userId) : AttributeValueToJson(attribute);
				field.isKnown = useSessionUserIdValue ? true : attribute->IsKnown();
				field.isCertain = useSessionUserIdValue ? true : attribute->IsCertain();
				field.isUncertain = useSessionUserIdValue ? false : attribute->IsUncertain();
				field.knowledgeState = useSessionUserIdValue ? "Certain" : Attribute::KnowledgeStateToString(attribute->GetKnowledgeState());
				field.valueGeneration = attributeMapping.valueGeneration;
				field.generatedOnCreate = attributeMapping.generatedOnCreate;

				if(!useSessionUserIdValue && !field.generatedOnCreate && !field.isKnown)
				{
					AddSavePlanDiagnostic(plan,
					                      "warning",
					                      "data_connection_save_value_unknown",
					                      "Save plan output value is unknown and will be represented as null.",
					                      fieldContext);
				}
				else if(!useSessionUserIdValue && !field.generatedOnCreate && field.isUncertain)
				{
					AddSavePlanDiagnostic(plan,
					                      "warning",
					                      "data_connection_save_value_uncertain",
					                      "Save plan output value is uncertain.",
					                      fieldContext);
				}

				row.fields.push_back(field);
			}

			for(const DataConnectionRelationshipFieldMapping& relationshipFieldMapping : mapping.relationshipFieldMappings)
			{
				if(!relationshipFieldMapping.output)
				{
					continue;
				}

				const string fieldContext = mappingContext
					+ ":instance:" + instance->instanceId
					+ ":relationship:" + (relationshipFieldMapping.relationshipId.empty() ? "<missing-relationship>" : relationshipFieldMapping.relationshipId)
					+ ":attribute:" + (relationshipFieldMapping.relatedAttributeId.empty() ? "<missing-related-attribute>" : relationshipFieldMapping.relatedAttributeId)
					+ ":field:" + (relationshipFieldMapping.sourceFieldName.empty() ? "<missing-field>" : relationshipFieldMapping.sourceFieldName);

				if(relationshipFieldMapping.sourceFieldName.empty())
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_relationship_field_missing",
					                      "Save plan relationship field mapping is missing sourceFieldName.",
					                      fieldContext);

					continue;
				}

				if(SaveRowHasFieldForField(row, relationshipFieldMapping.sourceFieldName))
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_relationship_field_duplicate",
					                      "Save plan relationship field mapping duplicates another output field in one row.",
					                      fieldContext);

					continue;
				}

				if(relationshipFieldMapping.relationshipId.empty())
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_relationship_missing_id",
					                      "Save plan relationship field mapping is missing relationshipId.",
					                      fieldContext);

					continue;
				}

				if(relationshipFieldMapping.relatedAttributeId.empty())
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_relationship_attribute_missing_id",
					                      "Save plan relationship field mapping is missing relatedAttributeId.",
					                      fieldContext);

					continue;
				}

				shared_ptr<Instance> relatedInstance = FindRelatedInstanceForRelationship(instance, relationshipFieldMapping);

				if(relatedInstance == nullptr)
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_relationship_instance_missing",
					                      "Save plan could not resolve exactly one related runtime instance for a relationship field mapping.",
					                      fieldContext);

					continue;
				}

				shared_ptr<Attribute> relatedAttribute = relatedInstance->GetAttributeByID(relationshipFieldMapping.relatedAttributeId);

				if(relatedAttribute == nullptr)
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_relationship_attribute_missing",
					                      "Save plan could not resolve the related runtime attribute for a relationship field mapping.",
					                      fieldContext + ":relatedInstance:" + relatedInstance->instanceId);

					continue;
				}

				const DataConnectionEntityMapping* relatedEntityMapping =
					FindDataConnectionEntityMapping(*dataConnectionEntityMappings, relatedInstance->entityId);
				const DataConnectionAttributeMapping* relatedAttributeMapping =
					relatedEntityMapping != nullptr
						? FindDataConnectionAttributeMapping(*relatedEntityMapping, relationshipFieldMapping.relatedAttributeId)
						: nullptr;
				const bool pendingGeneratedRelationshipValue =
					!relatedAttribute->IsKnown()
					&& relatedEntityMapping != nullptr
					&& relatedAttributeMapping != nullptr
					&& relatedAttributeMapping->output
					&& relatedAttributeMapping->generatedOnCreate
					&& SaveOperationPolicyAllowsInsertFallback(relatedEntityMapping->saveOperationPolicy);

				DataConnectionSaveField field;

				field.attributeId = relationshipFieldMapping.relatedAttributeId;
				field.attributeName = relationshipFieldMapping.relatedAttributeName.empty() ? relatedAttribute->GetAttributeText() : relationshipFieldMapping.relatedAttributeName;
				field.attributeCoreName = relationshipFieldMapping.relatedAttributeCoreName.empty() ? relatedAttribute->GetCoreName() : relationshipFieldMapping.relatedAttributeCoreName;
				field.attributeDataType = relationshipFieldMapping.relatedAttributeDataType;
				field.sourceFieldName = relationshipFieldMapping.sourceFieldName;
				field.valueSource = pendingGeneratedRelationshipValue
					? "PendingGeneratedRelationshipValue"
					: "RelatedInstanceValue";
				field.entityId = relatedInstance->entityId;
				field.entityName = relationshipFieldMapping.relatedEntityName;
				field.instanceId = relatedInstance->instanceId;
				field.relationshipId = relationshipFieldMapping.relationshipId;
				field.relationshipName = relationshipFieldMapping.relationshipName;
				field.valueType = AttributeTypeName(relatedAttribute->GetType());
				field.value = AttributeValueToJson(relatedAttribute);
				field.isKnown = relatedAttribute->IsKnown();
				field.isCertain = relatedAttribute->IsCertain();
				field.isUncertain = relatedAttribute->IsUncertain();
				field.knowledgeState = Attribute::KnowledgeStateToString(relatedAttribute->GetKnowledgeState());

				if(field.entityName.empty())
				{
					shared_ptr<Entity> relatedEntity = FindEntityById(entities, relatedInstance->entityId);
					field.entityName = relatedEntity != nullptr ? relatedEntity->description : "";
				}

				if(pendingGeneratedRelationshipValue)
				{
					AddSavePlanDiagnostic(plan,
					                      "information",
					                      "data_connection_save_relationship_value_pending_generated",
					                      "Save plan relationship field value is pending a generated key from the related parent save operation.",
					                      fieldContext + ":relatedInstance:" + relatedInstance->instanceId);
				}
				else if(!field.isKnown)
				{
					AddSavePlanDiagnostic(plan,
					                      "warning",
					                      "data_connection_save_relationship_value_unknown",
					                      "Save plan relationship field value is unknown and will be represented as null.",
					                      fieldContext + ":relatedInstance:" + relatedInstance->instanceId);
				}
				else if(field.isUncertain)
				{
					AddSavePlanDiagnostic(plan,
					                      "warning",
					                      "data_connection_save_relationship_value_uncertain",
					                      "Save plan relationship field value is uncertain.",
					                      fieldContext + ":relatedInstance:" + relatedInstance->instanceId);
				}

				row.fields.push_back(field);
			}

			for(const DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
			{
				if(!AttributeMappingUsesLoadFilter(attributeMapping))
				{
					continue;
				}

				const string fieldContext = mappingContext
					+ ":instance:" + instance->instanceId
					+ ":attribute:" + (attributeMapping.attributeId.empty() ? "<missing-attribute>" : attributeMapping.attributeId)
					+ ":field:" + (attributeMapping.sourceFieldName.empty() ? "<missing-field>" : attributeMapping.sourceFieldName);

				if(attributeMapping.sourceFieldName.empty())
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_filter_field_missing",
					                      "Save plan loadFilter mapping is missing sourceFieldName.",
					                      fieldContext);

					continue;
				}

				if(SaveRowHasFilterForField(row, attributeMapping.sourceFieldName))
				{
					continue;
				}

				shared_ptr<Attribute> attribute = instance->GetAttributeByID(attributeMapping.attributeId);

				if(attribute == nullptr)
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_filter_attribute_missing",
					                      "Save plan could not resolve the runtime attribute value for a record-selection filter.",
					                      fieldContext);

					continue;
				}

				const DataConnectionRecordIdentitySnapshot* snapshot =
					FindRecordIdentitySnapshot(*this, mapping, attributeMapping, instance->instanceId);

				const string valueSource = NormalizeDataConnectionLoadFilterValueSource(attributeMapping.loadFilterValueSource);
				const string attributeValueType = AttributeTypeName(attribute->GetType());

				if((snapshot == nullptr || !snapshot->isKnown || !snapshot->isCertain) && valueSource == "SessionUserId")
				{
					if(sessionContext.userId.empty())
					{
						AddSavePlanDiagnostic(plan,
						                      "warning",
						                      "data_connection_save_filter_user_id_missing",
						                      "Save plan skipped a signed-in user record-selection filter because the session user id is not available.",
						                      fieldContext);

						continue;
					}

					if(!DataConnectionMappingCanUseSessionUserId(attributeValueType))
					{
						AddSavePlanDiagnostic(plan,
						                      "error",
						                      "data_connection_save_filter_session_user_type_mismatch",
						                      "Save plan signed-in user record-selection filters must target Text attributes because generated signed-in user ids are text.",
						                      fieldContext);

						continue;
					}

					if(SaveRowCanInsertWithGeneratedIdentity(row))
					{
						AddSavePlanDiagnostic(plan,
						                      "information",
						                      "data_connection_save_filter_session_user_insert_scope",
						                      "Save plan kept the signed-in user value as saved row data and skipped it as a record-selection filter because this unsaved generated-key row should be inserted as a new case.",
						                      fieldContext);

						continue;
					}

					DataConnectionSaveFilter filter;

					filter.sourceFieldName = attributeMapping.sourceFieldName;
					filter.operatorName = attributeMapping.loadFilterOperator.empty() ? "Equals" : attributeMapping.loadFilterOperator;
					filter.valueSource = "SessionUserId";
					filter.entityId = mapping.entityId;
					filter.entityName = mapping.entityName.empty() ? entity->description : mapping.entityName;
					filter.instanceId = instance->instanceId;
					filter.attributeId = attributeMapping.attributeId;
					filter.attributeName = attributeMapping.attributeName.empty() ? attribute->GetAttributeText() : attributeMapping.attributeName;
					filter.attributeCoreName = attributeMapping.attributeCoreName.empty() ? attribute->GetCoreName() : attributeMapping.attributeCoreName;
					filter.attributeDataType = attributeMapping.attributeDataType;
					filter.valueType = "Text";
					filter.value = sessionContext.userId;
					filter.isKnown = true;
					filter.isCertain = true;
					filter.isUncertain = false;
					filter.knowledgeState = "Certain";

					row.filters.push_back(filter);

					continue;
				}

				if(!attribute->IsKnown())
				{
					if(attributeMapping.generatedOnCreate && row.createIfMissing)
					{
						continue;
					}

					if(snapshot == nullptr || !snapshot->isKnown || !snapshot->isCertain)
					{
						AddSavePlanDiagnostic(plan,
						                      "warning",
						                      "data_connection_save_filter_value_unknown",
						                      "Save plan skipped a record-selection filter because neither an original loaded record identity nor the mapped runtime attribute value is known.",
						                      fieldContext);

						continue;
					}
				}

				if(!attribute->IsCertain())
				{
					if(attributeMapping.generatedOnCreate && row.createIfMissing)
					{
						continue;
					}

					if(snapshot == nullptr || !snapshot->isKnown || !snapshot->isCertain)
					{
						AddSavePlanDiagnostic(plan,
						                      "warning",
						                      "data_connection_save_filter_value_uncertain",
						                      "Save plan skipped a record-selection filter because neither an original loaded record identity nor the mapped runtime attribute value is certain.",
						                      fieldContext);

						continue;
					}
				}

				DataConnectionSaveFilter filter;

				if(snapshot != nullptr && snapshot->isKnown && snapshot->isCertain)
				{
					filter.sourceFieldName = snapshot->sourceFieldName;
					filter.operatorName = snapshot->operatorName.empty() ? "Equals" : snapshot->operatorName;
					filter.valueSource = "LoadedRecordIdentity";
					filter.entityId = snapshot->entityId;
					filter.entityName = snapshot->entityName.empty()
						? (mapping.entityName.empty() ? entity->description : mapping.entityName)
						: snapshot->entityName;
					filter.instanceId = snapshot->instanceId;
					filter.attributeId = snapshot->attributeId;
					filter.attributeName = snapshot->attributeName.empty() ? attribute->GetAttributeText() : snapshot->attributeName;
					filter.attributeCoreName = snapshot->attributeCoreName.empty() ? attribute->GetCoreName() : snapshot->attributeCoreName;
					filter.attributeDataType = snapshot->attributeDataType.empty() ? attributeMapping.attributeDataType : snapshot->attributeDataType;
					filter.valueType = snapshot->valueType.empty() ? AttributeTypeName(attribute->GetType()) : snapshot->valueType;
					filter.value = snapshot->value;
					filter.isKnown = snapshot->isKnown;
					filter.isCertain = snapshot->isCertain;
					filter.isUncertain = snapshot->isUncertain;
					filter.knowledgeState = snapshot->knowledgeState.empty() ? "Known" : snapshot->knowledgeState;
				}
				else
				{
					filter.sourceFieldName = attributeMapping.sourceFieldName;
					filter.operatorName = attributeMapping.loadFilterOperator.empty() ? "Equals" : attributeMapping.loadFilterOperator;
					filter.valueSource = "CurrentRuntimeValue";
					filter.entityId = mapping.entityId;
					filter.entityName = mapping.entityName.empty() ? entity->description : mapping.entityName;
					filter.instanceId = instance->instanceId;
					filter.attributeId = attributeMapping.attributeId;
					filter.attributeName = attributeMapping.attributeName.empty() ? attribute->GetAttributeText() : attributeMapping.attributeName;
					filter.attributeCoreName = attributeMapping.attributeCoreName.empty() ? attribute->GetCoreName() : attributeMapping.attributeCoreName;
					filter.attributeDataType = attributeMapping.attributeDataType;
					filter.valueType = AttributeTypeName(attribute->GetType());
					filter.value = AttributeValueToJson(attribute);
					filter.isKnown = attribute->IsKnown();
					filter.isCertain = attribute->IsCertain();
					filter.isUncertain = attribute->IsUncertain();
					filter.knowledgeState = Attribute::KnowledgeStateToString(attribute->GetKnowledgeState());
				}

				row.filters.push_back(filter);
			}

			for(const DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
			{
				if(!attributeMapping.saveConflictCheck)
				{
					continue;
				}

				const string fieldContext = mappingContext
					+ ":instance:" + instance->instanceId
					+ ":attribute:" + (attributeMapping.attributeId.empty() ? "<missing-attribute>" : attributeMapping.attributeId)
					+ ":field:" + (attributeMapping.sourceFieldName.empty() ? "<missing-field>" : attributeMapping.sourceFieldName);

				if(attributeMapping.sourceFieldName.empty())
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_conflict_check_field_missing",
					                      "Save plan saveConflictCheck mapping is missing sourceFieldName.",
					                      fieldContext);

					continue;
				}

				if(SaveRowHasFilterForField(row, attributeMapping.sourceFieldName))
				{
					continue;
				}

				shared_ptr<Attribute> attribute = instance->GetAttributeByID(attributeMapping.attributeId);

				if(attribute == nullptr)
				{
					AddSavePlanDiagnostic(plan,
					                      "error",
					                      "data_connection_save_conflict_check_attribute_missing",
					                      "Save plan could not resolve the runtime attribute for a save conflict check.",
					                      fieldContext);

					continue;
				}

				const DataConnectionRecordIdentitySnapshot* snapshot =
					FindOptimisticConcurrencySnapshot(*this, mapping, attributeMapping, instance->instanceId);

				if(snapshot == nullptr || !snapshot->isKnown || !snapshot->isCertain || snapshot->value.is_null())
				{
					AddSavePlanDiagnostic(plan,
					                      "warning",
					                      "data_connection_save_conflict_check_snapshot_missing",
					                      "Save plan skipped an optimistic concurrency check because no known/certain original loaded value snapshot is available.",
					                      fieldContext);

					continue;
				}

				DataConnectionSaveFilter filter;

				filter.sourceFieldName = snapshot->sourceFieldName;
				filter.operatorName = snapshot->operatorName.empty() ? "Equals" : snapshot->operatorName;
				filter.filterRole = "OptimisticConcurrencyCheck";
				filter.valueSource = "LoadedOptimisticConcurrencyCheck";
				filter.entityId = snapshot->entityId;
				filter.entityName = snapshot->entityName.empty()
					? (mapping.entityName.empty() ? entity->description : mapping.entityName)
					: snapshot->entityName;
				filter.instanceId = snapshot->instanceId;
				filter.attributeId = snapshot->attributeId;
				filter.attributeName = snapshot->attributeName.empty() ? attribute->GetAttributeText() : snapshot->attributeName;
				filter.attributeCoreName = snapshot->attributeCoreName.empty() ? attribute->GetCoreName() : snapshot->attributeCoreName;
				filter.attributeDataType = snapshot->attributeDataType.empty() ? attributeMapping.attributeDataType : snapshot->attributeDataType;
				filter.valueType = snapshot->valueType.empty() ? AttributeTypeName(attribute->GetType()) : snapshot->valueType;
				filter.value = snapshot->value;
				filter.isKnown = snapshot->isKnown;
				filter.isCertain = snapshot->isCertain;
				filter.isUncertain = snapshot->isUncertain;
				filter.knowledgeState = snapshot->knowledgeState.empty() ? "Known" : snapshot->knowledgeState;

				row.filters.push_back(filter);
			}

			table.rows.push_back(row);
		}
	}

	AddPendingGeneratedDependencyDiagnostics(plan);

	return plan;
}

RuleSet::DataConnectionLoadPlan RuleSet::BuildDataConnectionDeletedInstanceDeletePlan() const
{
	DataConnectionLoadPlan plan;

	if(dataConnectionRecordIdentitySnapshots == nullptr || dataConnectionRecordIdentitySnapshots->empty())
	{
		return plan;
	}

	const vector<DataConnectionSource> emptySources;
	const vector<DataConnectionSource>& sources = dataConnectionSources != nullptr ? *dataConnectionSources : emptySources;

	struct PendingDeletedTable
	{
		DataConnectionLoadTable table;
		bool entityIsRoot = false;
	};

	vector<PendingDeletedTable> pendingTables;
	unordered_map<string, size_t> pendingTableIndexByKey;

	for(const DataConnectionRecordIdentitySnapshot& snapshot : *dataConnectionRecordIdentitySnapshots)
	{
		if(!IsUsableDeletedRecordIdentitySnapshot(snapshot)
			|| RuntimeInstanceExists(*this, snapshot.entityId, snapshot.instanceId))
		{
			continue;
		}

		const DataConnectionEntityMapping* mapping = FindDataConnectionEntityMappingForSnapshot(*this, snapshot);
		const string snapshotContext = "dataConnectionDeletedInstanceDeletePlan:"
			+ (snapshot.sourceId.empty() ? "<missing-source>" : snapshot.sourceId)
			+ ":"
			+ (snapshot.sourceObjectName.empty() ? "<missing-object>" : snapshot.sourceObjectName)
			+ ":"
			+ (snapshot.entityId.empty() ? "<missing-entity>" : snapshot.entityId)
			+ ":instance:"
			+ (snapshot.instanceId.empty() ? "<missing-instance>" : snapshot.instanceId);

		if(mapping == nullptr)
		{
			AddLoadPlanDiagnostic(plan,
			                      "warning",
			                      "data_connection_delete_deleted_instance_mapping_missing",
			                      "Deleted-instance delete plan skipped a loaded row identity because the data-connection entity mapping is no longer available.",
			                      snapshotContext);
			continue;
		}

		const DataConnectionSource* source = FindDataConnectionSource(sources, mapping->sourceId);

		if(source == nullptr)
		{
			AddLoadPlanDiagnostic(plan,
			                      "error",
			                      "data_connection_delete_deleted_instance_source_missing",
			                      "Deleted-instance delete plan could not resolve the mapped data connection source.",
			                      snapshotContext);
		}

		if(mapping->sourceObjectName.empty())
		{
			AddLoadPlanDiagnostic(plan,
			                      "error",
			                      "data_connection_delete_deleted_instance_source_object_missing",
			                      "Deleted-instance delete plan entity mapping is missing sourceObjectName.",
			                      snapshotContext);
			continue;
		}

		shared_ptr<Entity> entity = FindEntityById(entities, snapshot.entityId);
		const string groupKey = DeletedInstanceDeletePlanGroupKey(snapshot);
		auto tableIndex = pendingTableIndexByKey.find(groupKey);

		if(tableIndex == pendingTableIndexByKey.end())
		{
			PendingDeletedTable pending;
			pending.table.sourceId = mapping->sourceId;
			pending.table.sourceName = source != nullptr ? source->sourceName : mapping->sourceName;
			pending.table.sourceType = source != nullptr ? source->sourceType : mapping->sourceType;
			pending.table.databaseProvider = source != nullptr ? source->databaseProvider : "";
			pending.table.serverConnectionKey = source != nullptr ? source->serverConnectionKey : "";
			pending.table.sourceObjectName = mapping->sourceObjectName;
			pending.entityIsRoot = entity != nullptr && entity->root.value_or(false);

			pendingTableIndexByKey[groupKey] = pendingTables.size();
			pendingTables.push_back(std::move(pending));
			tableIndex = pendingTableIndexByKey.find(groupKey);
		}

		PendingDeletedTable& pending = pendingTables[tableIndex->second];

		if(LoadTableHasFilterForField(pending.table, snapshot.sourceFieldName))
		{
			AddLoadPlanDiagnostic(plan,
			                      "warning",
			                      "data_connection_delete_deleted_instance_duplicate_filter",
			                      "Deleted-instance delete plan skipped a duplicate loaded record identity filter for the same backend field.",
			                      snapshotContext + ":field:" + snapshot.sourceFieldName);
			continue;
		}

		pending.table.filters.push_back(DeletedInstanceDeleteFilterFromSnapshot(snapshot, *mapping, entity));
	}

	for(const PendingDeletedTable& pending : pendingTables)
	{
		if(!pending.entityIsRoot)
		{
			plan.tables.push_back(pending.table);
		}
	}

	for(const PendingDeletedTable& pending : pendingTables)
	{
		if(pending.entityIsRoot)
		{
			plan.tables.push_back(pending.table);
		}
	}

	return plan;
}

int RuleSet::ClearDataConnectionSnapshotsForMissingInstances()
{
	if(dataConnectionRecordIdentitySnapshots == nullptr)
	{
		return 0;
	}

	const size_t before = dataConnectionRecordIdentitySnapshots->size();

	dataConnectionRecordIdentitySnapshots->erase(
		std::remove_if(dataConnectionRecordIdentitySnapshots->begin(),
		               dataConnectionRecordIdentitySnapshots->end(),
		               [this](const DataConnectionRecordIdentitySnapshot& snapshot)
		               {
			               return !snapshot.entityId.empty()
				               && !snapshot.instanceId.empty()
				               && !RuntimeInstanceExists(*this, snapshot.entityId, snapshot.instanceId);
		               }),
		dataConnectionRecordIdentitySnapshots->end());

	return static_cast<int>(before - dataConnectionRecordIdentitySnapshots->size());
}

RuleSet::DataConnectionLoadPlan RuleSet::BuildDataConnectionLoadPlan() const
{
	DataConnectionLoadPlan plan;

	if(dataConnectionEntityMappings == nullptr || dataConnectionEntityMappings->empty())
	{
		return plan;
	}

	const vector<DataConnectionSource> emptySources;
	const vector<DataConnectionSource>& sources = dataConnectionSources != nullptr ? *dataConnectionSources : emptySources;

	for(const DataConnectionEntityMapping& mapping : *dataConnectionEntityMappings)
	{
		const string mappingContext = "dataConnectionLoadPlan:"
			+ (mapping.sourceId.empty() ? "<missing-source>" : mapping.sourceId)
			+ ":"
			+ (mapping.sourceObjectName.empty() ? "<missing-object>" : mapping.sourceObjectName)
			+ ":"
			+ (mapping.entityId.empty() ? "<missing-entity>" : mapping.entityId);

		const DataConnectionSource* source = FindDataConnectionSource(sources, mapping.sourceId);

		if(source == nullptr)
		{
			AddLoadPlanDiagnostic(plan,
			                      "error",
			                      "data_connection_load_source_missing",
			                      "Load plan could not resolve the mapped data connection source.",
			                      mappingContext);
		}

		if(mapping.sourceObjectName.empty())
		{
			AddLoadPlanDiagnostic(plan,
			                      "error",
			                      "data_connection_load_source_object_missing",
			                      "Load plan entity mapping is missing sourceObjectName.",
			                      mappingContext);
		}

		shared_ptr<Entity> entity = FindEntityById(entities, mapping.entityId);

		if(entity == nullptr)
		{
			AddLoadPlanDiagnostic(plan,
			                      "error",
			                      "data_connection_load_entity_missing",
			                      "Load plan could not resolve the mapped runtime entity.",
			                      mappingContext);

			continue;
		}

		size_t selectableMappingCount = 0;

		for(const DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
		{
			if(attributeMapping.input || attributeMapping.saveConflictCheck)
			{
				selectableMappingCount++;
			}
		}

		if(selectableMappingCount == 0)
		{
			AddLoadPlanDiagnostic(plan,
			                      "warning",
			                      "data_connection_load_input_mappings_missing",
			                      "Load plan entity mapping has no input-enabled or saveConflictCheck attribute mappings.",
			                      mappingContext);

			continue;
		}

		DataConnectionLoadTable& table = FindOrCreateLoadTable(plan, mapping, source);
		shared_ptr<Instance> filterInstance;

		if(entity->instances.size() > 1)
		{
			AddLoadPlanDiagnostic(plan,
			                      "error",
			                      "data_connection_load_filter_multiple_instances",
			                      "Load plan cannot build unambiguous record-selection filters because the mapped entity has multiple runtime instances.",
			                      mappingContext);
		}
		else if(!entity->instances.empty())
		{
			filterInstance = entity->instances.front();
		}

		for(const DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
		{
			if(!attributeMapping.input && !attributeMapping.saveConflictCheck)
			{
				continue;
			}

			const string fieldContext = mappingContext
				+ ":attribute:" + (attributeMapping.attributeId.empty() ? "<missing-attribute>" : attributeMapping.attributeId)
				+ ":field:" + (attributeMapping.sourceFieldName.empty() ? "<missing-field>" : attributeMapping.sourceFieldName);

			if(attributeMapping.sourceFieldName.empty())
			{
				AddLoadPlanDiagnostic(plan,
				                      "error",
				                      "data_connection_load_field_missing",
				                      "Load plan input mapping is missing sourceFieldName.",
				                      fieldContext);

				continue;
			}

			shared_ptr<Attribute> attribute = FindEntityAttributeById(entity, attributeMapping.attributeId);

			if(attribute == nullptr)
			{
				AddLoadPlanDiagnostic(plan,
				                      "error",
				                      "data_connection_load_attribute_missing",
				                      "Load plan could not resolve the mapped runtime attribute definition.",
				                      fieldContext);

				continue;
			}

			DataConnectionLoadTarget target;

			target.entityId = mapping.entityId;
			target.entityName = mapping.entityName.empty() ? entity->description : mapping.entityName;
			target.attributeId = attributeMapping.attributeId;
			target.attributeName = attributeMapping.attributeName.empty() ? attribute->GetAttributeText() : attributeMapping.attributeName;
			target.attributeCoreName = attributeMapping.attributeCoreName.empty() ? attribute->GetCoreName() : attributeMapping.attributeCoreName;
			target.attributeDataType = attributeMapping.attributeDataType;
			target.valueType = AttributeTypeName(attribute->GetType());

			DataConnectionLoadField& field = FindOrCreateLoadField(table, attributeMapping.sourceFieldName);
			field.targets.push_back(target);
		}

		for(const DataConnectionAttributeMapping& attributeMapping : mapping.attributeMappings)
		{
			if(!AttributeMappingUsesLoadFilter(attributeMapping))
			{
				continue;
			}

			const string fieldContext = mappingContext
				+ ":attribute:" + (attributeMapping.attributeId.empty() ? "<missing-attribute>" : attributeMapping.attributeId)
				+ ":field:" + (attributeMapping.sourceFieldName.empty() ? "<missing-field>" : attributeMapping.sourceFieldName);

			if(attributeMapping.sourceFieldName.empty())
			{
				AddLoadPlanDiagnostic(plan,
				                      "error",
				                      "data_connection_load_filter_field_missing",
				                      "Load plan loadFilter mapping is missing sourceFieldName.",
				                      fieldContext);

				continue;
			}

			if(LoadTableHasFilterForField(table, attributeMapping.sourceFieldName))
			{
				continue;
			}

			const string valueSource = NormalizeDataConnectionLoadFilterValueSource(attributeMapping.loadFilterValueSource);

			if(valueSource == "SessionUserId")
			{
				if(sessionContext.userId.empty())
				{
					AddLoadPlanDiagnostic(plan,
					                      "warning",
					                      "data_connection_load_filter_user_id_missing",
					                      "Load plan skipped a signed-in user record-selection filter because the session user id is not available.",
					                      fieldContext);

					continue;
				}

				shared_ptr<Attribute> attribute = FindEntityAttributeById(entity, attributeMapping.attributeId);
				const string attributeValueType = attribute != nullptr ? AttributeTypeName(attribute->GetType()) : "Text";

				if(!DataConnectionMappingCanUseSessionUserId(attributeValueType))
				{
					AddLoadPlanDiagnostic(plan,
					                      "error",
					                      "data_connection_load_filter_session_user_type_mismatch",
					                      "Load plan signed-in user record-selection filters must target Text attributes because generated signed-in user ids are text.",
					                      fieldContext);

					continue;
				}

				DataConnectionLoadFilter filter;

				filter.sourceFieldName = attributeMapping.sourceFieldName;
				filter.operatorName = attributeMapping.loadFilterOperator.empty() ? "Equals" : attributeMapping.loadFilterOperator;
				filter.valueSource = valueSource;
				filter.entityId = mapping.entityId;
				filter.entityName = mapping.entityName.empty() ? entity->description : mapping.entityName;
				filter.instanceId = filterInstance != nullptr ? filterInstance->instanceId : string();
				filter.attributeId = attributeMapping.attributeId;
				filter.attributeName = attributeMapping.attributeName.empty() && attribute != nullptr ? attribute->GetAttributeText() : attributeMapping.attributeName;
				filter.attributeCoreName = attributeMapping.attributeCoreName.empty() && attribute != nullptr ? attribute->GetCoreName() : attributeMapping.attributeCoreName;
				filter.attributeDataType = attributeMapping.attributeDataType;
				filter.valueType = "Text";
				filter.value = sessionContext.userId;
				filter.isKnown = true;
				filter.isCertain = true;
				filter.isUncertain = false;
				filter.knowledgeState = "Certain";

				table.filters.push_back(filter);

				continue;
			}

			if(filterInstance == nullptr)
			{
				continue;
			}

			shared_ptr<Attribute> runtimeAttribute = filterInstance->GetAttributeByID(attributeMapping.attributeId);

			if(runtimeAttribute == nullptr)
			{
				AddLoadPlanDiagnostic(plan,
				                      "error",
				                      "data_connection_load_filter_attribute_missing",
				                      "Load plan could not resolve the runtime attribute value for a record-selection filter.",
				                      fieldContext);

				continue;
			}

			if(!runtimeAttribute->IsKnown())
			{
				AddLoadPlanDiagnostic(plan,
				                      "warning",
				                      "data_connection_load_filter_value_unknown",
				                      "Load plan skipped a record-selection filter because the mapped runtime attribute value is unknown.",
				                      fieldContext);

				continue;
			}

			if(!runtimeAttribute->IsCertain())
			{
				AddLoadPlanDiagnostic(plan,
				                      "warning",
				                      "data_connection_load_filter_value_uncertain",
				                      "Load plan skipped a record-selection filter because the mapped runtime attribute value is uncertain.",
				                      fieldContext);

				continue;
			}

			DataConnectionLoadFilter filter;

			filter.sourceFieldName = attributeMapping.sourceFieldName;
			filter.operatorName = attributeMapping.loadFilterOperator.empty() ? "Equals" : attributeMapping.loadFilterOperator;
			filter.valueSource = valueSource;
			filter.entityId = mapping.entityId;
			filter.entityName = mapping.entityName.empty() ? entity->description : mapping.entityName;
			filter.instanceId = filterInstance->instanceId;
			filter.attributeId = attributeMapping.attributeId;
			filter.attributeName = attributeMapping.attributeName.empty() ? runtimeAttribute->GetAttributeText() : attributeMapping.attributeName;
			filter.attributeCoreName = attributeMapping.attributeCoreName.empty() ? runtimeAttribute->GetCoreName() : attributeMapping.attributeCoreName;
			filter.attributeDataType = attributeMapping.attributeDataType;
			filter.valueType = AttributeTypeName(runtimeAttribute->GetType());
			filter.value = AttributeValueToJson(runtimeAttribute);
			filter.isKnown = runtimeAttribute->IsKnown();
			filter.isCertain = runtimeAttribute->IsCertain();
			filter.isUncertain = runtimeAttribute->IsUncertain();
			filter.knowledgeState = Attribute::KnowledgeStateToString(runtimeAttribute->GetKnowledgeState());

			table.filters.push_back(filter);
		}
	}

	for(const DataConnectionLoadTable& table : plan.tables)
	{
		if(!table.fields.empty() && table.filters.empty())
		{
			AddLoadPlanDiagnostic(plan,
			                      "warning",
			                      "data_connection_load_filters_missing",
			                      "Load plan table has selectable input fields but no known record-selection filters yet. At least one loadFilter attribute value must be known before executing a load.",
			                      "dataConnectionLoadPlan:" + table.sourceId + ":" + table.sourceObjectName);
		}
	}

	return plan;
}

json RuleSet::ToJSON() const
{
	json j;

	j["uploads"] = *uploads;
	j["entities"] = json::array();
	
	for(const auto& entity : *entities)
	{
		j["entities"].push_back(entity->CreateJSON());
	}

	const size_t ruleCount = sessionModelRules != nullptr ? sessionModelRules->size() : 0;
	size_t activeRuleCount = 0;

	if(sessionModelRules != nullptr)
	{
		for(const SessionModelRuleMetadata& rule : *sessionModelRules)
		{
			if(rule.isActive)
			{
				activeRuleCount++;
			}
		}
	}

	json validationEvents = json::array();

	if(activeValidationEventsByKey != nullptr)
	{
		for(const auto& entry : *activeValidationEventsByKey)
		{
			validationEvents.push_back(entry.second);
		}
	}

	json attributeHistory = json::array();
	json entityExistenceHistory = json::array();
	json relationshipLinkHistory = json::array();
	json relationshipCoverageHistory = json::array();

	if(attributeHistorySegments != nullptr)
	{
		for(const AttributeHistorySegment& segment : *attributeHistorySegments)
		{
			attributeHistory.push_back(segment);
		}
	}

	if(entityExistenceHistorySegments != nullptr)
	{
		for(const EntityExistenceHistorySegment& segment : *entityExistenceHistorySegments)
		{
			entityExistenceHistory.push_back(segment);
		}
	}

	if(relationshipLinkHistorySegments != nullptr)
	{
		for(const RelationshipLinkHistorySegment& segment : *relationshipLinkHistorySegments)
		{
			relationshipLinkHistory.push_back(segment);
		}
	}

	if(relationshipCoverageHistorySegments != nullptr)
	{
		for(const RelationshipCoverageHistorySegment& segment : *relationshipCoverageHistorySegments)
		{
			relationshipCoverageHistory.push_back(segment);
		}
	}

	size_t dataConnectionAttributeMappingCount = 0;
	size_t dataConnectionRelationshipFieldMappingCount = 0;

	if(dataConnectionEntityMappings != nullptr)
	{
		for(const DataConnectionEntityMapping& mapping : *dataConnectionEntityMappings)
		{
			dataConnectionAttributeMappingCount += mapping.attributeMappings.size();
			dataConnectionRelationshipFieldMappingCount += mapping.relationshipFieldMappings.size();
		}
	}

	const DataConnectionSavePlan dataConnectionSavePlan = BuildDataConnectionSavePlan();
	const DataConnectionLoadPlan dataConnectionLoadPlan = BuildDataConnectionLoadPlan();
	const DataConnectionLoadPlan dataConnectionDeletedInstanceDeletePlan = BuildDataConnectionDeletedInstanceDeletePlan();
	const json dataConnectionRecordIdentitySnapshotsJson =
		dataConnectionRecordIdentitySnapshots != nullptr ? json(*dataConnectionRecordIdentitySnapshots) : json::array();

	j["sessionModel"] = {
		{"ruleCount", ruleCount},
		{"activeRuleCount", activeRuleCount},
		{"startInputCount", startInputs != nullptr ? startInputs->size() : 0},
		{"dataConnectionSourceCount", dataConnectionSources != nullptr ? dataConnectionSources->size() : 0},
		{"dataConnectionEntityMappingCount", dataConnectionEntityMappings != nullptr ? dataConnectionEntityMappings->size() : 0},
		{"dataConnectionAttributeMappingCount", dataConnectionAttributeMappingCount},
		{"dataConnectionRelationshipFieldMappingCount", dataConnectionRelationshipFieldMappingCount},
		{"dataConnectionRecordIdentitySnapshotCount", dataConnectionRecordIdentitySnapshots != nullptr ? dataConnectionRecordIdentitySnapshots->size() : 0},
		{"dataConnectionRecordIdentitySnapshots", dataConnectionRecordIdentitySnapshotsJson},
		{"dataConnectionLoadPlan", dataConnectionLoadPlan.ToJSON()},
		{"dataConnectionSavePlan", dataConnectionSavePlan.ToJSON()},
		{"dataConnectionDeletedInstanceDeletePlan", dataConnectionDeletedInstanceDeletePlan.ToJSON()},
		{"referencedAttributeDependencyCount", activeSessionModelRuleIdsByReferencedAttribute != nullptr ? activeSessionModelRuleIdsByReferencedAttribute->size() : 0},
		{"referencedRelationshipDependencyCount", activeSessionModelRuleIdsByReferencedRelationship != nullptr ? activeSessionModelRuleIdsByReferencedRelationship->size() : 0},
		{"referencedFunctionDependencyCount", activeSessionModelRuleIdsByReferencedFunction != nullptr ? activeSessionModelRuleIdsByReferencedFunction->size() : 0},
		{"concludedAttributeDependencyCount", activeSessionModelRuleIdsByConcludedAttribute != nullptr ? activeSessionModelRuleIdsByConcludedAttribute->size() : 0},
		{"inferredInstanceCount", inferredInstanceIdsByKey != nullptr ? inferredInstanceIdsByKey->size() : 0},
		{"activeValidationEventCount", activeValidationEventsByKey != nullptr ? activeValidationEventsByKey->size() : 0},
		{"attributeHistorySegmentCount", attributeHistorySegments != nullptr ? attributeHistorySegments->size() : 0},
		{"attributeHistoryDependencyCount", attributeHistorySegmentIndexesByAttribute != nullptr ? attributeHistorySegmentIndexesByAttribute->size() : 0},
		{"attributeHistorySegments", attributeHistory},
		{"entityExistenceHistorySegmentCount", entityExistenceHistorySegments != nullptr ? entityExistenceHistorySegments->size() : 0},
		{"entityExistenceHistoryDependencyCount", entityExistenceHistorySegmentIndexesByInstance != nullptr ? entityExistenceHistorySegmentIndexesByInstance->size() : 0},
		{"entityExistenceHistorySegments", entityExistenceHistory},
		{"relationshipLinkHistorySegmentCount", relationshipLinkHistorySegments != nullptr ? relationshipLinkHistorySegments->size() : 0},
		{"relationshipLinkHistorySourceDependencyCount", relationshipLinkHistorySegmentIndexesBySource != nullptr ? relationshipLinkHistorySegmentIndexesBySource->size() : 0},
		{"relationshipLinkHistoryTargetDependencyCount", relationshipLinkHistorySegmentIndexesByTarget != nullptr ? relationshipLinkHistorySegmentIndexesByTarget->size() : 0},
		{"relationshipLinkHistoryDependencyCount", relationshipLinkHistorySegmentIndexesByLink != nullptr ? relationshipLinkHistorySegmentIndexesByLink->size() : 0},
		{"relationshipLinkHistorySegments", relationshipLinkHistory},
		{"relationshipCoverageHistorySegmentCount", relationshipCoverageHistorySegments != nullptr ? relationshipCoverageHistorySegments->size() : 0},
		{"relationshipCoverageHistoryDependencyCount", relationshipCoverageHistorySegmentIndexesBySource != nullptr ? relationshipCoverageHistorySegmentIndexesBySource->size() : 0},
		{"relationshipCoverageHistorySegments", relationshipCoverageHistory},
		{"validationEvents", validationEvents},
		{"sessionStartDateTime", sessionContext.sessionStartDateTime},
		{"userId", sessionContext.userId},
		{"userName", sessionContext.userName},
		{"currentLocale", sessionContext.currentLocale},
		{"deploymentName", sessionContext.deploymentName},
		{"deploymentVersion", sessionContext.deploymentVersion},
		{"asOfTransactionTime", sessionContext.asOfTransactionTime},
		{"rules", sessionModelRules != nullptr ? json(*sessionModelRules) : json::array()}
	};

	return j;
}

void RuleSet::ResetSessionStartDateTime()
{
	sessionContext.sessionStartDateTime = CurrentLocalDateTime();
}

void RuleSet::SetSessionStartDateTime(const string& value)
{
	if(value.empty())
	{
		ResetSessionStartDateTime();

		return;
	}

	sessionContext.sessionStartDateTime = value;
}

void RuleSet::SetSessionMetadata(const string& currentLocale, const string& deploymentName, const string& deploymentVersion)
{
	sessionContext.currentLocale = currentLocale.empty() ? "en-GB" : currentLocale;
	sessionContext.deploymentName = deploymentName;
	sessionContext.deploymentVersion = deploymentVersion;
}

void RuleSet::SetHistorySnapshotTime(const string& value)
{
	sessionContext.asOfTransactionTime = SessionModelTemporalSupport::Trim(value);
}

string RuleSet::GetHistorySnapshotTime() const
{
	return sessionContext.asOfTransactionTime;
}

string RuleSet::GetSessionCurrentDate() const
{
	return sessionContext.sessionStartDateTime.size() >= 10
		? sessionContext.sessionStartDateTime.substr(0, 10)
		: sessionContext.sessionStartDateTime;
}

string RuleSet::GetSessionCurrentDateTime() const
{
	return sessionContext.sessionStartDateTime;
}

string RuleSet::GetSessionCurrentTime() const
{
	return sessionContext.sessionStartDateTime;
}

string RuleSet::AttributeDependencyKey(const string& entityId, const string& attributeId)
{
	return entityId + "\x1f" + attributeId;
}

string RuleSet::GetAttributeForgetPolicyMode(const string& entityId, const string& attributeId) const
{
	if(attributeForgetPolicyModeByKey == nullptr)
	{
		return "Forget";
	}

	auto it = attributeForgetPolicyModeByKey->find(AttributeDependencyKey(entityId, attributeId));

	return it == attributeForgetPolicyModeByKey->end() ? "Forget" : it->second;
}

string RuleSet::GetRelationshipForgetPolicyMode(const string& relationshipId) const
{
	if(relationshipForgetPolicyModeById == nullptr)
	{
		return "Forget";
	}

	auto it = relationshipForgetPolicyModeById->find(relationshipId);

	return it == relationshipForgetPolicyModeById->end() ? "Forget" : it->second;
}

string RuleSet::AttributeHistoryDependencyKey(const string& instanceId, const string& attributeId)
{
	return instanceId + "\x1f" + attributeId;
}

bool RuleSet::IsValidTransactionTime(const string& value)
{
	return SessionModelTemporalSupport::IsValidTransactionTime(value);
}

bool RuleSet::IsValidTransactionWindow(const string& transactionStart, const string& transactionEnd)
{
	return SessionModelTemporalSupport::IsValidTransactionWindow(transactionStart, transactionEnd);
}

string RuleSet::EntityExistenceHistoryDependencyKey(const string& entityId, const string& instanceId)
{
	return entityId + "\x1f" + instanceId;
}

string RuleSet::RelationshipLinkHistorySourceDependencyKey(const string& relationshipId, const string& sourceInstanceId)
{
	return relationshipId + "\x1f" + sourceInstanceId;
}

string RuleSet::RelationshipLinkHistoryTargetDependencyKey(const string& relationshipId, const string& targetInstanceId)
{
	return relationshipId + "\x1f" + targetInstanceId;
}

string RuleSet::RelationshipLinkHistoryDependencyKey(const string& relationshipId, const string& sourceInstanceId, const string& targetInstanceId)
{
	return relationshipId + "\x1f" + sourceInstanceId + "\x1f" + targetInstanceId;
}

string RuleSet::RelationshipCoverageHistoryDependencyKey(const string& relationshipId, const string& sourceInstanceId)
{
	return relationshipId + "\x1f" + sourceInstanceId;
}

void RuleSet::AddAttributeHistorySegment(const AttributeHistorySegment& segment)
{
	if(attributeHistorySegments == nullptr)
	{
		attributeHistorySegments = make_shared<vector<AttributeHistorySegment>>();
	}

	if(attributeHistorySegmentIndexesByAttribute == nullptr)
	{
		attributeHistorySegmentIndexesByAttribute = make_shared<unordered_map<string, vector<size_t>>>();
	}

	const size_t index = attributeHistorySegments->size();

	attributeHistorySegments->push_back(segment);

	(*attributeHistorySegmentIndexesByAttribute)[AttributeHistoryDependencyKey(segment.instanceId, segment.attributeId)].push_back(index);
}

void RuleSet::RebuildAttributeHistoryIndex()
{
	if(attributeHistorySegmentIndexesByAttribute == nullptr)
	{
		attributeHistorySegmentIndexesByAttribute = make_shared<unordered_map<string, vector<size_t>>>();
	}

	attributeHistorySegmentIndexesByAttribute->clear();

	if(attributeHistorySegments == nullptr)
	{
		return;
	}

	for(size_t index = 0; index < attributeHistorySegments->size(); ++index)
	{
		const AttributeHistorySegment& segment = attributeHistorySegments->at(index);

		(*attributeHistorySegmentIndexesByAttribute)[AttributeHistoryDependencyKey(segment.instanceId, segment.attributeId)].push_back(index);
	}
}

vector<const RuleSet::AttributeHistorySegment*> RuleSet::GetAttributeHistorySegments(const string& instanceId, const string& attributeId) const
{
	vector<const AttributeHistorySegment*> segments;

	if(attributeHistorySegments == nullptr || attributeHistorySegmentIndexesByAttribute == nullptr)
	{
		return segments;
	}

	auto it = attributeHistorySegmentIndexesByAttribute->find(AttributeHistoryDependencyKey(instanceId, attributeId));

	if(it == attributeHistorySegmentIndexesByAttribute->end())
	{
		return segments;
	}

	segments.reserve(it->second.size());

	const string snapshotTime = GetHistorySnapshotTime();

	for(size_t index : it->second)
	{
		if(index < attributeHistorySegments->size())
		{
			const AttributeHistorySegment& segment = attributeHistorySegments->at(index);

			if(SessionModelTemporalSupport::TransactionWindowVisibleAtSnapshot(segment.transactionStart,
			                                                                   segment.transactionEnd,
			                                                                   snapshotTime))
			{
				segments.push_back(&segment);
			}
		}
	}

	return segments;
}

void RuleSet::AddEntityExistenceHistorySegment(const EntityExistenceHistorySegment& segment)
{
	if(entityExistenceHistorySegments == nullptr)
	{
		entityExistenceHistorySegments = make_shared<vector<EntityExistenceHistorySegment>>();
	}

	if(entityExistenceHistorySegmentIndexesByInstance == nullptr)
	{
		entityExistenceHistorySegmentIndexesByInstance = make_shared<unordered_map<string, vector<size_t>>>();
	}

	const size_t index = entityExistenceHistorySegments->size();

	entityExistenceHistorySegments->push_back(segment);

	(*entityExistenceHistorySegmentIndexesByInstance)[EntityExistenceHistoryDependencyKey(segment.entityId, segment.instanceId)].push_back(index);
}

void RuleSet::RebuildEntityExistenceHistoryIndex()
{
	if(entityExistenceHistorySegmentIndexesByInstance == nullptr)
	{
		entityExistenceHistorySegmentIndexesByInstance = make_shared<unordered_map<string, vector<size_t>>>();
	}

	entityExistenceHistorySegmentIndexesByInstance->clear();

	if(entityExistenceHistorySegments == nullptr)
	{
		return;
	}

	for(size_t index = 0; index < entityExistenceHistorySegments->size(); ++index)
	{
		const EntityExistenceHistorySegment& segment = entityExistenceHistorySegments->at(index);

		(*entityExistenceHistorySegmentIndexesByInstance)[EntityExistenceHistoryDependencyKey(segment.entityId, segment.instanceId)].push_back(index);
	}
}

vector<const RuleSet::EntityExistenceHistorySegment*> RuleSet::GetEntityExistenceHistorySegments(const string& entityId, const string& instanceId) const
{
	vector<const EntityExistenceHistorySegment*> segments;

	if(entityExistenceHistorySegments == nullptr || entityExistenceHistorySegmentIndexesByInstance == nullptr)
	{
		return segments;
	}

	auto it = entityExistenceHistorySegmentIndexesByInstance->find(EntityExistenceHistoryDependencyKey(entityId, instanceId));

	if(it == entityExistenceHistorySegmentIndexesByInstance->end())
	{
		return segments;
	}

	segments.reserve(it->second.size());

	const string snapshotTime = GetHistorySnapshotTime();

	for(size_t index : it->second)
	{
		if(index < entityExistenceHistorySegments->size())
		{
			const EntityExistenceHistorySegment& segment = entityExistenceHistorySegments->at(index);

			if(SessionModelTemporalSupport::TransactionWindowVisibleAtSnapshot(segment.transactionStart,
			                                                                   segment.transactionEnd,
			                                                                   snapshotTime))
			{
				segments.push_back(&segment);
			}
		}
	}

	return segments;
}

void RuleSet::AddRelationshipLinkHistorySegment(const RelationshipLinkHistorySegment& segment)
{
	if(relationshipLinkHistorySegments == nullptr)
	{
		relationshipLinkHistorySegments = make_shared<vector<RelationshipLinkHistorySegment>>();
	}

	if(relationshipLinkHistorySegmentIndexesBySource == nullptr)
	{
		relationshipLinkHistorySegmentIndexesBySource = make_shared<unordered_map<string, vector<size_t>>>();
	}

	if(relationshipLinkHistorySegmentIndexesByLink == nullptr)
	{
		relationshipLinkHistorySegmentIndexesByLink = make_shared<unordered_map<string, vector<size_t>>>();
	}

	if(relationshipLinkHistorySegmentIndexesByTarget == nullptr)
	{
		relationshipLinkHistorySegmentIndexesByTarget = make_shared<unordered_map<string, vector<size_t>>>();
	}

	const size_t index = relationshipLinkHistorySegments->size();

	relationshipLinkHistorySegments->push_back(segment);

	(*relationshipLinkHistorySegmentIndexesBySource)[RelationshipLinkHistorySourceDependencyKey(segment.relationshipId, segment.sourceInstanceId)].push_back(index);
	(*relationshipLinkHistorySegmentIndexesByTarget)[RelationshipLinkHistoryTargetDependencyKey(segment.relationshipId, segment.targetInstanceId)].push_back(index);
	(*relationshipLinkHistorySegmentIndexesByLink)[RelationshipLinkHistoryDependencyKey(segment.relationshipId, segment.sourceInstanceId, segment.targetInstanceId)].push_back(index);
}

void RuleSet::RebuildRelationshipLinkHistoryIndex()
{
	if(relationshipLinkHistorySegmentIndexesBySource == nullptr)
	{
		relationshipLinkHistorySegmentIndexesBySource = make_shared<unordered_map<string, vector<size_t>>>();
	}

	if(relationshipLinkHistorySegmentIndexesByLink == nullptr)
	{
		relationshipLinkHistorySegmentIndexesByLink = make_shared<unordered_map<string, vector<size_t>>>();
	}

	if(relationshipLinkHistorySegmentIndexesByTarget == nullptr)
	{
		relationshipLinkHistorySegmentIndexesByTarget = make_shared<unordered_map<string, vector<size_t>>>();
	}

	relationshipLinkHistorySegmentIndexesBySource->clear();
	relationshipLinkHistorySegmentIndexesByTarget->clear();
	relationshipLinkHistorySegmentIndexesByLink->clear();

	if(relationshipLinkHistorySegments == nullptr)
	{
		return;
	}

	for(size_t index = 0; index < relationshipLinkHistorySegments->size(); ++index)
	{
		const RelationshipLinkHistorySegment& segment = relationshipLinkHistorySegments->at(index);

		(*relationshipLinkHistorySegmentIndexesBySource)[RelationshipLinkHistorySourceDependencyKey(segment.relationshipId, segment.sourceInstanceId)].push_back(index);
		(*relationshipLinkHistorySegmentIndexesByTarget)[RelationshipLinkHistoryTargetDependencyKey(segment.relationshipId, segment.targetInstanceId)].push_back(index);
		(*relationshipLinkHistorySegmentIndexesByLink)[RelationshipLinkHistoryDependencyKey(segment.relationshipId, segment.sourceInstanceId, segment.targetInstanceId)].push_back(index);
	}
}

vector<const RuleSet::RelationshipLinkHistorySegment*> RuleSet::GetRelationshipLinkHistorySegments(const string& relationshipId, const string& sourceInstanceId) const
{
	vector<const RelationshipLinkHistorySegment*> segments;

	if(relationshipLinkHistorySegments == nullptr || relationshipLinkHistorySegmentIndexesBySource == nullptr)
	{
		return segments;
	}

	auto it = relationshipLinkHistorySegmentIndexesBySource->find(RelationshipLinkHistorySourceDependencyKey(relationshipId, sourceInstanceId));

	if(it == relationshipLinkHistorySegmentIndexesBySource->end())
	{
		return segments;
	}

	segments.reserve(it->second.size());

	const string snapshotTime = GetHistorySnapshotTime();

	for(size_t index : it->second)
	{
		if(index < relationshipLinkHistorySegments->size())
		{
			const RelationshipLinkHistorySegment& segment = relationshipLinkHistorySegments->at(index);

			if(SessionModelTemporalSupport::TransactionWindowVisibleAtSnapshot(segment.transactionStart,
			                                                                   segment.transactionEnd,
			                                                                   snapshotTime))
			{
				segments.push_back(&segment);
			}
		}
	}

	return segments;
}

vector<const RuleSet::RelationshipLinkHistorySegment*> RuleSet::GetRelationshipLinkHistorySegmentsByTarget(const string& relationshipId, const string& targetInstanceId) const
{
	vector<const RelationshipLinkHistorySegment*> segments;

	if(relationshipLinkHistorySegments == nullptr || relationshipLinkHistorySegmentIndexesByTarget == nullptr)
	{
		return segments;
	}

	auto it = relationshipLinkHistorySegmentIndexesByTarget->find(RelationshipLinkHistoryTargetDependencyKey(relationshipId, targetInstanceId));

	if(it == relationshipLinkHistorySegmentIndexesByTarget->end())
	{
		return segments;
	}

	segments.reserve(it->second.size());

	const string snapshotTime = GetHistorySnapshotTime();

	for(size_t index : it->second)
	{
		if(index < relationshipLinkHistorySegments->size())
		{
			const RelationshipLinkHistorySegment& segment = relationshipLinkHistorySegments->at(index);

			if(SessionModelTemporalSupport::TransactionWindowVisibleAtSnapshot(segment.transactionStart,
			                                                                   segment.transactionEnd,
			                                                                   snapshotTime))
			{
				segments.push_back(&segment);
			}
		}
	}

	return segments;
}

vector<const RuleSet::RelationshipLinkHistorySegment*> RuleSet::GetRelationshipLinkHistorySegments(const string& relationshipId, const string& sourceInstanceId, const string& targetInstanceId) const
{
	vector<const RelationshipLinkHistorySegment*> segments;

	if(relationshipLinkHistorySegments == nullptr || relationshipLinkHistorySegmentIndexesByLink == nullptr)
	{
		return segments;
	}

	auto it = relationshipLinkHistorySegmentIndexesByLink->find(RelationshipLinkHistoryDependencyKey(relationshipId, sourceInstanceId, targetInstanceId));

	if(it == relationshipLinkHistorySegmentIndexesByLink->end())
	{
		return segments;
	}

	segments.reserve(it->second.size());

	const string snapshotTime = GetHistorySnapshotTime();

	for(size_t index : it->second)
	{
		if(index < relationshipLinkHistorySegments->size())
		{
			const RelationshipLinkHistorySegment& segment = relationshipLinkHistorySegments->at(index);

			if(SessionModelTemporalSupport::TransactionWindowVisibleAtSnapshot(segment.transactionStart,
			                                                                   segment.transactionEnd,
			                                                                   snapshotTime))
			{
				segments.push_back(&segment);
			}
		}
	}

	return segments;
}

void RuleSet::AddRelationshipCoverageHistorySegment(const RelationshipCoverageHistorySegment& segment)
{
	if(relationshipCoverageHistorySegments == nullptr)
	{
		relationshipCoverageHistorySegments = make_shared<vector<RelationshipCoverageHistorySegment>>();
	}

	if(relationshipCoverageHistorySegmentIndexesBySource == nullptr)
	{
		relationshipCoverageHistorySegmentIndexesBySource = make_shared<unordered_map<string, vector<size_t>>>();
	}

	const size_t index = relationshipCoverageHistorySegments->size();

	relationshipCoverageHistorySegments->push_back(segment);

	(*relationshipCoverageHistorySegmentIndexesBySource)[RelationshipCoverageHistoryDependencyKey(segment.relationshipId, segment.sourceInstanceId)].push_back(index);
}

void RuleSet::RebuildRelationshipCoverageHistoryIndex()
{
	if(relationshipCoverageHistorySegmentIndexesBySource == nullptr)
	{
		relationshipCoverageHistorySegmentIndexesBySource = make_shared<unordered_map<string, vector<size_t>>>();
	}

	relationshipCoverageHistorySegmentIndexesBySource->clear();

	if(relationshipCoverageHistorySegments == nullptr)
	{
		return;
	}

	for(size_t index = 0; index < relationshipCoverageHistorySegments->size(); ++index)
	{
		const RelationshipCoverageHistorySegment& segment = relationshipCoverageHistorySegments->at(index);

		(*relationshipCoverageHistorySegmentIndexesBySource)[RelationshipCoverageHistoryDependencyKey(segment.relationshipId, segment.sourceInstanceId)].push_back(index);
	}
}

vector<const RuleSet::RelationshipCoverageHistorySegment*> RuleSet::GetRelationshipCoverageHistorySegments(const string& relationshipId, const string& sourceInstanceId) const
{
	vector<const RelationshipCoverageHistorySegment*> segments;

	if(relationshipCoverageHistorySegments == nullptr || relationshipCoverageHistorySegmentIndexesBySource == nullptr)
	{
		return segments;
	}

	auto it = relationshipCoverageHistorySegmentIndexesBySource->find(RelationshipCoverageHistoryDependencyKey(relationshipId, sourceInstanceId));

	if(it == relationshipCoverageHistorySegmentIndexesBySource->end())
	{
		return segments;
	}

	segments.reserve(it->second.size());

	const string snapshotTime = GetHistorySnapshotTime();

	for(size_t index : it->second)
	{
		if(index < relationshipCoverageHistorySegments->size())
		{
			const RelationshipCoverageHistorySegment& segment = relationshipCoverageHistorySegments->at(index);

			if(SessionModelTemporalSupport::TransactionWindowVisibleAtSnapshot(segment.transactionStart,
			                                                                   segment.transactionEnd,
			                                                                   snapshotTime))
			{
				segments.push_back(&segment);
			}
		}
	}

	return segments;
}

void RuleSet::RebuildSessionModelRuleIndex()
{
	if(sessionModelRuleIndexById == nullptr)
	{
		sessionModelRuleIndexById = make_shared<unordered_map<string, size_t>>();
	}

	sessionModelRuleIndexById->clear();

	if(sessionModelRules == nullptr)
	{
		return;
	}

	for(size_t index = 0; index < sessionModelRules->size(); ++index)
	{
		const string& ruleId = sessionModelRules->at(index).ruleId;

		if(!ruleId.empty())
		{
			(*sessionModelRuleIndexById)[ruleId] = index;
		}
	}
}

const RuleSet::SessionModelRuleMetadata* RuleSet::GetSessionModelRuleById(const string& ruleId) const
{
	if(sessionModelRules == nullptr)
	{
		return nullptr;
	}

	if(sessionModelRuleIndexById != nullptr && !sessionModelRuleIndexById->empty())
	{
		auto it = sessionModelRuleIndexById->find(ruleId);

		if(it != sessionModelRuleIndexById->end() && it->second < sessionModelRules->size())
		{
			return &sessionModelRules->at(it->second);
		}
	}

	for(const SessionModelRuleMetadata& rule : *sessionModelRules)
	{
		if(rule.ruleId == ruleId)
		{
			return &rule;
		}
	}

	return nullptr;
}

void RuleSet::RebuildRelationshipDefinitionIndex() const
{
	if(relationshipDefinitionIndexById == nullptr)
	{
		relationshipDefinitionIndexById = make_shared<unordered_map<string, RelationshipDefinitionLocation>>();
	}

	if(relationshipDefinitionIndexByScope == nullptr)
	{
		relationshipDefinitionIndexByScope = make_shared<unordered_map<string, RelationshipDefinitionLocation>>();
	}

	relationshipDefinitionIndexById->clear();
	relationshipDefinitionIndexByScope->clear();

	relationshipDefinitionIndexEntityCount = entities != nullptr ? entities->size() : 0;
	relationshipDefinitionIndexRelationshipCount = RelationshipDefinitionCount(*this);

	if(entities == nullptr)
	{
		return;
	}

	for(size_t entityIndex = 0; entityIndex < entities->size(); ++entityIndex)
	{
		const shared_ptr<Entity>& entity = entities->at(entityIndex);

		if(entity == nullptr)
		{
			continue;
		}

		for(size_t relationshipIndex = 0; relationshipIndex < entity->relationships.size(); ++relationshipIndex)
		{
			const shared_ptr<Relationship>& relationship = entity->relationships[relationshipIndex];

			if(relationship == nullptr)
			{
				continue;
			}

			const RelationshipDefinitionLocation location{entityIndex, relationshipIndex};

			if(!relationship->id.empty() && relationshipDefinitionIndexById->find(relationship->id) == relationshipDefinitionIndexById->end())
			{
				(*relationshipDefinitionIndexById)[relationship->id] = location;
			}

			const string normalizedId = SessionModelExpressionParserSupport::NormalizeLookup(relationship->id);

			if(!normalizedId.empty() && relationshipDefinitionIndexByScope->find(normalizedId) == relationshipDefinitionIndexByScope->end())
			{
				(*relationshipDefinitionIndexByScope)[normalizedId] = location;
			}

			const string normalizedDescription = SessionModelExpressionParserSupport::NormalizeLookup(relationship->description);

			if(!normalizedDescription.empty() && relationshipDefinitionIndexByScope->find(normalizedDescription) == relationshipDefinitionIndexByScope->end())
			{
				(*relationshipDefinitionIndexByScope)[normalizedDescription] = location;
			}
		}
	}
}

shared_ptr<Relationship> RuleSet::GetRelationshipDefinitionById(const string& relationshipId) const
{
	if(relationshipId.empty() || entities == nullptr)
	{
		return nullptr;
	}

	if(relationshipDefinitionIndexById == nullptr
		|| relationshipDefinitionIndexEntityCount != entities->size()
		|| relationshipDefinitionIndexRelationshipCount == static_cast<size_t>(-1))
	{
		RebuildRelationshipDefinitionIndex();
	}

	auto it = relationshipDefinitionIndexById != nullptr
		? relationshipDefinitionIndexById->find(relationshipId)
		: unordered_map<string, RelationshipDefinitionLocation>::const_iterator();

	if(relationshipDefinitionIndexById != nullptr && it != relationshipDefinitionIndexById->end())
	{
		shared_ptr<Relationship> relationship = ResolveRelationshipDefinitionLocation(*this, it->second);

		if(relationship != nullptr && relationship->id == relationshipId)
		{
			return relationship;
		}
	}

	RebuildRelationshipDefinitionIndex();

	it = relationshipDefinitionIndexById != nullptr
		? relationshipDefinitionIndexById->find(relationshipId)
		: unordered_map<string, RelationshipDefinitionLocation>::const_iterator();

	if(relationshipDefinitionIndexById != nullptr && it != relationshipDefinitionIndexById->end())
	{
		shared_ptr<Relationship> relationship = ResolveRelationshipDefinitionLocation(*this, it->second);

		if(relationship != nullptr && relationship->id == relationshipId)
		{
			return relationship;
		}
	}

	return nullptr;
}

shared_ptr<Relationship> RuleSet::GetRelationshipDefinitionByScope(const string& scope) const
{
	const string normalizedScope = SessionModelExpressionParserSupport::NormalizeLookup(scope);

	if(normalizedScope.empty() || entities == nullptr)
	{
		return nullptr;
	}

	if(relationshipDefinitionIndexByScope == nullptr
		|| relationshipDefinitionIndexEntityCount != entities->size()
		|| relationshipDefinitionIndexRelationshipCount == static_cast<size_t>(-1))
	{
		RebuildRelationshipDefinitionIndex();
	}

	auto it = relationshipDefinitionIndexByScope != nullptr
		? relationshipDefinitionIndexByScope->find(normalizedScope)
		: unordered_map<string, RelationshipDefinitionLocation>::const_iterator();

	if(relationshipDefinitionIndexByScope != nullptr && it != relationshipDefinitionIndexByScope->end())
	{
		shared_ptr<Relationship> relationship = ResolveRelationshipDefinitionLocation(*this, it->second);

		if(RelationshipDefinitionMatchesScope(relationship, normalizedScope))
		{
			return relationship;
		}
	}

	RebuildRelationshipDefinitionIndex();

	it = relationshipDefinitionIndexByScope != nullptr
		? relationshipDefinitionIndexByScope->find(normalizedScope)
		: unordered_map<string, RelationshipDefinitionLocation>::const_iterator();

	if(relationshipDefinitionIndexByScope != nullptr && it != relationshipDefinitionIndexByScope->end())
	{
		shared_ptr<Relationship> relationship = ResolveRelationshipDefinitionLocation(*this, it->second);

		if(RelationshipDefinitionMatchesScope(relationship, normalizedScope))
		{
			return relationship;
		}
	}

	return nullptr;
}

const vector<string>* RuleSet::FindActiveSessionModelRulesReferencingAttribute(const string& entityId, const string& attributeId) const
{
	if(activeSessionModelRuleIdsByReferencedAttribute == nullptr)
	{
		return nullptr;
	}

	auto it = activeSessionModelRuleIdsByReferencedAttribute->find(AttributeDependencyKey(entityId, attributeId));

	return it == activeSessionModelRuleIdsByReferencedAttribute->end() ? nullptr : &it->second;
}

vector<string> RuleSet::GetActiveSessionModelRulesReferencingAttribute(const string& entityId, const string& attributeId) const
{
	const vector<string>* rules = FindActiveSessionModelRulesReferencingAttribute(entityId, attributeId);

	return rules == nullptr ? vector<string>() : *rules;
}

const vector<string>* RuleSet::FindActiveSessionModelRulesReferencingRelationship(const string& relationshipId) const
{
	if(activeSessionModelRuleIdsByReferencedRelationship == nullptr)
	{
		return nullptr;
	}

	auto it = activeSessionModelRuleIdsByReferencedRelationship->find(relationshipId);

	return it == activeSessionModelRuleIdsByReferencedRelationship->end() ? nullptr : &it->second;
}

vector<string> RuleSet::GetActiveSessionModelRulesReferencingRelationship(const string& relationshipId) const
{
	const vector<string>* rules = FindActiveSessionModelRulesReferencingRelationship(relationshipId);

	return rules == nullptr ? vector<string>() : *rules;
}

const vector<string>* RuleSet::FindActiveSessionModelRulesReferencingFunction(const string& functionName) const
{
	if(activeSessionModelRuleIdsByReferencedFunction == nullptr)
	{
		return nullptr;
	}

	auto it = activeSessionModelRuleIdsByReferencedFunction->find(functionName);

	return it == activeSessionModelRuleIdsByReferencedFunction->end() ? nullptr : &it->second;
}

vector<string> RuleSet::GetActiveSessionModelRulesReferencingFunction(const string& functionName) const
{
	const vector<string>* rules = FindActiveSessionModelRulesReferencingFunction(functionName);

	return rules == nullptr ? vector<string>() : *rules;
}

vector<string> RuleSet::GetActiveSessionModelRulesConcludingAttribute(const string& entityId, const string& attributeId) const
{
	if(activeSessionModelRuleIdsByConcludedAttribute == nullptr)
	{
		return {};
	}

	auto it = activeSessionModelRuleIdsByConcludedAttribute->find(AttributeDependencyKey(entityId, attributeId));

	return it == activeSessionModelRuleIdsByConcludedAttribute->end() ? vector<string>() : it->second;
}

void RuleSet::AddScreenLocation(const ScreenLocation& location)
{
	auto& locations = *screenLocations;

	for(auto it = locations.begin(); it != locations.end(); ++it)
	{
		if(it->entityId == location.entityId && it->instanceId == location.instanceId && it->screenId == location.screenId)
		{
			locations.erase(it + 1, locations.end());

			return;
		}
	}

	locations.push_back(location);

	return;
}

void RuleSet::RemoveLastScreenLocation()
{
	auto& locations = *screenLocations;

	if(!locations.empty())
	{
		locations.pop_back();
	}

	return;
}

ScreenLocation RuleSet::GetPreviousScreenLocation()
{
	auto& locations = *screenLocations;

	ScreenLocation result;

	if(locations.size() >= 2)
	{
		result = locations[locations.size() - 2];
		
		RemoveLastScreenLocation();
	}
	else if(locations.size() == 1)
	{
		result = locations[0];
	}
	
	return result;
}

ScreenLocation RuleSet::GetLatestScreenLocation(string screenId)
{
	auto& locations = *screenLocations;

	ScreenLocation result = ScreenLocation();
	
	for(auto it = locations.rbegin(); it != locations.rend(); ++it)
	{
		if(it->screenId == screenId)
		{
			result.entityId = it->entityId;
			result.instanceId = it->instanceId;
			result.screenId = it->screenId;

			locations.erase(locations.begin() + (locations.size() - (it - locations.rbegin())), locations.end());
		
			break;
		}
	}

	return result;
}
