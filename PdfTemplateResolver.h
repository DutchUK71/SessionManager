#pragma once

#include "RuleSet.h"

#include <string>
#include <vector>

class PdfTemplateResolver
{
public:
	struct Diagnostic
	{
		std::string code;
		std::string message;
		std::string path;
	};

	struct Result
	{
		bool succeeded = false;
		nlohmann::json document;
		std::vector<Diagnostic> diagnostics;

		nlohmann::json DiagnosticsJson() const;
	};

	static Result Resolve(const RuleSet& ruleSet, const std::string& templateId);
};
