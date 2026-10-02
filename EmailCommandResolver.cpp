#include "EmailCommandResolver.h"

#include "DataManager.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
	using json = nlohmann::json;

	std::string JsonString(const json& value, const char* key)
	{
		if(!value.is_object() || !value.contains(key) || !value.at(key).is_string())
		{
			return {};
		}
		return value.at(key).get<std::string>();
	}

	std::string Trim(std::string value)
	{
		while(!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
		while(!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
		return value;
	}

	std::uint64_t Fnv1a(const std::string& value, std::uint64_t seed)
	{
		std::uint64_t hash = seed;
		for(unsigned char character : value)
		{
			hash ^= static_cast<std::uint64_t>(character);
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	std::string StableCommandHash(const std::string& value)
	{
		std::ostringstream output;
		output << std::hex << std::setfill('0')
			<< std::setw(16) << Fnv1a(value, 1469598103934665603ULL)
			<< std::setw(16) << Fnv1a(value, 1099511628211ULL);
		return output.str();
	}

	std::string UtcNow()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t time = std::chrono::system_clock::to_time_t(now);
		std::tm utc{};
#if defined(_WIN32)
		gmtime_s(&utc, &time);
#else
		gmtime_r(&time, &utc);
#endif
		std::ostringstream output;
		output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
		return output.str();
	}

	std::shared_ptr<Instance> ResolveScalarInstance(
		RuleSet& ruleSet,
		const std::string& entityId,
		const std::string& currentEntityId,
		const std::string& currentInstanceId)
	{
		DataManager dataManager;
		if(entityId == currentEntityId && !currentInstanceId.empty())
		{
			return dataManager.GetEntityInstanceById(ruleSet.entities, entityId, currentInstanceId);
		}

		std::shared_ptr<Entity> entity = dataManager.GetEntityByID(ruleSet.entities, entityId);
		return entity != nullptr && entity->instances.size() == 1 ? entity->instances.front() : nullptr;
	}

	bool ResolveScalarAttribute(
		RuleSet& ruleSet,
		const std::string& entityId,
		const std::string& attributeId,
		const std::string& currentEntityId,
		const std::string& currentInstanceId,
		json& value)
	{
		std::shared_ptr<Instance> instance = ResolveScalarInstance(ruleSet, entityId, currentEntityId, currentInstanceId);
		std::shared_ptr<Attribute> attribute = instance != nullptr ? instance->GetAttributeByID(attributeId) : nullptr;
		if(attribute == nullptr || !attribute->IsKnown())
		{
			return false;
		}

		if(attribute->GetType() == Attribute::BOOLEAN)
		{
			value = attribute->GetStringValue() == "true";
		}
		else
		{
			value = attribute->GetStringValue();
		}
		return true;
	}

	bool ResolveAttributeList(RuleSet& ruleSet, const std::string& entityId, const std::string& attributeId, json& value)
	{
		DataManager dataManager;
		std::shared_ptr<Entity> entity = dataManager.GetEntityByID(ruleSet.entities, entityId);
		if(entity == nullptr)
		{
			return false;
		}

		value = json::array();
		for(const std::shared_ptr<Instance>& instance : entity->instances)
		{
			std::shared_ptr<Attribute> attribute = instance != nullptr ? instance->GetAttributeByID(attributeId) : nullptr;
			if(attribute != nullptr && attribute->IsKnown())
			{
				value.push_back(attribute->GetStringValue());
			}
		}
		return true;
	}

	bool AppendResolvedMessageValue(const json& value, std::string& message)
	{
		if(value.is_string())
		{
			message += value.get<std::string>();
			return true;
		}
		if(value.is_boolean())
		{
			message += value.get<bool>() ? "true" : "false";
			return true;
		}
		if(value.is_number())
		{
			message += value.dump();
			return true;
		}
		return false;
	}

	bool ResolveMessageParagraph(
		RuleSet& ruleSet,
		const json& paragraph,
		const std::string& currentEntityId,
		const std::string& currentInstanceId,
		std::string& message)
	{
		if(!paragraph.is_object() || !paragraph.contains("runs") || !paragraph.at("runs").is_array())
		{
			return false;
		}

		for(const json& run : paragraph.at("runs"))
		{
			const std::string runType = JsonString(run, "runType");
			if(runType == "Text")
			{
				message += JsonString(run, "text");
				continue;
			}
			if(runType != "Attribute")
			{
				return false;
			}

			json resolvedAttribute;
			if(!ResolveScalarAttribute(
				ruleSet,
				JsonString(run, "entityId"),
				JsonString(run, "attributeId"),
				currentEntityId,
				currentInstanceId,
				resolvedAttribute) ||
				!AppendResolvedMessageValue(resolvedAttribute, message))
			{
				return false;
			}
		}
		return true;
	}

	bool FindRepeatingParagraphEntity(
		RuleSet& ruleSet,
		const json& paragraph,
		std::shared_ptr<Entity>& repeatingEntity)
	{
		repeatingEntity.reset();
		if(!paragraph.is_object() || !paragraph.contains("runs") || !paragraph.at("runs").is_array())
		{
			return false;
		}

		DataManager dataManager;
		for(const json& run : paragraph.at("runs"))
		{
			const std::string runType = JsonString(run, "runType");
			if(runType == "Text")
			{
				continue;
			}
			if(runType != "Attribute")
			{
				return false;
			}

			const std::string entityId = JsonString(run, "entityId");
			std::shared_ptr<Entity> entity = dataManager.GetEntityByID(ruleSet.entities, entityId);
			if(entity == nullptr)
			{
				return false;
			}

			// Root and singleton entities retain the existing scalar behaviour. A
			// non-Root entity with zero or multiple instances drives paragraph
			// expansion in stable entity-instance order.
			if(entity->root.value_or(false) || entity->instances.size() == 1)
			{
				continue;
			}
			if(repeatingEntity != nullptr && repeatingEntity->entityid != entity->entityid)
			{
				return false;
			}
			repeatingEntity = entity;
		}
		return true;
	}

	bool ResolveComposedMessage(
		RuleSet& ruleSet,
		const json& mapping,
		const std::string& currentEntityId,
		const std::string& currentInstanceId,
		json& value)
	{
		if(!mapping.contains("messageParagraphs") || !mapping.at("messageParagraphs").is_array())
		{
			return false;
		}

		std::string message;
		bool hasRenderedParagraph = false;
		for(const json& paragraph : mapping.at("messageParagraphs"))
		{
			std::shared_ptr<Entity> repeatingEntity;
			if(!FindRepeatingParagraphEntity(ruleSet, paragraph, repeatingEntity))
			{
				return false;
			}

			std::vector<std::string> renderedParagraphs;
			if(repeatingEntity == nullptr)
			{
				std::string renderedParagraph;
				if(!ResolveMessageParagraph(ruleSet, paragraph, currentEntityId, currentInstanceId, renderedParagraph))
				{
					return false;
				}
				renderedParagraphs.push_back(std::move(renderedParagraph));
			}
			else
			{
				renderedParagraphs.reserve(repeatingEntity->instances.size());
				for(const std::shared_ptr<Instance>& instance : repeatingEntity->instances)
				{
					if(instance == nullptr)
					{
						return false;
					}
					std::string renderedParagraph;
					if(!ResolveMessageParagraph(
						ruleSet,
						paragraph,
						repeatingEntity->entityid,
						instance->instanceId,
						renderedParagraph))
					{
						return false;
					}
					renderedParagraphs.push_back(std::move(renderedParagraph));
				}
			}

			if(renderedParagraphs.empty())
			{
				continue;
			}
			if(hasRenderedParagraph)
			{
				message += "\n\n";
			}
			hasRenderedParagraph = true;
			for(std::size_t index = 0; index < renderedParagraphs.size(); ++index)
			{
				if(index > 0)
				{
					message += "\n";
				}
				message += renderedParagraphs[index];
			}
		}

		value = message;
		return true;
	}

	const json* FindTemplate(const RuleSet& ruleSet, const std::string& templateId)
	{
		if(ruleSet.emailTemplates == nullptr)
		{
			return nullptr;
		}
		for(const json& candidate : *ruleSet.emailTemplates)
		{
			if(JsonString(candidate, "id") == templateId)
			{
				return &candidate;
			}
		}
		return nullptr;
	}

	EmailCommandResolver::Result FailedCommand(
		const std::string& commandId,
		const std::string& transitionId,
		const std::string& templateId,
		const std::string& errorCode)
	{
		EmailCommandResolver::Result result;
		result.configured = true;
		result.errorCode = errorCode;
		result.command = {
			{"commandId", commandId},
			{"idempotencyKey", commandId},
			{"transitionId", transitionId},
			{"projectEmailTemplateId", templateId},
			{"state", "PermanentFailure"},
			{"errorCode", errorCode},
			{"createdAt", UtcNow()}
		};
		return result;
	}
}

EmailCommandResolver::Result EmailCommandResolver::Resolve(
	RuleSet& ruleSet,
	const std::string& projectEmailTemplateId,
	const std::string& transitionId,
	const std::string& sessionIdentity,
	std::uint64_t committedSequence,
	const std::string& currentEntityId,
	const std::string& currentInstanceId)
{
	Result result;
	if(projectEmailTemplateId.empty())
	{
		return result;
	}

	result.configured = true;
	const std::string identityMaterial = sessionIdentity + "\n" + std::to_string(committedSequence) + "\n" + transitionId + "\n" + projectEmailTemplateId;
	const std::string commandId = "email-" + StableCommandHash(identityMaterial);
	const json* emailTemplate = FindTemplate(ruleSet, projectEmailTemplateId);
	if(!ruleSet.notify.enabled || emailTemplate == nullptr)
	{
		return FailedCommand(commandId, transitionId, projectEmailTemplateId, "email_configuration_invalid");
	}

	const std::string recipientEntityId = JsonString(*emailTemplate, "recipientEntityId");
	const std::string recipientAttributeId = JsonString(*emailTemplate, "recipientAttributeId");
	json recipientValue;
	if(!ResolveScalarAttribute(ruleSet, recipientEntityId, recipientAttributeId, currentEntityId, currentInstanceId, recipientValue) || !recipientValue.is_string())
	{
		return FailedCommand(commandId, transitionId, projectEmailTemplateId, "email_recipient_unresolved");
	}

	const std::string recipient = Trim(recipientValue.get<std::string>());
	if(recipient.empty() || recipient.size() > 320 || recipient.find('@') == std::string::npos || recipient.find('\r') != std::string::npos || recipient.find('\n') != std::string::npos)
	{
		return FailedCommand(commandId, transitionId, projectEmailTemplateId, "email_recipient_invalid");
	}

	json personalisation = json::object();
	if(emailTemplate->contains("personalisation") && emailTemplate->at("personalisation").is_array())
	{
		for(const json& mapping : emailTemplate->at("personalisation"))
		{
			const std::string placeholder = Trim(JsonString(mapping, "placeholderName"));
			const std::string mode = JsonString(mapping, "valueMode");
			const bool required = !mapping.contains("required") || !mapping.at("required").is_boolean() || mapping.at("required").get<bool>();
			json value;
			bool resolved = false;
			if(mode == "Static")
			{
				value = JsonString(mapping, "staticValue");
				resolved = !required || !value.get<std::string>().empty();
			}
			else if(mode == "AttributeList")
			{
				resolved = ResolveAttributeList(ruleSet, JsonString(mapping, "entityId"), JsonString(mapping, "attributeId"), value);
				resolved = resolved && (!required || !value.empty());
			}
			else if(mode == "ComposedMessage")
			{
				resolved = ResolveComposedMessage(ruleSet, mapping, currentEntityId, currentInstanceId, value);
				resolved = resolved && value.is_string() && (!required || !value.get<std::string>().empty());
			}
			else if(mode == "Attribute" || mode == "OptionalBoolean")
			{
				resolved = ResolveScalarAttribute(
					ruleSet,
					JsonString(mapping, "entityId"),
					JsonString(mapping, "attributeId"),
					currentEntityId,
					currentInstanceId,
					value);
				if(mode == "OptionalBoolean")
				{
					resolved = resolved && value.is_boolean();
				}
			}

			if(!resolved)
			{
				return FailedCommand(commandId, transitionId, projectEmailTemplateId, "email_personalisation_unresolved");
			}
			personalisation[placeholder] = value;
		}
	}

	std::string profileKey = Trim(JsonString(*emailTemplate, "approvedProfileKey"));
	if(profileKey.empty()) profileKey = ruleSet.notify.approvedProfileKey;
	std::string replyToEmailId = Trim(JsonString(*emailTemplate, "replyToEmailId"));
	if(replyToEmailId.empty()) replyToEmailId = ruleSet.notify.defaultReplyToEmailId;
	if(profileKey.empty() || JsonString(*emailTemplate, "notifyTemplateId").empty())
	{
		return FailedCommand(commandId, transitionId, projectEmailTemplateId, "email_configuration_invalid");
	}

	result.succeeded = true;
	result.command = {
		{"commandId", commandId},
		{"idempotencyKey", commandId},
		{"actionId", transitionId + ":email:" + projectEmailTemplateId},
		{"transitionId", transitionId},
		{"projectEmailTemplateId", projectEmailTemplateId},
		{"approvedProfileKey", profileKey},
		{"notifyTemplateId", JsonString(*emailTemplate, "notifyTemplateId")},
		{"recipient", recipient},
		{"personalisation", personalisation},
		{"sanitisePersonalisation", !emailTemplate->contains("sanitisePersonalisation") || !emailTemplate->at("sanitisePersonalisation").is_boolean() || emailTemplate->at("sanitisePersonalisation").get<bool>()},
		{"state", "Pending"},
		{"createdAt", UtcNow()}
	};
	if(emailTemplate->contains("notifyTemplateVersion") && emailTemplate->at("notifyTemplateVersion").is_number_integer())
	{
		result.command["notifyTemplateVersion"] = emailTemplate->at("notifyTemplateVersion");
	}
	if(!replyToEmailId.empty())
	{
		result.command["replyToEmailId"] = replyToEmailId;
	}
	return result;
}
