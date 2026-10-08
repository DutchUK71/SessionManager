#pragma once

#include "Rules/RuleSet.h"

#include <cstdint>
#include <string>

class EmailCommandResolver
{
public:
	struct Result
	{
		bool configured = false;
		bool succeeded = false;
		std::string errorCode;
		nlohmann::json command = nullptr;
	};

	static Result Resolve(
		RuleSet& ruleSet,
		const std::string& projectEmailTemplateId,
		const std::string& transitionId,
		const std::string& sessionIdentity,
		std::uint64_t committedSequence,
		const std::string& currentEntityId,
		const std::string& currentInstanceId);
};
