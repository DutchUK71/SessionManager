#include "DataManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
	struct EntityVectorLookupIndex
	{
		std::unordered_map<string, size_t> entityIndexById;
		size_t entityIndexSize = (std::numeric_limits<size_t>::max)();
	};

	struct SubstitutionScope
	{
		shared_ptr<Instance> contextInstance;
		vector<vector<shared_ptr<Instance>>> instanceLevels;
		unordered_map<string, string> entityNamesById;
	};

	std::mutex g_entityVectorLookupIndexesMutex;
	std::unordered_map<const void*, EntityVectorLookupIndex> g_entityVectorLookupIndexes;

	string LowercaseCopy(string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});

		return value;
	}

	string TrimCopy(const string& value)
	{
		size_t start = 0;
		while(start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
		{
			++start;
		}

		size_t end = value.size();
		while(end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
		{
			--end;
		}

		return value.substr(start, end - start);
	}

	bool TryParseFiniteDouble(const string& value, double& result)
	{
		const string trimmed = TrimCopy(value);
		if(trimmed.empty())
		{
			return false;
		}

		try
		{
			size_t parsedCharacters = 0;
			result = std::stod(trimmed, &parsedCharacters);
			return parsedCharacters == trimmed.size() && std::isfinite(result);
		}
		catch(const std::exception&)
		{
			return false;
		}
	}

	string JsonScalarToString(const json& value)
	{
		if(value.is_string())
		{
			return value.get<string>();
		}

		if(value.is_boolean())
		{
			return value.get<bool>() ? "true" : "false";
		}

		if(value.is_number())
		{
			return value.dump();
		}

		return "";
	}

	string ReadJsonString(const json& object, initializer_list<const char*> keys)
	{
		if(!object.is_object())
		{
			return "";
		}

		for(const char* key : keys)
		{
			if(object.contains(key))
			{
				string value = TrimCopy(JsonScalarToString(object.at(key)));
				if(!value.empty())
				{
					return value;
				}
			}
		}

		return "";
	}

	string NormalizeTaskListMatchKey(const string& value)
	{
		return LowercaseCopy(TrimCopy(value));
	}

	string NormalizeTaskListTagColor(const string& value)
	{
		const string normalized = NormalizeTaskListMatchKey(value);

		if(normalized == "grey" || normalized == "gray")
		{
			return "grey";
		}

		if(normalized == "green")
		{
			return "green";
		}

		if(normalized == "turquoise" || normalized == "teal" || normalized == "cyan")
		{
			return "turquoise";
		}

		if(normalized == "light-blue" || normalized == "lightblue" || normalized == "light blue")
		{
			return "light-blue";
		}

		if(normalized == "purple" ||
			normalized == "pink" ||
			normalized == "red" ||
			normalized == "orange" ||
			normalized == "yellow" ||
			normalized == "blue")
		{
			return normalized;
		}

		return "blue";
	}

	string NormalizeSummaryTechnicalName(const string& value)
	{
		string normalized;
		bool lastWasSpace = true;

		for(char ch : LowercaseCopy(value))
		{
			const unsigned char uch = static_cast<unsigned char>(ch);
			if(std::isalnum(uch))
			{
				normalized.push_back(static_cast<char>(uch));
				lastWasSpace = false;
			}
			else if(!lastWasSpace)
			{
				normalized.push_back(' ');
				lastWasSpace = true;
			}
		}

		while(!normalized.empty() && normalized.back() == ' ')
		{
			normalized.pop_back();
		}

		if(normalized.rfind("the ", 0) == 0)
		{
			normalized = normalized.substr(4);
		}

		return normalized;
	}

	bool EndsWithText(const string& value, const string& suffix)
	{
		return value.size() >= suffix.size()
			&& value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

	bool IsSummaryTechnicalRowIdentifier(const shared_ptr<Attribute>& attribute)
	{
		if(attribute == nullptr)
		{
			return false;
		}

		for(const string& candidate : {attribute->GetAttributeText(), attribute->GetCoreName(), attribute->GetID()})
		{
			const string normalized = NormalizeSummaryTechnicalName(candidate);

			if(normalized == "row id"
				|| normalized == "row identifier"
				|| normalized == "rowid"
				|| normalized == "my row id"
				|| EndsWithText(normalized, " row id")
				|| EndsWithText(normalized, " row identifier"))
			{
				return true;
			}
		}

		return false;
	}

	bool AttributeMatchesVisibility(const shared_ptr<Attribute>& attribute, const ScreenFile::VisibilityPredicate& visibility)
	{
		const string op = LowercaseCopy(visibility.operatorName);

		if(attribute == nullptr)
		{
			return false;
		}

		if(op == "isknown" || op == "is_known" || op == "is known")
		{
			return attribute->IsKnown()
				&& (attribute->GetType() != Attribute::BOOLEAN || attribute->GetStringValue() == "true");
		}

		if(op == "isunknown" || op == "is_unknown" || op == "is unknown")
		{
			return attribute->IsUnknown();
		}

		if(!attribute->IsCertain())
		{
			return false;
		}

		const string value = attribute->GetStringValue();

		if(op == "istrue")
		{
			return value == "true";
		}

		if(op == "isfalse")
		{
			return value == "false";
		}

		if(op == "equals" || op == "equal")
		{
			return value == visibility.value;
		}

		if(op == "notequals" || op == "not_equals" || op == "not equal")
		{
			return value != visibility.value;
		}

		return false;
	}

	bool IsCollectListControlType(const string& controlType)
	{
		return controlType == "collect"
			|| controlType == "collectview"
			|| controlType == "collect-view"
			|| controlType == "collect-file"
			|| controlType == "entity-instance-list";
	}

	shared_ptr<Instance> FindInstanceByIdentity(
		const shared_ptr<vector<shared_ptr<Entity>>>& entities,
		const string& entityId,
		const string& instanceId)
	{
		if(entities == nullptr || entityId.empty() || instanceId.empty())
		{
			return nullptr;
		}

		for(const shared_ptr<Entity>& entity : *entities)
		{
			if(entity == nullptr || entity->entityid != entityId)
			{
				continue;
			}

			for(const shared_ptr<Instance>& instance : entity->instances)
			{
				if(instance != nullptr && instance->instanceId == instanceId)
				{
					return instance;
				}
			}
		}

		return nullptr;
	}

	shared_ptr<Relationship> FindRelationshipByIdentity(
		const shared_ptr<vector<shared_ptr<Entity>>>& entities,
		const string& relationshipId)
	{
		if(entities == nullptr || relationshipId.empty())
		{
			return nullptr;
		}

		for(const shared_ptr<Entity>& entity : *entities)
		{
			if(entity == nullptr)
			{
				continue;
			}

			for(const shared_ptr<Relationship>& relationship : entity->relationships)
			{
				if(relationship != nullptr && relationship->id == relationshipId)
				{
					return relationship;
				}
			}
		}

		return nullptr;
	}

	string NormalizeSubstitutionQualifier(const string& value)
	{
		string output;
		output.reserve(value.size());
		for(const unsigned char character : value)
		{
			if(std::isalnum(character))
			{
				output.push_back(static_cast<char>(std::tolower(character)));
			}
		}
		return output;
	}

	string NormalizeRelationshipCardinality(const string& value)
	{
		return NormalizeSubstitutionQualifier(value);
	}

	bool IsParentRelationshipLink(
		const shared_ptr<Instance>& current,
		const InstanceRelationshipLink& link,
		const shared_ptr<Relationship>& relationship,
		string& parentEntityId,
		string& parentInstanceId)
	{
		if(current == nullptr || relationship == nullptr || link.relationshipId != relationship->id)
		{
			return false;
		}

		const string cardinality = NormalizeRelationshipCardinality(relationship->type);
		if((cardinality == "1m" || cardinality == "onetomany" || cardinality == "11" || cardinality == "onetoone") &&
		   current->entityId == relationship->targetEntity &&
		   !link.outgoing &&
		   link.targetInstanceId == current->instanceId)
		{
			parentEntityId = relationship->sourceEntity;
			parentInstanceId = link.sourceInstanceId;
			return true;
		}

		if((cardinality == "m1" || cardinality == "manytoone") &&
		   current->entityId == relationship->sourceEntity &&
		   link.outgoing &&
		   link.sourceInstanceId == current->instanceId)
		{
			parentEntityId = relationship->targetEntity;
			parentInstanceId = link.targetInstanceId;
			return true;
		}

		return false;
	}

	SubstitutionScope BuildSubstitutionScope(
		const shared_ptr<vector<shared_ptr<Entity>>>& entities,
		const shared_ptr<Instance>& contextInstance)
	{
		SubstitutionScope scope;
		scope.contextInstance = contextInstance;
		if(contextInstance == nullptr)
		{
			return scope;
		}

		unordered_map<string, shared_ptr<Relationship>> relationshipsById;
		if(entities != nullptr)
		{
			for(const shared_ptr<Entity>& entity : *entities)
			{
				if(entity == nullptr)
				{
					continue;
				}

				scope.entityNamesById[entity->entityid] = entity->description;
				for(const shared_ptr<Relationship>& relationship : entity->relationships)
				{
					if(relationship != nullptr && !relationship->id.empty())
					{
						relationshipsById.emplace(relationship->id, relationship);
					}
				}
			}
		}

		vector<pair<shared_ptr<Instance>, size_t>> pending = {{contextInstance, 0}};
		unordered_set<string> visited;
		for(size_t index = 0; index < pending.size(); ++index)
		{
			const shared_ptr<Instance> current = pending[index].first;
			const size_t depth = pending[index].second;
			if(current == nullptr)
			{
				continue;
			}

			const string key = current->entityId + "\x1f" + current->instanceId;
			if(!visited.insert(key).second)
			{
				continue;
			}

			if(scope.instanceLevels.size() <= depth)
			{
				scope.instanceLevels.resize(depth + 1);
			}
			scope.instanceLevels[depth].push_back(current);

			for(const InstanceRelationshipLink& link : current->relationshipLinks)
			{
				auto relationshipIt = relationshipsById.find(link.relationshipId);
				if(relationshipIt == relationshipsById.end())
				{
					continue;
				}

				string parentEntityId;
				string parentInstanceId;
				if(!IsParentRelationshipLink(current, link, relationshipIt->second, parentEntityId, parentInstanceId))
				{
					continue;
				}

				shared_ptr<Instance> parent = link.linkedInstance.lock();
				if(parent == nullptr || parent->entityId != parentEntityId || parent->instanceId != parentInstanceId)
				{
					parent = FindInstanceByIdentity(entities, parentEntityId, parentInstanceId);
				}
				if(parent != nullptr)
				{
					pending.push_back({parent, depth + 1});
				}
			}
		}

		return scope;
	}

	shared_ptr<Instance> ResolveScopedInstance(const SubstitutionScope& scope, const string& entityId)
	{
		if(scope.contextInstance == nullptr || entityId.empty())
		{
			return nullptr;
		}

		for(const vector<shared_ptr<Instance>>& level : scope.instanceLevels)
		{
			shared_ptr<Instance> match = nullptr;
			for(const shared_ptr<Instance>& candidate : level)
			{
				if(candidate == nullptr || candidate->entityId != entityId)
				{
					continue;
				}

				if(match != nullptr && match->instanceId != candidate->instanceId)
				{
					return nullptr;
				}

				match = candidate;
			}

			if(match != nullptr)
			{
				return match;
			}
		}

		return nullptr;
	}

	bool RadioOptionIsVisible(const SubstitutionScope& scope, const ScreenFile::Options& option)
	{
		string conditionOperator = option.conditionOperator.has_value()
			? LowercaseCopy(TrimCopy(option.conditionOperator.value()))
			: string();
		if(conditionOperator.empty())
		{
			if(!option.conditionValue.has_value())
			{
				return true;
			}
			conditionOperator = option.conditionValue.value() ? "true" : "false";
		}

		if(conditionOperator == "never" || conditionOperator == "nevershow" || conditionOperator == "never show")
		{
			return false;
		}

		if(!option.conditionEntityId.has_value() || !option.conditionAttributeId.has_value())
		{
			return false;
		}

		shared_ptr<Instance> conditionInstance = ResolveScopedInstance(scope, option.conditionEntityId.value());
		shared_ptr<Attribute> conditionAttribute = conditionInstance != nullptr
			? conditionInstance->GetAttributeByID(option.conditionAttributeId.value())
			: nullptr;
		if(conditionAttribute == nullptr || !conditionAttribute->IsCertain())
		{
			return false;
		}

		if(conditionOperator == "lower" || conditionOperator == "higher")
		{
			double optionValue = 0.0;
			double attributeValue = 0.0;
			if(!TryParseFiniteDouble(option.value, optionValue) ||
				!TryParseFiniteDouble(conditionAttribute->GetStringValue(), attributeValue))
			{
				return false;
			}

			return conditionOperator == "lower"
				? optionValue < attributeValue
				: optionValue > attributeValue;
		}

		if(conditionOperator == "equals" || conditionOperator == "textequals" || conditionOperator == "text equals")
		{
			return option.conditionComparisonValue.has_value() &&
				conditionAttribute->GetStringValue() == option.conditionComparisonValue.value();
		}

		if(conditionOperator == "true" || conditionOperator == "istrue" ||
			conditionOperator == "false" || conditionOperator == "isfalse")
		{
			const string actual = LowercaseCopy(TrimCopy(conditionAttribute->GetStringValue()));
			const bool expectsTrue = conditionOperator == "true" || conditionOperator == "istrue";
			return actual == (expectsTrue ? "true" : "false");
		}

		return false;
	}

	shared_ptr<Instance> ResolveRelationshipSourceInstance(
		const shared_ptr<vector<shared_ptr<Entity>>>& entities,
		const shared_ptr<Instance>& contextInstance,
		const string& sourceEntityId)
	{
		if(contextInstance == nullptr || sourceEntityId.empty())
		{
			return nullptr;
		}

		vector<shared_ptr<Instance>> pending = {contextInstance};
		unordered_set<string> visited;

		for(size_t index = 0; index < pending.size(); ++index)
		{
			const shared_ptr<Instance> current = pending[index];
			if(current == nullptr)
			{
				continue;
			}

			const string key = current->entityId + "\x1f" + current->instanceId;
			if(!visited.insert(key).second)
			{
				continue;
			}

			if(current->entityId == sourceEntityId)
			{
				return current;
			}

			for(const InstanceRelationshipLink& link : current->relationshipLinks)
			{
				if(link.outgoing || link.targetInstanceId != current->instanceId)
				{
					continue;
				}

				shared_ptr<Instance> parent = link.linkedInstance.lock();
				if(parent == nullptr || parent->instanceId != link.sourceInstanceId)
				{
					parent = FindInstanceByIdentity(entities, link.sourceEntityId, link.sourceInstanceId);
				}

				if(parent != nullptr)
				{
					pending.push_back(parent);
				}
			}
		}

		return nullptr;
	}

	vector<shared_ptr<Instance>> ResolveCollectInstances(
		const shared_ptr<vector<shared_ptr<Entity>>>& entities,
		const ScreenFile::Element& element,
		const shared_ptr<Instance>& contextInstance)
	{
		vector<shared_ptr<Instance>> result;
		if(contextInstance == nullptr || !element.collectEntity.has_value())
		{
			return result;
		}

		const string& targetEntityId = element.collectEntity.value();
		if(element.collectRelationshipId.has_value() && !element.collectRelationshipId->empty())
		{
			shared_ptr<Relationship> relationship = FindRelationshipByIdentity(entities, element.collectRelationshipId.value());
			if(relationship == nullptr || relationship->targetEntity != targetEntityId)
			{
				return result;
			}

			shared_ptr<Instance> sourceInstance = ResolveRelationshipSourceInstance(entities, contextInstance, relationship->sourceEntity);
			if(sourceInstance == nullptr)
			{
				return result;
			}

			for(const InstanceRelationshipLink& link : sourceInstance->relationshipLinks)
			{
				if(!link.outgoing ||
				   link.relationshipId != relationship->id ||
				   link.sourceInstanceId != sourceInstance->instanceId ||
				   link.targetEntityId != targetEntityId)
				{
					continue;
				}

				shared_ptr<Instance> target = link.linkedInstance.lock();
				if(target == nullptr || target->instanceId != link.targetInstanceId)
				{
					target = FindInstanceByIdentity(entities, link.targetEntityId, link.targetInstanceId);
				}

				if(target != nullptr)
				{
					result.push_back(target);
				}
			}

			return result;
		}

		for(const shared_ptr<Instance>& linkedInstance : contextInstance->linkedInstances)
		{
			if(linkedInstance != nullptr && linkedInstance->entityId == targetEntityId)
			{
				result.push_back(linkedInstance);
			}
		}

		return result;
	}

	bool IsCollectMembershipSelected(
		const ScreenFile::Element& element,
		const shared_ptr<Instance>& contextInstance,
		const shared_ptr<Instance>& targetInstance)
	{
		if(contextInstance == nullptr || targetInstance == nullptr ||
		   !element.collectData.has_value() || !element.collectData->select.value_or(false) ||
		   !element.collectData->membershipRelationshipId.has_value())
		{
			return false;
		}

		const string& relationshipId = element.collectData->membershipRelationshipId.value();
		for(const InstanceRelationshipLink& link : contextInstance->relationshipLinks)
		{
			if(link.outgoing &&
			   link.relationshipId == relationshipId &&
			   link.sourceEntityId == contextInstance->entityId &&
			   link.sourceInstanceId == contextInstance->instanceId &&
			   link.targetEntityId == targetInstance->entityId &&
			   link.targetInstanceId == targetInstance->instanceId)
			{
				return true;
			}
		}

		return false;
	}

	const RuleSet::UploadInfo* FindCollectFileInfo(
		const shared_ptr<RuleSet>& ruleSet,
		const ScreenFile::Element& element,
		const shared_ptr<Instance>& instance)
	{
		if(ruleSet == nullptr || ruleSet->uploads == nullptr || instance == nullptr || !element.id.has_value())
		{
			return nullptr;
		}

		for(const RuleSet::UploadInfo& upload : *ruleSet->uploads)
		{
			if(upload.instanceId == instance->instanceId && upload.uploadControlId == element.id.value())
			{
				return &upload;
			}
		}

		return nullptr;
	}

	string GetMappedCollectFileValue(
		const shared_ptr<Instance>& instance,
		const optional<ScreenFile::CollectFileAttribute>& mapping)
	{
		if(instance == nullptr || !mapping.has_value() || mapping->attributeId.empty())
		{
			return "";
		}

		shared_ptr<Attribute> attribute = instance->GetAttributeByID(mapping->attributeId);
		return attribute == nullptr || attribute->IsUnknown() ? "" : attribute->GetStringValue();
	}

	bool HasCollectFileColumnAttributeMapping(const ScreenFile::Column& column)
	{
		return !column.attributeId.empty() && column.attributeId.rfind("__file_", 0) != 0;
	}

	string GetCollectFileColumnAttributeValue(
		const shared_ptr<Instance>& instance,
		const ScreenFile::Column& column)
	{
		if(instance == nullptr || !HasCollectFileColumnAttributeMapping(column))
		{
			return "";
		}

		shared_ptr<Attribute> attribute = instance->GetAttributeByID(column.attributeId);
		return attribute == nullptr || attribute->IsUnknown() ? "" : attribute->GetStringValue();
	}

	string FormatCollectFileSize(unsigned long long bytes)
	{
		constexpr double bytesPerKilobyte = 1024.0;
		constexpr double bytesPerMegabyte = 1024.0 * 1024.0;
		ostringstream formatted;
		if(static_cast<double>(bytes) < bytesPerMegabyte)
		{
			formatted << static_cast<unsigned long long>(std::llround(static_cast<double>(bytes) / bytesPerKilobyte)) << " KB";
		}
		else
		{
			formatted << fixed << setprecision(2) << (static_cast<double>(bytes) / bytesPerMegabyte) << " MB";
		}
		return formatted.str();
	}

	ScreenFile::CollectInstance BuildCollectInstance(
		const shared_ptr<Instance>& instance,
		const vector<ScreenFile::Column>& columns,
		const ScreenFile::Element& element,
		const shared_ptr<Instance>& contextInstance,
		const shared_ptr<RuleSet>& ruleSet)
	{
		ScreenFile::CollectInstance row;
		if(instance == nullptr)
		{
			return row;
		}

		row.instanceId = instance->instanceId;
		row.selected = IsCollectMembershipSelected(element, contextInstance, instance);
		const RuleSet::UploadInfo* fileInfo = FindCollectFileInfo(ruleSet, element, instance);
		for(const ScreenFile::Column& column : columns)
		{
			if(element.collectFile.has_value() && !column.source.empty() && column.source != "Other")
			{
				ScreenFile::InstanceAttribute value;
				value.attributeId = column.attributeId;
				const bool hasColumnMapping = HasCollectFileColumnAttributeMapping(column);
				const string columnAttributeValue = GetCollectFileColumnAttributeValue(instance, column);
				if(column.source == "FileName")
				{
					value.value = hasColumnMapping
						? columnAttributeValue
						: GetMappedCollectFileValue(instance, element.collectFile->fileNameAttribute);
					if(!hasColumnMapping && value.value.empty() && fileInfo != nullptr) value.value = fileInfo->fileName;
				}
				else if(column.source == "Extension")
				{
					value.value = hasColumnMapping
						? columnAttributeValue
						: GetMappedCollectFileValue(instance, element.collectFile->extensionAttribute);
					if(!hasColumnMapping && value.value.empty() && fileInfo != nullptr) value.value = fileInfo->fileExtension;
				}
				else if(column.source == "Size")
				{
					string mappedSize = hasColumnMapping
						? columnAttributeValue
						: GetMappedCollectFileValue(instance, element.collectFile->sizeAttribute);
					unsigned long long bytes = 0;
					if(!mappedSize.empty())
					{
						try
						{
							bytes = std::stoull(mappedSize);
						}
						catch(...)
						{
							bytes = 0;
						}
					}
					else if(!hasColumnMapping && fileInfo != nullptr)
					{
						bytes = fileInfo->fileSizeBytes;
					}
					value.value = FormatCollectFileSize(bytes);
				}
				row.attributes.push_back(value);
				continue;
			}

			shared_ptr<Attribute> attribute = instance->GetAttributeByID(column.attributeId);
			if(attribute == nullptr)
			{
				continue;
			}

			ScreenFile::InstanceAttribute value;
			value.attributeId = attribute->GetID();
			value.value = attribute->IsUnknown() ? "" : attribute->GetStringValue();
			row.attributes.push_back(value);
		}

		return row;
	}

	bool IsPlaceholderNameCharacter(unsigned char character)
	{
		return std::isalnum(character) || character == '_' || character == '-';
	}

	shared_ptr<Attribute> FindAttributeByCoreName(const shared_ptr<Instance>& instance, const string& coreName)
	{
		if(instance == nullptr || coreName.empty())
		{
			return nullptr;
		}

		const string normalizedCoreName = LowercaseCopy(coreName);

		for(const shared_ptr<Attribute>& attribute : instance->attributes)
		{
			if(attribute != nullptr && LowercaseCopy(attribute->GetCoreName()) == normalizedCoreName)
			{
				return attribute;
			}
		}

		return nullptr;
	}

	bool InstanceMatchesSubstitutionQualifier(
		const SubstitutionScope& scope,
		const shared_ptr<Instance>& instance,
		const string& qualifier)
	{
		if(instance == nullptr || qualifier.empty())
		{
			return false;
		}

		const string normalizedQualifier = NormalizeSubstitutionQualifier(qualifier);
		if(normalizedQualifier.empty())
		{
			return false;
		}

		if(NormalizeSubstitutionQualifier(instance->entityId) == normalizedQualifier)
		{
			return true;
		}

		auto entityNameIt = scope.entityNamesById.find(instance->entityId);
		return entityNameIt != scope.entityNamesById.end() &&
			NormalizeSubstitutionQualifier(entityNameIt->second) == normalizedQualifier;
	}

	shared_ptr<Attribute> ResolveSubstitutionAttribute(
		const SubstitutionScope& scope,
		const string& entityQualifier,
		const string& coreName)
	{
		if(coreName.empty())
		{
			return nullptr;
		}

		for(const vector<shared_ptr<Instance>>& level : scope.instanceLevels)
		{
			vector<shared_ptr<Instance>> candidates;
			for(const shared_ptr<Instance>& instance : level)
			{
				if(entityQualifier.empty() || InstanceMatchesSubstitutionQualifier(scope, instance, entityQualifier))
				{
					candidates.push_back(instance);
				}
			}

			if(!entityQualifier.empty())
			{
				if(candidates.empty())
				{
					continue;
				}
				if(candidates.size() != 1)
				{
					return nullptr;
				}

				return FindAttributeByCoreName(candidates.front(), coreName);
			}

			shared_ptr<Attribute> match = nullptr;
			for(const shared_ptr<Instance>& candidate : candidates)
			{
				shared_ptr<Attribute> attribute = FindAttributeByCoreName(candidate, coreName);
				if(attribute == nullptr)
				{
					continue;
				}
				if(match != nullptr)
				{
					return nullptr;
				}
				match = attribute;
			}

			if(match != nullptr)
			{
				return match;
			}
		}

		return nullptr;
	}

	string SubstituteAttributeIdPlaceholders(const shared_ptr<Instance>& instance, const string& input)
	{
		string output;
		output.reserve(input.size());

		for(size_t index = 0; index < input.size();)
		{
			if(input[index] == '{')
			{
				const size_t start = index + 1;
				const size_t end = input.find('}', start);

				if(end != string::npos && end > start)
				{
					const string attributeId = input.substr(start, end - start);
					shared_ptr<Attribute> attribute = instance != nullptr ? instance->GetAttributeByID(attributeId) : nullptr;

					if(attribute != nullptr)
					{
						output += attribute->GetStringValue();
						index = end + 1;
						continue;
					}
				}
			}

			output.push_back(input[index]);
			++index;
		}

		return output;
	}

	string SubstituteCoreNamePlaceholders(const SubstitutionScope& scope, const string& input)
	{
		string output;
		output.reserve(input.size());

		for(size_t index = 0; index < input.size();)
		{
			if(input[index] == '#')
			{
				const size_t start = index + 1;
				size_t firstEnd = start;

				while(firstEnd < input.size() && IsPlaceholderNameCharacter(static_cast<unsigned char>(input[firstEnd])))
				{
					++firstEnd;
				}

				if(firstEnd > start)
				{
					string entityQualifier;
					string coreName = input.substr(start, firstEnd - start);
					size_t tokenEnd = firstEnd;
					if(firstEnd < input.size() && input[firstEnd] == '.')
					{
						const size_t qualifiedStart = firstEnd + 1;
						size_t qualifiedEnd = qualifiedStart;
						while(qualifiedEnd < input.size() && IsPlaceholderNameCharacter(static_cast<unsigned char>(input[qualifiedEnd])))
						{
							++qualifiedEnd;
						}
						if(qualifiedEnd > qualifiedStart)
						{
							entityQualifier = coreName;
							coreName = input.substr(qualifiedStart, qualifiedEnd - qualifiedStart);
							tokenEnd = qualifiedEnd;
						}
					}

					shared_ptr<Attribute> attribute = ResolveSubstitutionAttribute(scope, entityQualifier, coreName);

					if(attribute != nullptr)
					{
						if(!attribute->IsUnknown())
						{
							output += attribute->GetStringValue();
						}

						index = tokenEnd;
						continue;
					}
				}
			}

			output.push_back(input[index]);
			++index;
		}

		return output;
	}

	void RebuildEntityVectorLookupIndex(const shared_ptr<vector<shared_ptr<Entity>>>& entities, EntityVectorLookupIndex& index)
	{
		index.entityIndexById.clear();
		index.entityIndexById.reserve(entities != nullptr ? entities->size() : 0);

		if(entities != nullptr)
		{
			for(size_t position = 0; position < entities->size(); ++position)
			{
				const shared_ptr<Entity>& entity = (*entities)[position];

				if(entity != nullptr && !entity->entityid.empty())
				{
					index.entityIndexById[entity->entityid] = position;
				}
			}

			index.entityIndexSize = entities->size();
		}
		else
		{
			index.entityIndexSize = 0;
		}
	}

	shared_ptr<Entity> FindEntityByIdIndexed(const shared_ptr<vector<shared_ptr<Entity>>>& entities, const string& entityId)
	{
		if(entities == nullptr || entityId.empty())
		{
			return nullptr;
		}

		std::lock_guard<std::mutex> lock(g_entityVectorLookupIndexesMutex);
		EntityVectorLookupIndex& index = g_entityVectorLookupIndexes[entities.get()];

		if(index.entityIndexSize != entities->size())
		{
			RebuildEntityVectorLookupIndex(entities, index);
		}

		auto it = index.entityIndexById.find(entityId);

		if(it == index.entityIndexById.end())
		{
			RebuildEntityVectorLookupIndex(entities, index);
			it = index.entityIndexById.find(entityId);
		}

		if(it != index.entityIndexById.end() &&
		   it->second < entities->size() &&
		   (*entities)[it->second] != nullptr &&
		   (*entities)[it->second]->entityid == entityId)
		{
			return (*entities)[it->second];
		}

		RebuildEntityVectorLookupIndex(entities, index);
		it = index.entityIndexById.find(entityId);

		if(it != index.entityIndexById.end() &&
		   it->second < entities->size() &&
		   (*entities)[it->second] != nullptr &&
		   (*entities)[it->second]->entityid == entityId)
		{
			return (*entities)[it->second];
		}

		return nullptr;
	}

	shared_ptr<Instance> ResolveVisibilityInstance(const shared_ptr<vector<shared_ptr<Entity>>>& entities,
	                                               const shared_ptr<Instance>& screenInstance,
	                                               const ScreenFile::VisibilityPredicate& visibility)
	{
		if(screenInstance == nullptr)
		{
			return nullptr;
		}

		if(visibility.entityId.empty() || visibility.entityId == screenInstance->entityId)
		{
			return screenInstance;
		}

		if(entities == nullptr)
		{
			return nullptr;
		}

		shared_ptr<Entity> entity = FindEntityByIdIndexed(entities, visibility.entityId);

		if(entity != nullptr && !entity->instances.empty())
		{
			return entity->instances.front();
		}

		return nullptr;
	}
}

string SubstituteText(const SubstitutionScope& scope, const string& input)
{
	if(scope.contextInstance == nullptr)
	{
		return input;
	}

	string output = SubstituteAttributeIdPlaceholders(scope.contextInstance, input);
	output = SubstituteCoreNamePlaceholders(scope, output);

	return output;
}

void SubstituteValidationMessageTextField(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	if(scope.contextInstance != nullptr && element.validationMessage.has_value())
	{
		element.validationMessage = SubstituteText(scope, element.validationMessage.value());
	}
}

void SubstituteRuleErrorMessageTextFields(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	if(scope.contextInstance == nullptr)
	{
		return;
	}

	if(element.ruleErrors.has_value())
	{
		for(ScreenFile::CollectData::SelectionError& error : element.ruleErrors.value())
		{
			error.message = SubstituteText(scope, error.message);
		}
	}

	if(element.collectData.has_value() && element.collectData->selectionErrors.has_value())
	{
		for(ScreenFile::CollectData::SelectionError& error : element.collectData->selectionErrors.value())
		{
			error.message = SubstituteText(scope, error.message);
		}
	}
}

void AddElementValidationError(ScreenFile::Element& element, const string& fallbackMessage)
{
	if(!element.errors.has_value())
	{
		element.errors = make_optional<vector<ScreenFile::Error>>();
	}

	const string message = element.validationMessage.has_value() && !element.validationMessage->empty()
		? element.validationMessage.value()
		: fallbackMessage;
	const bool alreadyPresent = any_of(
		element.errors->begin(),
		element.errors->end(),
		[&message](const ScreenFile::Error& error) { return error.errorMessage == message; });
	if(!alreadyPresent)
	{
		element.errors->push_back(ScreenFile::Error{message});
	}
}

string Substitute(const SubstitutionScope& scope, shared_ptr<ScreenFile::Element> element)
{
	if(scope.contextInstance == nullptr || element == nullptr)
	{
		return "";
	}

	string input = "";
	const string controlType = LowercaseCopy(element->controlType);
	const bool substituteLabel =
		(controlType == "label" || controlType == "heading") &&
		element->labelText.has_value();
	const bool substituteQuestion = !substituteLabel && element->questionText.has_value();

	if(substituteLabel)
	{
		input = element->labelText.value();
	}
	else if(substituteQuestion)
	{
		input = element->questionText.value();
	}
	else
	{
		return "";
	}

	string output = SubstituteText(scope, input);

	if(substituteQuestion)
	{
		element->questionText = output;
		return element->questionText.value();
	}

	element->labelText = output;
	return element->labelText.value();
}

void SubstituteRenderingTextFields(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	if(scope.contextInstance == nullptr || !element.rendering.has_value() || !element.rendering->is_object())
	{
		return;
	}

	for(const string& key : {
		"linkTarget",
		"linktarget",
		"linkUrl",
		"linkurl",
		"downloadTarget",
		"downloadtarget",
		"imageSource",
		"imagesource",
		"imageUrl",
		"imageurl",
		"imageAltText",
		"imagealttext",
		"imageCaption",
		"imagecaption",
		"src",
		"href",
		"url",
		"target"
	})
	{
		if(element.rendering->contains(key) && element.rendering->at(key).is_string())
		{
			(*element.rendering)[key] = SubstituteText(scope, element.rendering->at(key).get<string>());
		}
	}
}

void SubstituteNotificationBannerTextFields(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	if(scope.contextInstance == nullptr || LowercaseCopy(element.controlType) != "notification-banner")
	{
		return;
	}

	if(element.bannerHeader.has_value())
	{
		element.bannerHeader = SubstituteText(scope, element.bannerHeader.value());
	}

	if(element.bannerContent.has_value())
	{
		element.bannerContent = SubstituteText(scope, element.bannerContent.value());
	}
}

void SubstituteLinkTextFields(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	if(scope.contextInstance == nullptr || LowercaseCopy(element.controlType) != "link")
	{
		return;
	}

	if(element.labelText.has_value())
	{
		element.labelText = SubstituteText(scope, element.labelText.value());
	}

	if(element.questionText.has_value())
	{
		element.questionText = SubstituteText(scope, element.questionText.value());
	}
}

bool IsCollectionLabelControl(const string& controlType)
{
	const string normalized = LowercaseCopy(controlType);
	return normalized == "collect" ||
		normalized == "collect-view" ||
		normalized == "entity-instance-list";
}

void SubstituteCollectionLabelTextFields(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	if(scope.contextInstance == nullptr || !IsCollectionLabelControl(element.controlType))
	{
		return;
	}

	if(element.questionText.has_value())
	{
		element.questionText = SubstituteText(scope, element.questionText.value());
	}

	if(element.labelText.has_value())
	{
		element.labelText = SubstituteText(scope, element.labelText.value());
	}
}

void SubstituteOptionDisplayTextFields(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	if(scope.contextInstance == nullptr || !element.options.has_value())
	{
		return;
	}

	for(ScreenFile::Options& option : element.options.value())
	{
		option.caption = SubstituteText(scope, option.caption);
	}
}

void SubstituteOptionDisplayTextFieldsRecursive(const SubstitutionScope& scope, ScreenFile::Element& element)
{
	SubstituteOptionDisplayTextFields(scope, element);
	for(ScreenFile::Element& child : element.children)
	{
		SubstituteOptionDisplayTextFieldsRecursive(scope, child);
	}
}

void DataManager::SetDisplayElement(shared_ptr<vector<shared_ptr<Entity>>> entities, shared_ptr<ScreenFile::Screen> screen)
{
	if(entities == nullptr || screen == nullptr)
	{
		return;
	}

	shared_ptr<Instance> screenInstance = nullptr;

	screenInstance = GetEntityInstanceById(entities, screen->entityId, screen->instanceId.value_or(""));

	if(screenInstance == nullptr)
	{
		return;
	}

	const auto setElementDisplay = [&](const auto& self, ScreenFile::Element& element) -> void {
		if(element.displayRuleId.has_value())
		{
			element.displayKnown = true;
			for(const shared_ptr<Rule>& rule : screenInstance->rules)
			{
				if(rule->GetID() == element.displayRuleId)
				{
					element.display = rule->GetState();

					break;
				}
			}
		}
		else if(element.visibility.has_value())
		{
			shared_ptr<Instance> visibilityInstance = ResolveVisibilityInstance(entities, screenInstance, element.visibility.value());
			shared_ptr<Attribute> visibilityAttribute = visibilityInstance != nullptr
				? visibilityInstance->GetAttributeByID(element.visibility->attributeId)
				: nullptr;
			const string visibilityOperator = LowercaseCopy(element.visibility->operatorName);
			element.displayKnown = visibilityAttribute != nullptr
				&& (visibilityOperator == "isknown"
					|| visibilityOperator == "is_known"
					|| visibilityOperator == "is known"
					|| visibilityOperator == "isunknown"
					|| visibilityOperator == "is_unknown"
					|| visibilityOperator == "is unknown"
					|| visibilityAttribute->IsCertain());

			element.display = AttributeMatchesVisibility(visibilityAttribute, element.visibility.value());
		}
		else
		{
			element.display = true;
			element.displayKnown = true;
		}

		for(ScreenFile::Element& child : element.children)
		{
			self(self, child);
		}
	};

	for(ScreenFile::Element& element : screen->elements)
	{
		setElementDisplay(setElementDisplay, element);
	}
}

shared_ptr<Instance> DataManager::GetEntityInstanceById(shared_ptr<vector<shared_ptr<Entity>>> entities, const string& entityId, const string& instanceId)
{
	shared_ptr<Entity> entity = FindEntityByIdIndexed(entities, entityId);

	return entity != nullptr ? entity->GetInstanceById(instanceId) : nullptr;
}

shared_ptr<ScreenFile::Screen> DataManager::HandleData(shared_ptr<vector<shared_ptr<Entity>>> entities, 
	                                                   shared_ptr<ScreenData::DataRoot> data, 
	                                                   shared_ptr<ScreenFile::Screen> screen,
	                                                   shared_ptr<ScreenFile::ScreenData> screens,
	                                                   string directTo, 
	                                                   shared_ptr<RuleSet>& ruleSet)
{
	if(data == nullptr || screen == nullptr || screens == nullptr || ruleSet == nullptr)
	{
		return screen;
	}

	shared_ptr<Instance> instance = GetEntityInstanceById(entities, data->entityId, data->instanceId);

	if(instance == nullptr)
	{
		return screen;
	}
	const SubstitutionScope substitutionScope = BuildSubstitutionScope(entities, instance);

	for(ScreenFile::Element& element : screen->elements)
	{
		element.errors = make_optional<vector<ScreenFile::Error>>();
	}

	bool result = true;

	for(ScreenFile::Element& element : screen->elements)
	{
		SubstituteOptionDisplayTextFieldsRecursive(substitutionScope, element);
		SubstituteRenderingTextFields(substitutionScope, element);
		SubstituteNotificationBannerTextFields(substitutionScope, element);
		SubstituteLinkTextFields(substitutionScope, element);
		SubstituteCollectionLabelTextFields(substitutionScope, element);
		SubstituteValidationMessageTextField(substitutionScope, element);

		const string controlType = LowercaseCopy(element.controlType);
		if(controlType == "label" || controlType == "heading")
		{
			element.labelText = Substitute(substitutionScope, make_shared<ScreenFile::Element>(element));
		}
		else if(controlType == "question")
		{
			for(const shared_ptr<Attribute>& att : instance->attributes)
			{
				if(element.id == att->GetID())
				{
					bool found = false;

					for(const ScreenData::Data& d : data->data)
					{
						if(element.id == d.attributeId)
						{
							found = true;

							element.questionText = Substitute(substitutionScope, make_shared<ScreenFile::Element>(element));

							shared_ptr<ValueVariant> value = make_shared<ValueVariant>();

							if(d.value.empty())
							{
								att->MakeUnknown();

								element.value = nullopt;
							}
							else
							{
								if(att->screenId.empty())
								{
									att->screenId = screen->id;
								}

								element.value = att->GetStringValue();
							}

							if(element.required.has_value() && element.required.value())
							{
								if(att->IsUnknown())
								{
									cout << "Attribute " << att->GetAttributeText() << " is required but has no value." << endl;

									AddElementValidationError(element, "This field is required.");

									result = false;
								}
							}

							if(element.constraints.has_value())
							{
								for(const ScreenFile::Constraint& constraint : element.constraints.value())
								{
									if(constraint.maxCharacters.has_value() && d.value.length() > static_cast<size_t>(constraint.maxCharacters.value()))
									{
										cout << "Attribute " << att->GetAttributeText() << " exceeds maximum character limit of " << constraint.maxCharacters.value() << "." << endl;

										AddElementValidationError(element, "Exceeds maximum character limit of " + to_string(constraint.maxCharacters.value()) + ".");

										result = false;
									}
								}
							}

							break;
						}
					}

					if(!found)
					{
						att->MakeUnknown();

						if(element.required.has_value() && element.required.value())
						{
							cout << "Attribute " << att->GetAttributeText() << " is required but has no value." << endl;

							AddElementValidationError(element, "This field is required.");

							result = false;
						}
					}

					break;
				}
			}
		}
		else if(IsCollectListControlType(element.controlType))
		{
			if(!element.collectEntity.has_value() || !element.collectData.has_value() || !element.collectData->columns.has_value())
			{
				continue;
			}

			element.collectData->instances = vector<ScreenFile::CollectInstance>();
			element.collectData->selectedCount = 0;
			element.collectData->instanceCount = 0;
			for(const shared_ptr<Instance>& linkedInstance : ResolveCollectInstances(entities, element, instance))
			{
				element.collectData->instanceCount = element.collectData->instanceCount.value_or(0) + 1;
				ScreenFile::CollectInstance row = BuildCollectInstance(linkedInstance, element.collectData->columns.value(), element, instance, ruleSet);
				if(row.selected.value_or(false))
				{
					element.collectData->selectedCount = element.collectData->selectedCount.value_or(0) + 1;
				}
				element.collectData->instances->push_back(row);
			}
		}

	}

	if(result)
	{
		shared_ptr<ScreenFile::Screen> nextScreen;

		ScreenLocation redirect = ScreenLocation();

		if(!data->nextScreenId.empty() && directTo.empty())
		{
			nextScreen = GetNextScreen(screens, data->nextScreenId, data->nextEntityId);

			if(nextScreen == nullptr)
			{
				nextScreen = GetScreenByID(screens, data->nextScreenId);
			}

			if(!directTo.empty())
			{
				nextScreen->clearNextOnReturn = false;
			}

			nextScreen->instanceId = data->nextInstanceId;

			nextScreen = GetSessionData(entities, nextScreen, data->nextEntityId, data->nextInstanceId, ruleSet, screens.get());

			return nextScreen;
		}
		
		if(!directTo.empty())
		{
			redirect = ruleSet->GetLatestScreenLocation(directTo);

			if(redirect.screenId.empty())
			{
				nextScreen = GetScreenByID(screens, directTo);

				nextScreen->instanceId = data->instanceId;

				nextScreen = GetSessionData(entities, nextScreen, data->entityId, data->instanceId, ruleSet, screens.get());

				if(!data->nextScreenId.empty())
				{
					nextScreen->clearNextOnReturn = false;
				}

				return nextScreen;
			}
		}

		if(!redirect.screenId.empty())
		{
			nextScreen = GetScreen(entities, screens, redirect, ruleSet);
			
			if(!data->nextScreenId.empty())
			{
				nextScreen->clearNextOnReturn = false;
			}

			return nextScreen;
		}
		else
		{
			nextScreen = GetNextScreen(screens, screen->id, screen->entityId);

			if(nextScreen == nullptr)
			{
				nextScreen = GetScreenByID(screens, directTo);
			}

			nextScreen->instanceId = data->instanceId;

			nextScreen = GetSessionData(entities, nextScreen, data->entityId, data->instanceId, ruleSet, screens.get());

			if(!data->nextScreenId.empty())
			{
				nextScreen->clearNextOnReturn = false;
			}

			return nextScreen;
		}

		if(nextScreen == nullptr)
		{
			return screen;
		}
	}

	return screen;
}

shared_ptr<ScreenFile::Screen> DataManager::GetScreenByID(shared_ptr<ScreenFile::ScreenData> screens, const string& screenId)
{
	if(screens == nullptr)
	{
		return nullptr;
	}

	return screens->GetScreenByID(screenId);
}

shared_ptr<ScreenFile::Element> DataManager::GetElementById(shared_ptr<ScreenFile::Screen> screen, const string& elementId)
{
	if(screen == nullptr)
	{
		return nullptr;
	}

	for(ScreenFile::Element& element : screen->elements)
	{
		const auto findElement = [&](const auto& self, ScreenFile::Element& current) -> shared_ptr<ScreenFile::Element> {
			if(current.id.has_value() && current.id.value() == elementId)
			{
				return make_shared<ScreenFile::Element>(current);
			}

			for(ScreenFile::Element& child : current.children)
			{
				shared_ptr<ScreenFile::Element> found = self(self, child);

				if(found != nullptr)
				{
					return found;
				}
			}

			return nullptr;
		};

		shared_ptr<ScreenFile::Element> found = findElement(findElement, element);

		if(found != nullptr)
		{
			return found;
		}
	}

	return nullptr;
}

void DataManager::UpdateAttributeValue(shared_ptr<vector<shared_ptr<Entity>>> entities, 
	                                   const string& attributeId, 
	                                   const string& entityId,
									   const string& instanceId,
	                                   const string& strValue,
									   const string& screenId)
{
	if(entities == nullptr)
	{
		return;
	}

	shared_ptr<Instance> instance = GetEntityInstanceById(entities, entityId, instanceId);

	if(instance == nullptr)
	{
		return;
	}

	shared_ptr<Attribute> attribute = instance->GetAttributeByID(attributeId);

	if(attribute == nullptr)
	{
		return;
	}

	attribute->SetValueFromString(strValue);
	attribute->screenId = screenId;
}

shared_ptr<ScreenFile::Screen> DataManager::GetSessionData(shared_ptr<vector<shared_ptr<Entity>>> entities, 
	                                                       shared_ptr<ScreenFile::Screen> screen, 
	                                                       string entityId, 
	                                                       string instanceId,
														   shared_ptr<RuleSet>& ruleSet,
														   const ScreenFile::ScreenData* screens)
{
	shared_ptr<Instance> instance = GetEntityInstanceById(entities, entityId, instanceId);

	if(screen == nullptr || instance == nullptr || ruleSet == nullptr)
	{
		return screen;
	}
	const SubstitutionScope substitutionScope = BuildSubstitutionScope(entities, instance);

	if(screen->title.has_value())
	{
		screen->title = SubstituteText(substitutionScope, screen->title.value());
	}

	const auto populateSummaryTableRendering = [&](ScreenFile::Element& element) -> void {
		if(element.rendering.has_value() == false || element.rendering->is_object() == false)
		{
			return;
		}

		json rendering = element.rendering.value();
		if(rendering.contains("summaryTable") == false || rendering["summaryTable"].is_object() == false)
		{
			return;
		}

		json summaryTable = rendering["summaryTable"];
		if(summaryTable.contains("rows") == false || summaryTable["rows"].is_array() == false)
		{
			return;
		}

		for(json& row : summaryTable["rows"])
		{
			if(row.is_object() == false)
			{
				continue;
			}

			string value = "";
			const string attributeId = ReadJsonString(row, {"attributeId", "attributeid"});
			if(!attributeId.empty())
			{
				shared_ptr<Attribute> attribute = instance->GetAttributeByID(attributeId);
				if(attribute != nullptr && !attribute->IsUnknown())
				{
					value = attribute->GetStringValue();
				}
			}

			row["value"] = value;
		}

		rendering["summaryTable"] = summaryTable;
		element.rendering = rendering;
	};

	const auto populateTaskListRendering = [&](ScreenFile::Element& element) -> void {
		if(element.rendering.has_value() == false || element.rendering->is_object() == false)
		{
			return;
		}

		json rendering = element.rendering.value();
		if(rendering.contains("taskList") == false || rendering["taskList"].is_object() == false)
		{
			return;
		}

		json taskList = rendering["taskList"];
		if(taskList.contains("rows") == false || taskList["rows"].is_array() == false)
		{
			return;
		}

		const string defaultColor = NormalizeTaskListTagColor(ReadJsonString(taskList, {
			"defaultTagColor",
			"defaulttagcolor",
			"defaultColor",
			"defaultcolor",
			"defaultColour",
			"defaultcolour"
		}));
		unordered_map<string, string> tagColors;

		if(taskList.contains("tagColors") && taskList["tagColors"].is_array())
		{
			for(const json& mapping : taskList["tagColors"])
			{
				if(mapping.is_object() == false)
				{
					continue;
				}

				const string color = NormalizeTaskListTagColor(ReadJsonString(mapping, {"color", "colour"}));
				for(const string& key : {
					ReadJsonString(mapping, {"value"}),
					ReadJsonString(mapping, {"label", "displayText", "displaytext"})
				})
				{
					const string normalizedKey = NormalizeTaskListMatchKey(key);
					if(!normalizedKey.empty())
					{
						tagColors[normalizedKey] = color;
					}
				}
			}
		}

		for(json& row : taskList["rows"])
		{
			if(row.is_object() == false)
			{
				continue;
			}

			string status = "";
			const string statusAttributeId = ReadJsonString(row, {
				"statusAttributeId",
				"statusattributeid",
				"attributeId",
				"attributeid"
			});

			if(!statusAttributeId.empty())
			{
				shared_ptr<Attribute> attribute = instance->GetAttributeByID(statusAttributeId);
				if(attribute != nullptr && !attribute->IsUnknown())
				{
					status = attribute->GetStringValue();
				}
			}

			if(status.empty())
			{
				status = ReadJsonString(row, {"status", "statusText", "statustext", "tag", "state", "value"});
			}

			if(status.empty())
			{
				status = "Not started";
			}

			row["status"] = status;

			string tagColor = defaultColor;
			const string statusKey = NormalizeTaskListMatchKey(status);
			const auto colorIt = tagColors.find(statusKey);
			if(colorIt != tagColors.end())
			{
				tagColor = colorIt->second;
			}
			row["tagColor"] = tagColor;

			const string targetScreenId = ReadJsonString(row, {
				"targetScreenId",
				"targetscreenid",
				"screenId",
				"screenid"
			});

			if(targetScreenId.empty() || screens == nullptr)
			{
				continue;
			}

			string targetEntityId = entityId;
			string targetInstanceId = instanceId;
			for(const ScreenFile::Screen& candidateScreen : screens->screens)
			{
				if(candidateScreen.id == targetScreenId)
				{
					if(!candidateScreen.entityId.empty())
					{
						targetEntityId = candidateScreen.entityId;
					}
					break;
				}
			}

			if(targetEntityId != entityId)
			{
				targetInstanceId = GetFirstInstanceId(entities, targetEntityId);
			}

			if(!targetEntityId.empty() && !targetInstanceId.empty())
			{
				row["target"] = {
					{"screenId", targetScreenId},
					{"entityId", targetEntityId},
					{"instanceId", targetInstanceId},
					{"elementId", statusAttributeId}
				};
			}
		}

		rendering["taskList"] = taskList;
		element.rendering = rendering;
	};

	const auto populateElement = [&](const auto& self, ScreenFile::Element& element) -> void {
		const string boundAttributeId = element.boundAttributeId.value_or(element.id.value_or(""));
		const bool hasBoundAttribute = !boundAttributeId.empty();

		const string controlType = LowercaseCopy(element.controlType);
		if((controlType == "radio" || controlType == "dropdown") &&
			element.radioOptionVisibility.value_or(false) &&
			element.options.has_value())
		{
			vector<ScreenFile::Options> visibleOptions;
			visibleOptions.reserve(element.options->size());
			for(const ScreenFile::Options& option : element.options.value())
			{
				if(RadioOptionIsVisible(substitutionScope, option))
				{
					visibleOptions.push_back(option);
				}
			}
			element.options = std::move(visibleOptions);
		}
		if((controlType == "label" || controlType == "heading") && element.labelText.has_value())
		{
			element.labelText = Substitute(substitutionScope, make_shared<ScreenFile::Element>(element));
		}
		else if(!IsCollectionLabelControl(controlType) && element.questionText.has_value())
		{
			element.questionText = Substitute(substitutionScope, make_shared<ScreenFile::Element>(element));
		}

		SubstituteRenderingTextFields(substitutionScope, element);
		SubstituteNotificationBannerTextFields(substitutionScope, element);
		SubstituteLinkTextFields(substitutionScope, element);
		SubstituteCollectionLabelTextFields(substitutionScope, element);
		SubstituteOptionDisplayTextFields(substitutionScope, element);
		SubstituteValidationMessageTextField(substitutionScope, element);
		SubstituteRuleErrorMessageTextFields(substitutionScope, element);

		if(hasBoundAttribute && (controlType == "question" || element.boundAttributeId.has_value()))
		{
			shared_ptr<Attribute> att = instance->GetAttributeByID(boundAttributeId);

			if(att != nullptr && !att->IsUnknown())
			{
				string value = att->GetStringValue();

				if(value.empty())
				{
					element.value = nullopt;
				}
				else
				{
					element.value = att->GetStringValue();
				}
			}
		}
		else if(element.controlType == "status")
		{
			if(element.statusData.has_value())
			{
				vector<ScreenFile::InstanceAttribute> statusData = vector<ScreenFile::InstanceAttribute>();

				for(ScreenFile::Column statusCol : element.statusData->columns.value_or(vector<ScreenFile::Column>()))
				{
					for(const shared_ptr<Attribute>& att : instance->attributes)
					{
						if(statusCol.attributeId == att->GetID())
						{
							string value = att->GetStringValue();
						
							if(value.empty())
							{
								value = "";
							}

							ScreenFile::InstanceAttribute ia = ScreenFile::InstanceAttribute();
						
							ia.attributeId = statusCol.attributeId;
							ia.value = value;
						
							statusData.push_back(ia);
						
							break;
						}
					}
				}

				element.statusData->attributes = statusData;
			}
		}
		else if(element.controlType == "summary-table")
		{
			populateSummaryTableRendering(element);
		}
		else if(element.controlType == "summary-list")
		{
			json rendering = element.rendering.has_value() && element.rendering->is_object()
				? element.rendering.value()
				: json::object();
			const bool showPresentInstance = rendering.contains("showPresentInstance")
				&& rendering.at("showPresentInstance").is_boolean()
				&& rendering.at("showPresentInstance").get<bool>();

			rendering["summary"] = GetSummaryData(entities, screens, showPresentInstance ? instance : nullptr);
			element.rendering = rendering;
		}
		else if(element.controlType == "task-list")
		{
			populateTaskListRendering(element);
		}
		else if(IsCollectListControlType(element.controlType))
		{
			if(element.collectData.has_value() && element.collectData->pagination.has_value())
			{
				RuleSet::PaginationInfo& pageInfo = ruleSet->GetPaginationInfo(element.id.value_or("-1"));

				string activeFilter = "";

				if(pageInfo.currentPage < 1)
				{
					pageInfo.currentPage = element.collectData->pagination->currentPage.value_or(1);
					pageInfo.filter = element.collectData->pagination->filter.value_or("");
				}
				else
				{
					element.collectData->pagination->currentPage = pageInfo.currentPage;
					element.collectData->pagination->filter = pageInfo.filter;

					activeFilter = pageInfo.filter;
				}

				GetPage(-1, element, instance, ruleSet, activeFilter);
			}
			else if(element.collectEntity.has_value() && element.collectData.has_value() && element.collectData->columns.has_value())
			{
				element.collectData->instances = vector<ScreenFile::CollectInstance>();
				element.collectData->selectedCount = 0;
				element.collectData->instanceCount = 0;
				for(const shared_ptr<Instance>& linkedInstance : ResolveCollectInstances(entities, element, instance))
				{
					element.collectData->instanceCount = element.collectData->instanceCount.value_or(0) + 1;
					ScreenFile::CollectInstance row = BuildCollectInstance(linkedInstance, element.collectData->columns.value(), element, instance, ruleSet);
					if(row.selected.value_or(false))
					{
						element.collectData->selectedCount = element.collectData->selectedCount.value_or(0) + 1;
					}
					element.collectData->instances->push_back(row);
				}
			}
		}
		else if(element.controlType == "upload")
		{
			if(element.fileUpload.has_value())
			{
				element.fileUpload->files = vector<ScreenFile::FileUploadData>();

				if(ruleSet->uploads != nullptr)
				{
					for(const auto& uploadInfo : *ruleSet->uploads)
					{
						if(uploadInfo.uploadControlId == element.fileUpload->id && uploadInfo.instanceId == instance->instanceId)
						{
							ScreenFile::FileUploadData fileData = ScreenFile::FileUploadData();

							fileData.fileName = uploadInfo.fileName;
							fileData.fileExtension = uploadInfo.fileExtension;
							fileData.fileSize = uploadInfo.fileSize;
							fileData.fileId = uploadInfo.fileId;

							element.fileUpload->files->push_back(fileData);
						}
					}
				}
			}
		}
		for(ScreenFile::Element& child : element.children)
		{
			self(self, child);
		}
	};

	for(ScreenFile::Element& element : screen->elements)
	{
		populateElement(populateElement, element);
	}

	return screen;
}

vector<DataManager::RadioSelectionChange> DataManager::ClearHiddenRadioSelections(
	shared_ptr<vector<shared_ptr<Entity>>> entities,
	shared_ptr<ScreenFile::Screen> screen)
{
	vector<RadioSelectionChange> changes;
	if(entities == nullptr || screen == nullptr || !screen->instanceId.has_value())
	{
		return changes;
	}

	shared_ptr<Instance> contextInstance = GetEntityInstanceById(entities, screen->entityId, screen->instanceId.value());
	if(contextInstance == nullptr)
	{
		return changes;
	}

	const SubstitutionScope scope = BuildSubstitutionScope(entities, contextInstance);
	unordered_set<string> changedKeys;
	const auto inspectElement = [&](const auto& self, ScreenFile::Element& element) -> void {
		const string controlType = LowercaseCopy(element.controlType);
		if((controlType == "radio" || controlType == "dropdown") &&
			element.radioOptionVisibility.value_or(false) &&
			element.boundAttributeId.has_value())
		{
			const string bindingEntityId = element.boundEntityId.value_or(screen->entityId);
			shared_ptr<Instance> bindingInstance = ResolveScopedInstance(scope, bindingEntityId);
			shared_ptr<Attribute> boundAttribute = bindingInstance != nullptr
				? bindingInstance->GetAttributeByID(element.boundAttributeId.value())
				: nullptr;
			if(boundAttribute != nullptr && boundAttribute->IsCertain())
			{
				const string selectedValue = boundAttribute->GetStringValue();
				const bool remainsVisible = element.options.has_value() &&
					std::any_of(element.options->begin(), element.options->end(), [&](const ScreenFile::Options& option) {
						return option.value == selectedValue;
					});
				if(!selectedValue.empty() && !remainsVisible)
				{
					const string key = bindingInstance->entityId + "\x1f" + bindingInstance->instanceId + "\x1f" + element.boundAttributeId.value();
					if(changedKeys.insert(key).second)
					{
						boundAttribute->SetValueFromString("");
						boundAttribute->screenId = screen->id;
						changes.push_back({bindingInstance->entityId, bindingInstance->instanceId, element.boundAttributeId.value()});
					}
					element.value = nullopt;
				}
			}
		}

		for(ScreenFile::Element& child : element.children)
		{
			self(self, child);
		}
	};

	for(ScreenFile::Element& element : screen->elements)
	{
		inspectElement(inspectElement, element);
	}

	return changes;
}

ScreenFile::CollectData DataManager::GetPageData(shared_ptr<vector<shared_ptr<Entity>>> entities,
	shared_ptr<ScreenFile::Screen> screen,
	string entityId,
	string instanceId,
	int page,
	shared_ptr<RuleSet>& ruleSet,
	string filter)
{
	shared_ptr<Instance> instance = GetEntityInstanceById(entities, entityId, instanceId);

	if(screen == nullptr || instance == nullptr || ruleSet == nullptr)
	{
		return ScreenFile::CollectData();
	}

	ScreenFile::CollectData result;
	bool found = false;

	const auto findPageElement = [&](const auto& self, ScreenFile::Element& element) -> void {
		if(found)
		{
			return;
		}

		if(IsCollectListControlType(element.controlType))
		{
			GetPage(page, element, instance, ruleSet, filter);
			result = element.collectData.value_or(ScreenFile::CollectData());
			found = true;

			return;
		}

		for(ScreenFile::Element& child : element.children)
		{
			self(self, child);
		}
	};

	for(ScreenFile::Element& element : screen->elements)
	{
		findPageElement(findPageElement, element);
	}

	if(found)
	{
		return result;
	}

	return ScreenFile::CollectData();
}

bool DataManager::IsCollectTargetInScope(shared_ptr<vector<shared_ptr<Entity>>> entities,
	const ScreenFile::Element& element,
	const shared_ptr<Instance>& contextInstance,
	const string& targetInstanceId)
{
	if(targetInstanceId.empty())
	{
		return false;
	}

	for(const shared_ptr<Instance>& candidate : ResolveCollectInstances(entities, element, contextInstance))
	{
		if(candidate != nullptr && candidate->instanceId == targetInstanceId)
		{
			return true;
		}
	}

	return false;
}

void DataManager::GetPage(int page, ScreenFile::Element& element, shared_ptr<Instance> instance, shared_ptr<RuleSet>& ruleSet, string filter)
{
	if(instance == nullptr || ruleSet == nullptr || !element.collectData.has_value() || !element.collectData->pagination.has_value() || !element.collectData->columns.has_value() || !element.collectEntity.has_value())
	{
		return;
	}

	RuleSet::PaginationInfo& pageInfo = ruleSet->GetPaginationInfo(element.id.value_or("-1"));

	if(page != -1)
	{
		if(page < 1)
		{
			page = 1;
		}

		element.collectData->pagination->filter = filter;

		pageInfo.currentPage = page;
		pageInfo.filter = filter;
	}
	else
	{
		page = pageInfo.currentPage > 0 ? pageInfo.currentPage : element.collectData->pagination->currentPage.value_or(1);

		pageInfo.currentPage = page;

		element.collectData->pagination->filter = pageInfo.filter;
	}

	bool filterActive =  element.collectData->pagination->filter.has_value() && !element.collectData->pagination->filter->empty();

	if(page == 1)
	{
		filterActive = false;

		if(!filter.empty() || (element.collectData->pagination->filter.has_value() && !element.collectData->pagination->filter->empty()))
		{
			filterActive = true;
		}
	}

	int optionsPerPage = element.collectData->pagination->optionsPerPage.value_or(10);

	if(optionsPerPage < 1)
	{
		optionsPerPage = 10;
	}
	
	int currentPage = page;

	if(element.collectData->instances.has_value())
	{
		element.collectData->instances->clear();
	}
	else
	{
		element.collectData->instances = vector<ScreenFile::CollectInstance>();
	}

	shared_ptr<vector<ScreenFile::CollectInstance>> allData = make_shared<vector<ScreenFile::CollectInstance>>();
	element.collectData->selectedCount = 0;
	element.collectData->instanceCount = 0;

	for(const shared_ptr<Instance>& linkedInstance : ResolveCollectInstances(ruleSet->entities, element, instance))
	{
		element.collectData->instanceCount = element.collectData->instanceCount.value_or(0) + 1;
		ScreenFile::CollectInstance rowData = BuildCollectInstance(linkedInstance, element.collectData->columns.value(), element, instance, ruleSet);
		if(rowData.selected.value_or(false))
		{
			element.collectData->selectedCount = element.collectData->selectedCount.value_or(0) + 1;
		}

		if(filterActive)
		{
			if(ApplyFilter(rowData, filter))
			{
				allData->push_back(rowData);
			}
		}
		else
		{
			allData->push_back(rowData);
		}
	}
		
	const int rowCount = static_cast<int>(allData->size());

	int totalPages = (rowCount + optionsPerPage - 1) / optionsPerPage;

	if(totalPages < 1)
	{
		totalPages = 1;
	}

	if(currentPage > totalPages)
	{
		currentPage = totalPages;
	}

	if(currentPage < 1)
	{
		currentPage = 1;
	}

	pageInfo.currentPage = currentPage;

	element.collectData->pagination->totalPages = totalPages;

	element.collectData->pagination->currentPage = currentPage;

	int startIndex = (currentPage - 1) * optionsPerPage;

	int endIndex = min(startIndex + optionsPerPage, rowCount);

	for(int i = startIndex; i < endIndex; ++i)
	{
		element.collectData->instances->push_back((*allData)[i]);
	}
}

// A function that takes in a ScreenFile::CollectInstance and a filter and returns a boolean to indicate if any of the attributes match the filter criteria
bool DataManager::ApplyFilter(const ScreenFile::CollectInstance& instance, const string& filter)
{
	const string normalizedFilter = LowercaseCopy(filter);
	for(const ScreenFile::InstanceAttribute& attr : instance.attributes)
	{
		if(LowercaseCopy(attr.value).find(normalizedFilter) != string::npos)
		{
			return true;
		}
	}

	return false;
}

shared_ptr<Entity> DataManager::GetEntityByID(shared_ptr<vector<shared_ptr<Entity>>> entities, const string& entityId)
{
	return FindEntityByIdIndexed(entities, entityId);
}

shared_ptr<ScreenFile::Screen> DataManager::GetNextScreen(shared_ptr<ScreenFile::ScreenData> screens, const string& currentScreenID, const string& entityId)
{
	if(screens == nullptr)
	{
		return nullptr;
	}

	bool returnNext = false;

	for(const ScreenFile::Screen& screen : screens->screens)
	{
		if(screen.entityId != entityId)
		{
			continue;
		}

		if(returnNext)
		{
			return make_shared<ScreenFile::Screen>(screen);
		}

		if(screen.id == currentScreenID)
		{
			returnNext = true;
		}
	}

	return nullptr;
}

shared_ptr<ScreenFile::Screen> DataManager::GetFirstEntityScreen(shared_ptr<ScreenFile::ScreenData> screens, const string& entityId)
{
	if(screens == nullptr)
	{
		return nullptr;
	}

	bool returnNext = false;

	for(const ScreenFile::Screen& screen : screens->screens)
	{
		if(screen.entityId == entityId)
		{
			return make_shared<ScreenFile::Screen>(screen);
		}
	}

	return nullptr;
}

shared_ptr<ScreenFile::Screen> DataManager::GetScreen(shared_ptr<vector<shared_ptr<Entity>>> entities, 
	                                                  shared_ptr<ScreenFile::ScreenData> screens, 
	                                                  const ScreenLocation& loc,
	                                                  shared_ptr<RuleSet> ruleSet,
													  bool skipDataLoad)
{
	if(screens == nullptr)
	{
		return nullptr;
	}

	shared_ptr<ScreenFile::Screen> screen;

	screen = screens->GetScreenByID(loc.screenId);

	if(screen != nullptr)
	{
		screen->entityId = loc.entityId;

		screen->instanceId = loc.instanceId;

		if(!skipDataLoad)
		{
			screen = GetSessionData(entities, screen, loc.entityId, loc.instanceId, ruleSet, screens.get());
		}

		return screen;
	}

	return nullptr;
}

void DataManager::ConvertValue(const string& strValue, Attribute::AttributeType type, shared_ptr<ValueVariant> outValue)
{
	if(outValue == nullptr)
	{
		return;
	}

	ValueVariant convertedValue;

	if(Attribute::TryConvertValue(strValue, type, convertedValue))
	{
		*outValue = convertedValue;
	}
}

string DataManager::GetFirstInstanceId(shared_ptr<vector<shared_ptr<Entity>>> entities, const string& entityId)
{
	shared_ptr<Entity> entity = GetEntityByID(entities, entityId);

	return entity != nullptr && !entity->instances.empty() ? entity->instances[0]->instanceId : "";
}

void DataManager::DeleteInstance(string entityId, string instanceId, shared_ptr<vector<shared_ptr<Entity>>> entities)
{	
	if(entities == nullptr)
	{
		return;
	}

	shared_ptr<Entity> entity = GetEntityByID(entities, entityId);

	if(entity == nullptr)
	{
		return;
	}

	shared_ptr<Instance> instance = entity->GetInstanceById(instanceId);

	if(instance == nullptr)
	{
		return;
	}

	entity->CascadeDelete(instance, instance, entities, true);
}

Summary::SummaryData DataManager::GetSummaryData(shared_ptr<vector<shared_ptr<Entity>>> entities,
	const ScreenFile::ScreenData* screens,
	shared_ptr<Instance> scopeInstance)
{
	Summary::SummaryData summaryData = {}; // = Summary::SummaryData();
	
	if(entities == nullptr)
	{
		return summaryData;
	}

	unordered_map<string, size_t> entityOrder;
	unordered_map<string, unordered_map<string, size_t>> instanceOrder;
	unordered_map<string, string> summaryScreenTargets;
	const auto summaryTargetKey = [](const string& entityId, const string& attributeId) {
		return entityId + "\n" + attributeId;
	};

	for(size_t entityIndex = 0; entityIndex < entities->size(); ++entityIndex)
	{
		const shared_ptr<Entity>& entity = (*entities)[entityIndex];
		if(entity == nullptr)
		{
			continue;
		}

		entityOrder[entity->entityid] = entityIndex;

		for(size_t instanceIndex = 0; instanceIndex < entity->instances.size(); ++instanceIndex)
		{
			const shared_ptr<Instance>& instance = entity->instances[instanceIndex];
			if(instance != nullptr)
			{
				instanceOrder[entity->entityid][instance->instanceId] = instanceIndex;
			}
		}
	}

	const auto elementCanSupplyChangeTarget = [](const ScreenFile::Element& element) {
		const string controlType = LowercaseCopy(element.controlType);

		// Summary rows are answers, not a dump of every known attribute. Only
		// controls that genuinely collect an attribute value can provide the
		// fallback Change target used for restored/pre-populated answers.
		return controlType == "question"
			|| controlType == "textbox"
			|| controlType == "text"
			|| controlType == "text-input"
			|| controlType == "number"
			|| controlType == "currency"
			|| controlType == "email"
			|| controlType == "password"
			|| controlType == "date"
			|| controlType == "datetime"
			|| controlType == "time"
			|| controlType == "textarea"
			|| controlType == "character-count"
			|| controlType == "dropdown"
			|| controlType == "select"
			|| controlType == "radio"
			|| controlType == "checkbox"
			|| controlType == "file";
	};

	if(screens != nullptr)
	{
		const auto addScreenTargets = [&](const auto& self,
		                                  const ScreenFile::Screen& screen,
		                                  const vector<ScreenFile::Element>& elements) -> void {
			for(const ScreenFile::Element& element : elements)
			{
				const string attributeId = element.boundAttributeId.value_or(element.id.value_or(""));
				const string targetEntityId = element.boundEntityId.value_or(screen.entityId);

				if(!attributeId.empty() && !targetEntityId.empty() && elementCanSupplyChangeTarget(element))
				{
					summaryScreenTargets.emplace(summaryTargetKey(targetEntityId, attributeId), screen.id);
				}

				if(!element.children.empty())
				{
					self(self, screen, element.children);
				}
			}
		};

		for(const ScreenFile::Screen& screen : screens->screens)
		{
			if(screen.id.empty())
			{
				continue;
			}

			addScreenTargets(addScreenTargets, screen, screen.elements);
		}
	}

	const auto entityName = [](const shared_ptr<Entity>& entity) {
		if(entity == nullptr)
		{
			return string();
		}

		return !entity->description.empty() ? entity->description : entity->entityid;
	};

	const auto hasNonBlankText = [](const string& value) {
		return value.find_first_not_of(" \t\r\n") != string::npos;
	};

	const auto mainIdentifierAttributeId = [](const shared_ptr<Entity>& entity) {
		if(entity == nullptr)
		{
			return string();
		}

		if(entity->identifyingAttribute != nullptr)
		{
			return entity->identifyingAttribute->GetID();
		}

		for(const shared_ptr<Attribute>& att : entity->attributes)
		{
			if(att != nullptr && att->IsMainIdentifier())
			{
				return att->GetID();
			}
		}

		return string();
	};

	const auto mainIdentifierValue = [&](const shared_ptr<Entity>& entity, const shared_ptr<Instance>& instance) {
		if(instance == nullptr || (entity != nullptr && entity->root.value_or(false)))
		{
			return string();
		}

		const string identifierAttributeId = mainIdentifierAttributeId(entity);
		if(!identifierAttributeId.empty())
		{
			shared_ptr<Attribute> identifierAttribute = instance->GetAttributeByID(identifierAttributeId);
			if(identifierAttribute != nullptr && !identifierAttribute->IsUnknown())
			{
				string value = identifierAttribute->GetStringValue();
				if(hasNonBlankText(value))
				{
					return value;
				}
			}
		}

		for(const shared_ptr<Attribute>& att : instance->attributes)
		{
			if(att == nullptr || !att->IsMainIdentifier() || att->IsUnknown())
			{
				continue;
			}

			string value = att->GetStringValue();
			if(hasNonBlankText(value))
			{
				return value;
			}
		}

		return string();
	};

	const auto instanceName = [&](const shared_ptr<Entity>& entity, const shared_ptr<Instance>& instance) {
		if(instance == nullptr)
		{
			return string();
		}

		const string identifierValue = mainIdentifierValue(entity, instance);
		return !identifierValue.empty() ? identifierValue : instance->instanceId;
	};

	const auto buildSummaryAttributes = [&](const shared_ptr<Instance>& instance) {
		vector<Summary::SummaryAttribute> attributes;

		if(instance == nullptr)
		{
			return attributes;
		}

		for(const shared_ptr<Attribute>& att : instance->attributes)
		{
			if(att == nullptr || att->IsUnknown() || att->IsInferred() || IsSummaryTechnicalRowIdentifier(att))
			{
				continue;
			}

			Summary::SummaryAttribute summaryAttribute; // = Summary::SummaryAttribute();

			summaryAttribute.attributeId = att->GetID();
			summaryAttribute.name = att->GetAttributeText();
			summaryAttribute.value = att->GetStringValue();
			summaryAttribute.screenId = att->screenId;
			if(summaryAttribute.screenId.empty())
			{
				auto targetIt = summaryScreenTargets.find(summaryTargetKey(instance->entityId, att->GetID()));
				if(targetIt != summaryScreenTargets.end())
				{
					summaryAttribute.screenId = targetIt->second;
				}
			}
			if(summaryAttribute.screenId.empty())
			{
				continue;
			}
			summaryAttribute.entityId = instance->entityId;
			summaryAttribute.instanceId = instance->instanceId;
			summaryAttribute.elementId = att->GetID();

			attributes.push_back(summaryAttribute);
		}

		return attributes;
	};

	struct TraversalChild
	{
		shared_ptr<Entity> entity;
		shared_ptr<Instance> instance;
		string relationshipId;
	};

	unordered_set<string> visited;

	const auto instanceKey = [](const shared_ptr<Instance>& instance) {
		return instance != nullptr ? instance->entityId + "\n" + instance->instanceId : string();
	};

	const auto appendInstanceGroup = [&](const shared_ptr<Entity>& entity,
	                                     const shared_ptr<Instance>& instance,
	                                     const string& parentEntityId,
	                                     const string& parentInstanceId,
	                                     const string& relationshipId) {
		vector<Summary::SummaryAttribute> attributes = buildSummaryAttributes(instance);

		if(attributes.empty())
		{
			return;
		}

		Summary::SummaryGroup group;
		group.entityId = entity != nullptr ? entity->entityid : instance->entityId;
		group.entityName = entityName(entity);
		group.instanceId = instance->instanceId;
		group.instanceName = instanceName(entity, instance);
		group.displayHeading = !mainIdentifierValue(entity, instance).empty();
		group.parentEntityId = parentEntityId;
		group.parentInstanceId = parentInstanceId;
		group.relationshipId = relationshipId;

		for(const Summary::SummaryAttribute& attribute : attributes)
		{
			group.attributes->push_back(attribute);
		}

		summaryData.groups->push_back(group);
	};

	const auto relatedInstanceOrder = [&](const TraversalChild& child) {
		size_t entityIndex = (std::numeric_limits<size_t>::max)();
		size_t instanceIndex = (std::numeric_limits<size_t>::max)();

		if(child.entity != nullptr)
		{
			auto entityIt = entityOrder.find(child.entity->entityid);
			if(entityIt != entityOrder.end())
			{
				entityIndex = entityIt->second;
			}

			auto instanceEntityIt = instanceOrder.find(child.entity->entityid);
			if(instanceEntityIt != instanceOrder.end() && child.instance != nullptr)
			{
				auto instanceIt = instanceEntityIt->second.find(child.instance->instanceId);
				if(instanceIt != instanceEntityIt->second.end())
				{
					instanceIndex = instanceIt->second;
				}
			}
		}

		return make_pair(entityIndex, instanceIndex);
	};

	const auto relationshipIdForLinkedChild = [](const shared_ptr<Entity>& entity,
	                                             const shared_ptr<Instance>& linkedInstance) {
		if(entity == nullptr || linkedInstance == nullptr)
		{
			return string();
		}

		for(const shared_ptr<Relationship>& relationship : entity->relationships)
		{
			if(relationship == nullptr)
			{
				continue;
			}

			if(relationship->sourceEntity == entity->entityid && relationship->targetEntity == linkedInstance->entityId)
			{
				return relationship->id;
			}
		}

		return string();
	};

	const auto appendTraversal = [&](const auto& self,
	                                 const shared_ptr<Entity>& entity,
	                                 const shared_ptr<Instance>& instance,
	                                 const string& parentEntityId,
	                                 const string& parentInstanceId,
	                                 const string& relationshipId) -> void {
		if(instance == nullptr)
		{
			return;
		}

		const string key = instanceKey(instance);
		if(!visited.insert(key).second)
		{
			return;
		}

		appendInstanceGroup(entity, instance, parentEntityId, parentInstanceId, relationshipId);

		vector<TraversalChild> children;
		for(const InstanceRelationshipLink& link : instance->relationshipLinks)
		{
			string childEntityId;
			string childInstanceId;

			if(scopeInstance != nullptr)
			{
				const shared_ptr<Relationship> relationship = FindRelationshipByIdentity(entities, link.relationshipId);
				if(relationship == nullptr)
				{
					continue;
				}

				const string cardinality = NormalizeRelationshipCardinality(relationship->type);
				const bool currentIsParentOfTarget =
					(cardinality == "1m" || cardinality == "onetomany" || cardinality == "11" || cardinality == "onetoone")
					&& link.outgoing
					&& instance->entityId == relationship->sourceEntity
					&& link.sourceInstanceId == instance->instanceId
					&& link.targetEntityId == relationship->targetEntity;
				const bool currentIsParentOfSource =
					(cardinality == "m1" || cardinality == "manytoone")
					&& !link.outgoing
					&& instance->entityId == relationship->targetEntity
					&& link.targetInstanceId == instance->instanceId
					&& link.sourceEntityId == relationship->sourceEntity;

				if(currentIsParentOfTarget)
				{
					childEntityId = link.targetEntityId;
					childInstanceId = link.targetInstanceId;
				}
				else if(currentIsParentOfSource)
				{
					childEntityId = link.sourceEntityId;
					childInstanceId = link.sourceInstanceId;
				}
				else
				{
					continue;
				}
			}
			else
			{
				if(!link.outgoing)
				{
					continue;
				}

				childEntityId = link.targetEntityId;
				childInstanceId = link.targetInstanceId;
			}

			if(childEntityId.empty() || childInstanceId.empty())
			{
				continue;
			}

			shared_ptr<Instance> childInstance = link.linkedInstance.lock();
			if(childInstance == nullptr
				|| childInstance->entityId != childEntityId
				|| childInstance->instanceId != childInstanceId)
			{
				childInstance = GetEntityInstanceById(entities, childEntityId, childInstanceId);
			}

			if(childInstance == nullptr)
			{
				continue;
			}

			shared_ptr<Entity> childEntity = FindEntityByIdIndexed(entities, childEntityId);
			children.push_back(TraversalChild{childEntity, childInstance, link.relationshipId});
		}

		unordered_set<string> childKeys;
		for(const TraversalChild& child : children)
		{
			childKeys.insert(instanceKey(child.instance));
		}

		// Present-instance summaries are a strict concrete-instance view. Legacy
		// linkedInstances do not carry a relationship identity and can therefore
		// include stale or cross-sibling links; only exact relationshipLinks are
		// authoritative for a scoped traversal. The unscoped legacy summary keeps
		// its compatibility fallback.
		if(scopeInstance == nullptr)
		{
			for(const shared_ptr<Instance>& linkedInstance : instance->linkedInstances)
			{
				if(linkedInstance == nullptr || linkedInstance->entityId.empty() || linkedInstance->entityId == instance->entityId)
				{
					continue;
				}

				const string linkedRelationshipId = relationshipIdForLinkedChild(entity, linkedInstance);
				if(linkedRelationshipId.empty())
				{
					continue;
				}

				const string linkedKey = instanceKey(linkedInstance);
				if(!childKeys.insert(linkedKey).second)
				{
					continue;
				}

				shared_ptr<Entity> childEntity = FindEntityByIdIndexed(entities, linkedInstance->entityId);
				children.push_back(TraversalChild{childEntity, linkedInstance, linkedRelationshipId});
			}
		}

		sort(children.begin(), children.end(), [&](const TraversalChild& left, const TraversalChild& right) {
			return relatedInstanceOrder(left) < relatedInstanceOrder(right);
		});

		for(const TraversalChild& child : children)
		{
			self(self,
			     child.entity,
			     child.instance,
			     instance->entityId,
			     instance->instanceId,
			     child.relationshipId);
		}
	};

	if(scopeInstance != nullptr)
	{
		shared_ptr<Instance> resolvedScopeInstance = GetEntityInstanceById(
			entities,
			scopeInstance->entityId,
			scopeInstance->instanceId);
		if(resolvedScopeInstance != nullptr)
		{
			shared_ptr<Entity> scopeEntity = FindEntityByIdIndexed(entities, resolvedScopeInstance->entityId);
			appendTraversal(appendTraversal, scopeEntity, resolvedScopeInstance, string(), string(), string());
		}
	}
	else
	{
		for(const shared_ptr<Entity>& entity : *entities)
		{
			if(entity == nullptr)
			{
				continue;
			}

			for(const shared_ptr<Instance>& instance : entity->instances)
			{
				if(instance != nullptr && (instance->isRoot || entity->root.value_or(false)))
				{
					appendTraversal(appendTraversal, entity, instance, string(), string(), string());
				}
			}
		}

		for(const shared_ptr<Entity>& entity : *entities)
		{
			if(entity == nullptr)
			{
				continue;
			}

			for(const shared_ptr<Instance>& instance : entity->instances)
			{
				appendTraversal(appendTraversal, entity, instance, string(), string(), string());
			}
		}
	}

	for(const shared_ptr<Entity>& ent : *entities)
	{
		if(ent == nullptr || ent->instances.empty())
		{
			continue;
		}

		Summary::SummaryEntity summaryEntity = {};
		summaryEntity.entityId = ent->entityid;
		summaryEntity.entityName = entityName(ent);
		bool entityIsInScope = scopeInstance == nullptr;

		for(const shared_ptr<Instance>& inst : ent->instances)
		{
			if(inst == nullptr || visited.find(instanceKey(inst)) == visited.end())
			{
				continue;
			}

			entityIsInScope = true;
			Summary::SummaryInstance summaryInstance;
			summaryInstance.instanceId = inst->instanceId;
			summaryInstance.instanceName = instanceName(ent, inst);

			for(const Summary::SummaryAttribute& summaryAttribute : buildSummaryAttributes(inst))
			{
				summaryInstance.attributes->push_back(summaryAttribute);
			}

			if(!summaryInstance.attributes->empty())
			{
				summaryEntity.instances->push_back(summaryInstance);
			}
		}

		if(entityIsInScope)
		{
			summaryData.entities->push_back(summaryEntity);
		}
	}

	return summaryData;
}

void DataManager::Copy(shared_ptr<Instance> sourceInstance, shared_ptr<Instance> targetInstance)
{
	if(sourceInstance == nullptr || targetInstance == nullptr)
	{
		return;
	}

	for(shared_ptr<Attribute>& targetAtt : targetInstance->attributes)
	{
		if(targetAtt == nullptr)
		{
			continue;
		}

		CopyFrom cf = targetAtt->GetCopyFrom();

		if(cf.sourceEntityId.empty() || cf.sourceAttributeId.empty())
		{
			continue;
		}

		if(sourceInstance->entityId != cf.sourceEntityId)
		{
			continue;
		}

		for(shared_ptr<Attribute>& sourceAtt : sourceInstance->attributes)
		{
			if(sourceAtt == nullptr)
			{
				continue;
			}

			if(sourceAtt->GetID() == cf.sourceAttributeId)
			{
				if(sourceAtt->IsUnknown())
				{
					targetAtt->SetValueFromString("");
				}
				else
				{
					string sourceValue = sourceAtt->GetRawValue();

					if(sourceValue.empty())
					{
						sourceValue = sourceAtt->GetStringValue();
					}

					targetAtt->SetValueFromString(sourceValue);
				}
				
				break;
			}
		}
	}
}

shared_ptr<ScreenFile::Screen> DataManager::HandleData(SessionEngine::ISessionState& sessionState,
                                                      SessionEngine::IRuleEngine& ruleEngine,
                                                      shared_ptr<ScreenData::DataRoot> data,
                                                      shared_ptr<ScreenFile::Screen> screen,
                                                      shared_ptr<ScreenFile::ScreenData> screens,
                                                      string directTo)
{
    std::vector<SessionEngine::AttributeMutation> mutations;

    if(data != nullptr)
    {
        for(const auto& input : data->data)
        {
            string entityId = data->entityId.empty() && screen != nullptr ? screen->entityId : data->entityId;
            string instanceId = data->instanceId;
            if(instanceId.empty())
            {
                instanceId = GetFirstInstanceId(sessionState, entityId);
            }
            UpdateAttributeValue(sessionState, input.attributeId, entityId, instanceId, input.value, screen != nullptr ? screen->id : "");
            SessionEngine::AttributeMutation mutation;
            mutation.entityId = entityId;
            mutation.instanceId = instanceId;
            mutation.attributeId = input.attributeId;
            mutation.rawValue = input.value;
            mutations.push_back(mutation);
        }
    }

    ruleEngine.Evaluate(sessionState, mutations);

    auto targetScreenOpt = ruleEngine.ResolveNextScreen(sessionState, directTo);
    string nextScreenId = targetScreenOpt.has_value() ? targetScreenOpt.value() : (data != nullptr ? data->nextScreenId : "");

    if(nextScreenId.empty() && screens != nullptr && screen != nullptr)
    {
        auto next = GetNextScreen(screens, screen->id, screen->entityId);
        if(next != nullptr)
        {
            nextScreenId = next->id;
        }
    }

    return GetScreenByID(screens, nextScreenId);
}

void DataManager::UpdateAttributeValue(SessionEngine::ISessionState& sessionState, const string& attributeId, const string& entityId, const string& instanceId, const string& strValue, const string& screenId)
{
    auto instance = sessionState.GetInstance(entityId, instanceId);
    if(instance == nullptr)
    {
        instance = sessionState.CreateInstance(entityId);
    }
    if(instance != nullptr)
    {
        instance->SetAttributeValue(attributeId, strValue);
    }
}

void DataManager::DeleteInstance(string entityId, string instanceId, SessionEngine::ISessionState& sessionState)
{
    sessionState.DeleteInstance(entityId, instanceId);
}

void DataManager::Copy(const string& sourceInstanceId, const string& targetInstanceId, SessionEngine::ISessionState& sessionState)
{
    sessionState.CopyInstanceData(sourceInstanceId, targetInstanceId);
}

string DataManager::GetFirstInstanceId(const SessionEngine::ISessionState& sessionState, const string& entityId)
{
    auto instances = sessionState.GetAllInstances(entityId);
    if(!instances.empty() && instances[0] != nullptr)
    {
        return instances[0]->GetInstanceId();
    }
    return "";
}

Summary::SummaryData DataManager::GetSummaryData(const SessionEngine::ISessionState& sessionState,
    const ScreenFile::ScreenData* screens,
    const string& scopeEntityId,
    const string& scopeInstanceId)
{
    Summary::SummaryData summaryData;
    std::vector<Summary::SummaryGroup> groups;
    std::vector<Summary::SummaryEntity> entities;

    auto entityIds = sessionState.GetEntityIds();
    for(const auto& entityId : entityIds)
    {
        if(!scopeEntityId.empty() && entityId != scopeEntityId)
        {
            continue;
        }

        Summary::SummaryEntity summaryEntity;
        summaryEntity.entityId = entityId;
        summaryEntity.entityName = entityId;
        std::vector<Summary::SummaryInstance> summaryInstances;

        auto instances = sessionState.GetAllInstances(entityId);
        for(const auto& inst : instances)
        {
            if(inst == nullptr) continue;
            if(!scopeInstanceId.empty() && inst->GetInstanceId() != scopeInstanceId)
            {
                continue;
            }

            Summary::SummaryInstance summaryInst;
            summaryInst.instanceId = inst->GetInstanceId();
            summaryInst.instanceName = inst->GetInstanceId();
            std::vector<Summary::SummaryAttribute> summaryAttrs;

            Summary::SummaryGroup group;
            group.entityId = entityId;
            group.entityName = entityId;
            group.instanceId = inst->GetInstanceId();
            group.instanceName = inst->GetInstanceId();

            auto attrs = inst->GetAllAttributes();
            for(const auto& [attrId, val] : attrs)
            {
                Summary::SummaryAttribute sa;
                sa.attributeId = attrId;
                sa.name = attrId;
                sa.value = val;
                sa.entityId = entityId;
                sa.instanceId = inst->GetInstanceId();
                summaryAttrs.push_back(sa);
            }

            summaryInst.attributes = summaryAttrs;
            summaryInstances.push_back(summaryInst);

            group.attributes = summaryAttrs;
            groups.push_back(group);
        }

        summaryEntity.instances = summaryInstances;
        entities.push_back(summaryEntity);
    }

    summaryData.groups = groups;
    summaryData.entities = entities;
    return summaryData;
}



