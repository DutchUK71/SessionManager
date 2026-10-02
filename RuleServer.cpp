#define CPPHTTPLIB_USE_MULTIPART_FORMDATA

#include "httplib.h"
#include "json.hpp"
#include "ScreenData.h"
#include "Attribute.h"
#include "Rule.h"
#include <crtdbg.h>
#include <cstdlib>
#include <iostream>
#include "JSONFile.h"
#include "Date.h"
#include "DataManager.h"
#include "RuleSet.h"
#include "RuleServer.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>
#include <string>
#include <mutex>
#include <optional>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include "ScreenFile.h"
#include "Entity.h"
#include "Summary.h"
#include "FunctionRule.h"
#include "SessionModelImporter.h"
#include "SessionModelRuleEvaluator.h"
#include "PdfTemplateResolver.h"
#include "EmailCommandResolver.h"
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")
#endif

using Clock = std::chrono::system_clock;

using json = nlohmann::json;

ScreenFile RuleServer::ruleServerScreenFile;

JSONFile::DataModel RuleServer::model;

bool RuleServer::loaded = false;

bool RuleServer::loadedFromSessionModel = false;

RuleSet RuleServer::sessionModelRuntimeTemplate;

std::vector<SessionModelImporter::Diagnostic> RuleServer::sessionModelDiagnostics;

shared_ptr<ScreenFile::ScreenData> RuleServerScreenData()
{
	// ScreenData is the immutable template; DataManager clones returned screens before decorating them.
	static shared_ptr<ScreenFile::ScreenData> screenDataView(&RuleServer::ruleServerScreenFile.screenData, [](ScreenFile::ScreenData*) {});

	return screenDataView;
}

shared_ptr<ScreenFile::ScreenData> RuleServerScreenData(ScreenFile::ScreenData& screenData)
{
	return shared_ptr<ScreenFile::ScreenData>(&screenData, [](ScreenFile::ScreenData*) {});
}

void EnsureRuleServerScreenDataLoaded()
{
	if(!RuleServer::loaded)
	{
		RuleServer::Load();
	}

	if(RuleServer::ruleServerScreenFile.screenData.screens.empty() && !RuleServer::ruleServerScreenFile.f.empty())
	{
		RuleServer::ruleServerScreenFile.LoadFile();
	}
}

ScreenFile::ScreenData CurrentDefaultScreenDataCopy()
{
	EnsureRuleServerScreenDataLoaded();
	return RuleServer::ruleServerScreenFile.screenData;
}

std::string SerializeScreenJson(const shared_ptr<ScreenFile::Screen>& screen)
{
	if(screen == nullptr)
	{
		return "null";
	}

	ScreenFile serializer;
	return serializer.GetScreenJSON(screen);
}

std::string SerializeElementJson(const shared_ptr<ScreenFile::Element>& element)
{
	if(element == nullptr)
	{
		return "null";
	}

	ScreenFile serializer;
	return serializer.GetElementJSON(element);
}

std::string get_env_or_default(const char* name, const std::string& fallback)
{
	const char* value = std::getenv(name);

	if(value == nullptr || value[0] == '\0')
	{
		return fallback;
	}

	return value;
}

std::string FirstExistingPathOrDefault(const std::vector<std::string>& candidates, const std::string& fallback)
{
	for(const std::string& candidate : candidates)
	{
		std::ifstream input(candidate);

		if(input.is_open())
		{
			return candidate;
		}
	}

	return fallback;
}

std::string build_configuration_name()
{
#if defined(_DEBUG)
	return "Debug";
#else
	return "Release";
#endif
}

int get_env_int_or_default(const char* name, int fallback, int minValue, int maxValue)
{
	const char* value = std::getenv(name);

	if(value == nullptr || value[0] == '\0')
	{
		return fallback;
	}

	try
	{
		int parsed = std::stoi(value);

		if(parsed < minValue)
		{
			return minValue;
		}

		if(parsed > maxValue)
		{
			return maxValue;
		}

		return parsed;
	}
	catch(...)
	{
		return fallback;
	}
}

void ApplySessionContextEnvironment(RuleSet& ruleSet, bool resetSessionClock)
{
	if(resetSessionClock)
	{
		ruleSet.ResetSessionStartDateTime();
	}

	const std::string configuredSessionStart = get_env_or_default("CCP_SESSION_START_DATETIME", "");

	if(!configuredSessionStart.empty())
	{
		ruleSet.SetSessionStartDateTime(configuredSessionStart);
	}

	ruleSet.SetSessionMetadata(
		get_env_or_default("CCP_CURRENT_LOCALE", ruleSet.sessionContext.currentLocale),
		get_env_or_default("CCP_DEPLOYMENT_NAME", ruleSet.sessionContext.deploymentName),
		get_env_or_default("CCP_DEPLOYMENT_VERSION", ruleSet.sessionContext.deploymentVersion));

	const std::string configuredHistorySnapshot = get_env_or_default(
		"CCP_AS_OF_TRANSACTION_TIME",
		get_env_or_default("CCP_HISTORY_SNAPSHOT_TIME", ruleSet.sessionContext.asOfTransactionTime));

	if(!configuredHistorySnapshot.empty())
	{
		ruleSet.SetHistorySnapshotTime(configuredHistorySnapshot);
	}
}

RuleSet CloneSessionModelRuleSetTemplate(const RuleSet& source)
{
	RuleSet clone;

	if(source.entities == nullptr)
	{
		return clone;
	}

	for(const shared_ptr<Entity>& sourceEntity : *source.entities)
	{
		if(sourceEntity == nullptr)
		{
			continue;
		}

		shared_ptr<Entity> entity = make_shared<Entity>();

		entity->entityid = sourceEntity->entityid;
		entity->description = sourceEntity->description;
		entity->root = sourceEntity->root;
		entity->useRuntimeDefinitionsForInstances = sourceEntity->useRuntimeDefinitionsForInstances;

		entity->attributes.reserve(sourceEntity->attributes.size());

		for(const shared_ptr<Attribute>& sourceAttribute : sourceEntity->attributes)
		{
			if(sourceAttribute == nullptr)
			{
				continue;
			}

			shared_ptr<Attribute> attribute = make_shared<Attribute>(*sourceAttribute);

			attribute->instance = nullptr;
			entity->attributes.push_back(attribute);
		}

		entity->relationships.reserve(sourceEntity->relationships.size());

		for(const shared_ptr<Relationship>& sourceRelationship : sourceEntity->relationships)
		{
			if(sourceRelationship != nullptr)
			{
				entity->relationships.push_back(make_shared<Relationship>(*sourceRelationship));
			}
		}

		clone.entities->push_back(entity);
	}

	for(const shared_ptr<Entity>& entity : *clone.entities)
	{
		if(entity != nullptr && entity->root.value_or(false))
		{
			entity->AddInstance();
		}
	}

	if(source.sessionModelRules != nullptr)
	{
		*clone.sessionModelRules = *source.sessionModelRules;
	}

	if(source.pdfTemplates != nullptr)
	{
		*clone.pdfTemplates = *source.pdfTemplates;
	}

	if(source.emailTemplates != nullptr)
	{
		*clone.emailTemplates = *source.emailTemplates;
	}

	clone.forgetPolicyEnabled = source.forgetPolicyEnabled;
	clone.externalSecurity = source.externalSecurity;
	clone.notify = source.notify;

	if(source.attributeForgetPolicyModeByKey != nullptr)
	{
		*clone.attributeForgetPolicyModeByKey = *source.attributeForgetPolicyModeByKey;
	}

	if(source.relationshipForgetPolicyModeById != nullptr)
	{
		*clone.relationshipForgetPolicyModeById = *source.relationshipForgetPolicyModeById;
	}

	if(source.sessionModelRuleIndexById != nullptr)
	{
		*clone.sessionModelRuleIndexById = *source.sessionModelRuleIndexById;
	}
	else
	{
		clone.RebuildSessionModelRuleIndex();
	}

	if(source.relationshipDefinitionIndexById != nullptr && source.relationshipDefinitionIndexByScope != nullptr)
	{
		*clone.relationshipDefinitionIndexById = *source.relationshipDefinitionIndexById;
		*clone.relationshipDefinitionIndexByScope = *source.relationshipDefinitionIndexByScope;
		clone.relationshipDefinitionIndexEntityCount = source.relationshipDefinitionIndexEntityCount;
		clone.relationshipDefinitionIndexRelationshipCount = source.relationshipDefinitionIndexRelationshipCount;
	}
	else
	{
		clone.RebuildRelationshipDefinitionIndex();
	}

	if(source.activeSessionModelRuleIdsByReferencedAttribute != nullptr)
	{
		*clone.activeSessionModelRuleIdsByReferencedAttribute = *source.activeSessionModelRuleIdsByReferencedAttribute;
	}

	if(source.activeSessionModelRuleIdsByReferencedRelationship != nullptr)
	{
		*clone.activeSessionModelRuleIdsByReferencedRelationship = *source.activeSessionModelRuleIdsByReferencedRelationship;
	}

	if(source.activeSessionModelRuleIdsByReferencedFunction != nullptr)
	{
		*clone.activeSessionModelRuleIdsByReferencedFunction = *source.activeSessionModelRuleIdsByReferencedFunction;
	}

	if(source.activeSessionModelRuleIdsByConcludedAttribute != nullptr)
	{
		*clone.activeSessionModelRuleIdsByConcludedAttribute = *source.activeSessionModelRuleIdsByConcludedAttribute;
	}

	if(source.dataConnectionSources != nullptr)
	{
		*clone.dataConnectionSources = *source.dataConnectionSources;
	}

	if(source.dataConnectionEntityMappings != nullptr)
	{
		*clone.dataConnectionEntityMappings = *source.dataConnectionEntityMappings;
	}

	if(source.startInputs != nullptr)
	{
		*clone.startInputs = *source.startInputs;
	}

	if(source.attributeHistorySegments != nullptr)
	{
		*clone.attributeHistorySegments = *source.attributeHistorySegments;
	}

	if(source.attributeHistorySegmentIndexesByAttribute != nullptr)
	{
		*clone.attributeHistorySegmentIndexesByAttribute = *source.attributeHistorySegmentIndexesByAttribute;
	}
	else
	{
		clone.RebuildAttributeHistoryIndex();
	}

	if(source.entityExistenceHistorySegments != nullptr)
	{
		*clone.entityExistenceHistorySegments = *source.entityExistenceHistorySegments;
	}

	if(source.entityExistenceHistorySegmentIndexesByInstance != nullptr)
	{
		*clone.entityExistenceHistorySegmentIndexesByInstance = *source.entityExistenceHistorySegmentIndexesByInstance;
	}
	else
	{
		clone.RebuildEntityExistenceHistoryIndex();
	}

	if(source.relationshipLinkHistorySegments != nullptr)
	{
		*clone.relationshipLinkHistorySegments = *source.relationshipLinkHistorySegments;
	}

	if(source.relationshipLinkHistorySegmentIndexesBySource != nullptr)
	{
		*clone.relationshipLinkHistorySegmentIndexesBySource = *source.relationshipLinkHistorySegmentIndexesBySource;
	}

	if(source.relationshipLinkHistorySegmentIndexesByTarget != nullptr)
	{
		*clone.relationshipLinkHistorySegmentIndexesByTarget = *source.relationshipLinkHistorySegmentIndexesByTarget;
	}

	if(source.relationshipLinkHistorySegmentIndexesByLink != nullptr)
	{
		*clone.relationshipLinkHistorySegmentIndexesByLink = *source.relationshipLinkHistorySegmentIndexesByLink;
	}
	else
	{
		clone.RebuildRelationshipLinkHistoryIndex();
	}

	if(source.relationshipCoverageHistorySegments != nullptr)
	{
		*clone.relationshipCoverageHistorySegments = *source.relationshipCoverageHistorySegments;
	}

	if(source.relationshipCoverageHistorySegmentIndexesBySource != nullptr)
	{
		*clone.relationshipCoverageHistorySegmentIndexesBySource = *source.relationshipCoverageHistorySegmentIndexesBySource;
	}
	else
	{
		clone.RebuildRelationshipCoverageHistoryIndex();
	}

	clone.sessionContext = source.sessionContext;
	ApplySessionContextEnvironment(clone, true);

	return clone;
}

shared_ptr<RuleSet> BorrowRuleSet(RuleSet& ruleSet)
{
	return shared_ptr<RuleSet>(&ruleSet, [](RuleSet*) {});
}

void AppendDiagnostics(std::vector<SessionModelImporter::Diagnostic>& target, const std::vector<SessionModelImporter::Diagnostic>& source)
{
	target.insert(target.end(), source.begin(), source.end());
}

const char* DiagnosticSeverityText(SessionModelImporter::DiagnosticSeverity severity)
{
	switch(severity)
	{
		case SessionModelImporter::DiagnosticSeverity::Info:
			return "info";
		case SessionModelImporter::DiagnosticSeverity::Warning:
			return "warning";
		case SessionModelImporter::DiagnosticSeverity::Error:
			return "error";
		default:
			return "unknown";
	}
}

void LogSessionModelDiagnostics(const std::vector<SessionModelImporter::Diagnostic>& diagnostics)
{
	for(const SessionModelImporter::Diagnostic& diagnostic : diagnostics)
	{
		if(diagnostic.severity != SessionModelImporter::DiagnosticSeverity::Error)
		{
			continue;
		}

		std::cerr << "SessionModel " << DiagnosticSeverityText(diagnostic.severity)
			<< " [" << diagnostic.code << "] " << diagnostic.message;

		if(!diagnostic.context.empty())
		{
			std::cerr << " (" << diagnostic.context << ")";
		}

		std::cerr << std::endl;
	}
}

json SessionModelDiagnosticsToJson(const std::vector<SessionModelImporter::Diagnostic>& diagnostics)
{
	json result = json::array();

	for(const SessionModelImporter::Diagnostic& diagnostic : diagnostics)
	{
		json item = {
			{"severity", DiagnosticSeverityText(diagnostic.severity)},
			{"code", diagnostic.code},
			{"message", diagnostic.message}
		};

		if(!diagnostic.context.empty())
		{
			item["context"] = diagnostic.context;
		}

		result.push_back(item);
	}

	return result;
}

bool IsDebugBlockingSessionModelDiagnostic(const SessionModelImporter::Diagnostic& diagnostic)
{
	if(diagnostic.severity == SessionModelImporter::DiagnosticSeverity::Error)
	{
		return true;
	}

	static const std::unordered_set<std::string> blockingWarningCodes = {
		"attribute_invalid",
		"attribute_missing_type",
		"decision_table_missing_executable",
		"entity_invalid",
		"relationship_invalid",
		"relationship_reference_missing",
		"rule_conclusion_attribute_missing",
		"rule_conclusion_entity_missing",
		"rule_invalid",
		"rule_reference_attribute_missing",
		"rule_reference_entity_missing",
		"unsupported_attribute_type"
	};

	return blockingWarningCodes.find(diagnostic.code) != blockingWarningCodes.end();
}

int DebugBlockingSessionModelDiagnosticCount(const std::vector<SessionModelImporter::Diagnostic>& diagnostics)
{
	return static_cast<int>(std::count_if(
		diagnostics.begin(),
		diagnostics.end(),
		[](const SessionModelImporter::Diagnostic& diagnostic) {
			return IsDebugBlockingSessionModelDiagnostic(diagnostic);
		}));
}

bool EndpointMetricsEnabled()
{
	const char* value = std::getenv("CCP_ENDPOINT_METRICS");

	if(value == nullptr || value[0] == '\0')
	{
		return true;
	}

	std::string normalized = value;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});

	return normalized != "0" && normalized != "false" && normalized != "off" && normalized != "no";
}

int EffectiveEndpointStatusCode(const httplib::Response& response)
{
	return response.status > 0 ? response.status : 200;
}

class EndpointMetricsScope
{
public:
	EndpointMetricsScope(const char* endpointName, const httplib::Request& request, const httplib::Response& response)
		: endpoint(endpointName),
		  requestBytes(request.body.size()),
		  response(response),
		  startedAt(std::chrono::steady_clock::now()),
		  enabled(EndpointMetricsEnabled())
	{
	}

	~EndpointMetricsScope() noexcept
	{
		if(!enabled)
		{
			return;
		}

		try
		{
			const auto elapsed = std::chrono::steady_clock::now() - startedAt;
			const auto elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

			std::cerr << "endpointMetrics endpoint=" << endpoint
				<< " status=" << EffectiveEndpointStatusCode(response)
				<< " requestBytes=" << requestBytes
				<< " responseBytes=" << response.body.size()
				<< " elapsedMicros=" << elapsedMicros
				<< std::endl;
		}
		catch(...)
		{
		}
	}

private:
	const char* endpoint;
	std::size_t requestBytes;
	const httplib::Response& response;
	std::chrono::steady_clock::time_point startedAt;
	bool enabled;
};

bool SelectedFormLoadMetricsEnabled()
{
	const char* value = std::getenv("CCP_SELECTED_FORM_LOAD_METRICS");

	if(value == nullptr || value[0] == '\0')
	{
		return false;
	}

	std::string normalized = value;
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});

	return normalized != "0" && normalized != "false" && normalized != "off" && normalized != "no";
}

void WriteSelectedFormLoadMetric(const std::string& stage, long long elapsedMicros)
{
	if(!SelectedFormLoadMetricsEnabled())
	{
		return;
	}

	std::cerr << "selectedFormLoadMetrics stage=" << stage
		<< " elapsedMicros=" << elapsedMicros
		<< std::endl;
}

class SelectedFormLoadMetricScope
{
public:
	explicit SelectedFormLoadMetricScope(std::string stageName)
		: stage(std::move(stageName)),
		  startedAt(std::chrono::steady_clock::now())
	{
	}

	~SelectedFormLoadMetricScope() noexcept
	{
		try
		{
			const auto elapsed = std::chrono::steady_clock::now() - startedAt;
			WriteSelectedFormLoadMetric(stage, std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
		}
		catch(...)
		{
		}
	}

private:
	std::string stage;
	std::chrono::steady_clock::time_point startedAt;
};

struct EndpointResponseOptions
{
	bool includeUiState = false;
	bool prettyJson = false;
};

std::string NormalizeOptionValue(std::string value)
{
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
		return std::isspace(ch) == 0;
	}));
	value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
		return std::isspace(ch) == 0;
	}).base(), value.end());
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});

	return value;
}

std::optional<bool> BooleanOptionFromString(const std::string& value)
{
	const std::string normalized = NormalizeOptionValue(value);

	if(normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on")
	{
		return true;
	}

	if(normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off")
	{
		return false;
	}

	return std::nullopt;
}

std::optional<bool> BooleanOptionFromJson(const json& value)
{
	if(value.is_boolean())
	{
		return value.get<bool>();
	}

	if(value.is_number_integer())
	{
		return value.get<int>() != 0;
	}

	if(value.is_string())
	{
		return BooleanOptionFromString(value.get<std::string>());
	}

	return std::nullopt;
}

std::optional<bool> EnvironmentBooleanOption(const char* name)
{
	const char* value = std::getenv(name);

	if(value == nullptr || value[0] == '\0')
	{
		return std::nullopt;
	}

	return BooleanOptionFromString(value);
}

std::optional<bool> RequestBooleanOption(const httplib::Request& request,
                                         const json* bodyJson,
                                         std::initializer_list<const char*> bodyOrQueryKeys,
                                         std::initializer_list<const char*> headerKeys,
                                         const char* environmentName)
{
	if(bodyJson != nullptr && bodyJson->is_object())
	{
		for(const char* key : bodyOrQueryKeys)
		{
			if(bodyJson->contains(key) && !bodyJson->at(key).is_null())
			{
				std::optional<bool> parsed = BooleanOptionFromJson(bodyJson->at(key));

				if(parsed.has_value())
				{
					return parsed;
				}
			}
		}
	}

	for(const char* key : bodyOrQueryKeys)
	{
		if(request.has_param(key))
		{
			std::optional<bool> parsed = BooleanOptionFromString(request.get_param_value(key));

			if(parsed.has_value())
			{
				return parsed;
			}
		}
	}

	for(const char* key : headerKeys)
	{
		const std::string value = request.get_header_value(key);

		if(!value.empty())
		{
			std::optional<bool> parsed = BooleanOptionFromString(value);

			if(parsed.has_value())
			{
				return parsed;
			}
		}
	}

	return EnvironmentBooleanOption(environmentName);
}

EndpointResponseOptions BuildEndpointResponseOptions(const httplib::Request& request, const json* bodyJson = nullptr)
{
	const bool debugResponse = RequestBooleanOption(
		request,
		bodyJson,
		{"debug", "debugResponse", "debugPayload"},
		{"X-Debug-Response"},
		"CCP_DEBUG_RESPONSES").value_or(false);

	EndpointResponseOptions options;
	options.includeUiState = debugResponse || RequestBooleanOption(
		request,
		bodyJson,
		{"includeUiState", "includeUIState", "include_ui_state", "uiState"},
		{"X-Include-Ui-State"},
		"CCP_INCLUDE_UI_STATE").value_or(false);
	options.prettyJson = debugResponse || RequestBooleanOption(
		request,
		bodyJson,
		{"prettyJson", "debugJson", "pretty", "indentJson"},
		{"X-Pretty-Json", "X-Debug-Json"},
		"CCP_RESPONSE_PRETTY_JSON").value_or(false);

	return options;
}

std::string SerializeEndpointJson(const json& value, const EndpointResponseOptions& options)
{
	return options.prettyJson ? value.dump(2) : value.dump();
}

void WriteEndpointJsonResponse(httplib::Response& response, const json& value, const EndpointResponseOptions& options)
{
	response.set_content(SerializeEndpointJson(value, options), "application/json");
}

std::string ConfigString(const json& object, const char* key)
{
	if(!object.is_object() || !object.contains(key) || object.at(key).is_null())
	{
		return "";
	}

	const json& value = object.at(key);

	if(value.is_string())
	{
		return value.get<std::string>();
	}

	return "";
}

std::string JoinConfigPath(std::string directory, const std::string& fileName)
{
	if(directory.empty())
	{
		return fileName;
	}

	if(fileName.empty())
	{
		return directory;
	}

	std::replace(directory.begin(), directory.end(), '\\', '/');

	if(directory.back() != '/')
	{
		directory.push_back('/');
	}

	return directory + fileName;
}

std::string SessionModelPathFromConfig(const json& config)
{
	const std::string directPath = ConfigString(config, "sessionModelPath");

	if(!directPath.empty())
	{
		return directPath;
	}

	if(config.is_object() && config.contains("sessionModel") && config.at("sessionModel").is_object())
	{
		const json& sessionModel = config.at("sessionModel");
		const std::string nestedPath = ConfigString(sessionModel, "path");

		if(!nestedPath.empty())
		{
			return nestedPath;
		}

		const std::string directory = ConfigString(sessionModel, "directory");
		const std::string fileName = ConfigString(sessionModel, "fileName");
		const std::string combined = JoinConfigPath(directory, fileName);

		if(!combined.empty())
		{
			return combined;
		}
	}

	return ConfigString(config, "modelPath");
}

constexpr const char* DataConnectionSavePlanDryRunPath = "/api/data-connections/save-plan/dry-run";
constexpr const char* DataConnectionSavePlanExecutePath = "/api/data-connections/save-plan/execute";
constexpr const char* DataConnectionLoadPlanDryRunPath = "/api/data-connections/load-plan/dry-run";
constexpr const char* DataConnectionLoadPlanExecutePath = "/api/data-connections/load-plan/execute";
constexpr const char* DataConnectionDeletePlanDryRunPath = "/api/data-connections/delete-plan/dry-run";
constexpr const char* DataConnectionDeletePlanExecutePath = "/api/data-connections/delete-plan/execute";
constexpr const char* DefaultDataConnectionSavePlanDryRunUrl = "http://127.0.0.1:5129/api/data-connections/save-plan/dry-run";
constexpr const char* DefaultDataConnectionSavePlanExecuteUrl = "http://127.0.0.1:5129/api/data-connections/save-plan/execute";
constexpr const char* DefaultDataConnectionLoadPlanDryRunUrl = "http://127.0.0.1:5129/api/data-connections/load-plan/dry-run";
constexpr const char* DefaultDataConnectionLoadPlanExecuteUrl = "http://127.0.0.1:5129/api/data-connections/load-plan/execute";
constexpr const char* DefaultDataConnectionDeletePlanDryRunUrl = "http://127.0.0.1:5129/api/data-connections/delete-plan/dry-run";
constexpr const char* DefaultDataConnectionDeletePlanExecuteUrl = "http://127.0.0.1:5129/api/data-connections/delete-plan/execute";

std::string TrimAscii(std::string value)
{
	const auto first = std::find_if(value.begin(), value.end(), [](unsigned char ch)
	{
		return std::isspace(ch) == 0;
	});

	if(first == value.end())
	{
		return "";
	}

	const auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch)
	{
		return std::isspace(ch) == 0;
	}).base();

	return std::string(first, last);
}

std::string AppendDataConnectionSavePlanPath(std::string baseUrl, const char* path)
{
	baseUrl = TrimAscii(std::move(baseUrl));

	if(baseUrl.empty())
	{
		return "";
	}

	while(!baseUrl.empty() && baseUrl.back() == '/')
	{
		baseUrl.pop_back();
	}

	return baseUrl + path;
}

std::string AppendDataConnectionSavePlanDryRunPath(std::string baseUrl)
{
	return AppendDataConnectionSavePlanPath(std::move(baseUrl), DataConnectionSavePlanDryRunPath);
}

std::string AppendDataConnectionSavePlanExecutePath(std::string baseUrl)
{
	return AppendDataConnectionSavePlanPath(std::move(baseUrl), DataConnectionSavePlanExecutePath);
}

std::string AppendDataConnectionLoadPlanDryRunPath(std::string baseUrl)
{
	return AppendDataConnectionSavePlanPath(std::move(baseUrl), DataConnectionLoadPlanDryRunPath);
}

std::string AppendDataConnectionLoadPlanExecutePath(std::string baseUrl)
{
	return AppendDataConnectionSavePlanPath(std::move(baseUrl), DataConnectionLoadPlanExecutePath);
}

std::string AppendDataConnectionDeletePlanDryRunPath(std::string baseUrl)
{
	return AppendDataConnectionSavePlanPath(std::move(baseUrl), DataConnectionDeletePlanDryRunPath);
}

std::string AppendDataConnectionDeletePlanExecutePath(std::string baseUrl)
{
	return AppendDataConnectionSavePlanPath(std::move(baseUrl), DataConnectionDeletePlanExecutePath);
}

std::string FirstConfigString(const json& object, std::initializer_list<const char*> keys)
{
	for(const char* key : keys)
	{
		const std::string value = TrimAscii(ConfigString(object, key));

		if(!value.empty())
		{
			return value;
		}
	}

	return "";
}

std::string DataConnectionSavePlanDryRunUrlFromConfig(const json& config)
{
	const std::string direct = FirstConfigString(config, {
		"dataConnectionSavePlanDryRunUrl",
		"dataConnectionDryRunUrl",
		"dataConnectionDryRunEndpoint"
	});

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = FirstConfigString(config, {
		"dataConnectionApiBaseUrl",
		"dataConnectionBaseUrl"
	});

	if(!base.empty())
	{
		return AppendDataConnectionSavePlanDryRunPath(base);
	}

	if(config.is_object() && config.contains("dataConnections") && config.at("dataConnections").is_object())
	{
		const json& dataConnections = config.at("dataConnections");
		const std::string nestedDirect = FirstConfigString(dataConnections, {
			"savePlanDryRunUrl",
			"dryRunUrl",
			"dryRunEndpoint"
		});

		if(!nestedDirect.empty())
		{
			return nestedDirect;
		}

		const std::string nestedBase = FirstConfigString(dataConnections, {
			"apiBaseUrl",
			"baseUrl"
		});

		if(!nestedBase.empty())
		{
			return AppendDataConnectionSavePlanDryRunPath(nestedBase);
		}
	}

	return "";
}

std::string DataConnectionSavePlanExecuteUrlFromConfig(const json& config)
{
	const std::string direct = FirstConfigString(config, {
		"dataConnectionSavePlanExecuteUrl",
		"dataConnectionSavePlanExecutionUrl",
		"dataConnectionExecuteUrl",
		"dataConnectionExecutionEndpoint"
	});

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = FirstConfigString(config, {
		"dataConnectionApiBaseUrl",
		"dataConnectionBaseUrl"
	});

	if(!base.empty())
	{
		return AppendDataConnectionSavePlanExecutePath(base);
	}

	if(config.is_object() && config.contains("dataConnections") && config.at("dataConnections").is_object())
	{
		const json& dataConnections = config.at("dataConnections");
		const std::string nestedDirect = FirstConfigString(dataConnections, {
			"savePlanExecuteUrl",
			"savePlanExecutionUrl",
			"executeUrl",
			"executionEndpoint"
		});

		if(!nestedDirect.empty())
		{
			return nestedDirect;
		}

		const std::string nestedBase = FirstConfigString(dataConnections, {
			"apiBaseUrl",
			"baseUrl"
		});

		if(!nestedBase.empty())
		{
			return AppendDataConnectionSavePlanExecutePath(nestedBase);
		}
	}

	return "";
}

std::string DataConnectionLoadPlanDryRunUrlFromConfig(const json& config)
{
	const std::string direct = FirstConfigString(config, {
		"dataConnectionLoadPlanDryRunUrl",
		"dataConnectionLoadDryRunUrl",
		"dataConnectionLoadDryRunEndpoint"
	});

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = FirstConfigString(config, {
		"dataConnectionApiBaseUrl",
		"dataConnectionBaseUrl"
	});

	if(!base.empty())
	{
		return AppendDataConnectionLoadPlanDryRunPath(base);
	}

	if(config.is_object() && config.contains("dataConnections") && config.at("dataConnections").is_object())
	{
		const json& dataConnections = config.at("dataConnections");
		const std::string nestedDirect = FirstConfigString(dataConnections, {
			"loadPlanDryRunUrl",
			"loadDryRunUrl",
			"loadDryRunEndpoint"
		});

		if(!nestedDirect.empty())
		{
			return nestedDirect;
		}

		const std::string nestedBase = FirstConfigString(dataConnections, {
			"apiBaseUrl",
			"baseUrl"
		});

		if(!nestedBase.empty())
		{
			return AppendDataConnectionLoadPlanDryRunPath(nestedBase);
		}
	}

	return "";
}

std::string DataConnectionLoadPlanExecuteUrlFromConfig(const json& config)
{
	const std::string direct = FirstConfigString(config, {
		"dataConnectionLoadPlanExecuteUrl",
		"dataConnectionLoadPlanExecutionUrl",
		"dataConnectionLoadExecuteUrl",
		"dataConnectionLoadExecutionUrl",
		"dataConnectionLoadExecutionEndpoint"
	});

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = FirstConfigString(config, {
		"dataConnectionApiBaseUrl",
		"dataConnectionBaseUrl"
	});

	if(!base.empty())
	{
		return AppendDataConnectionLoadPlanExecutePath(base);
	}

	if(config.is_object() && config.contains("dataConnections") && config.at("dataConnections").is_object())
	{
		const json& dataConnections = config.at("dataConnections");
		const std::string nestedDirect = FirstConfigString(dataConnections, {
			"loadPlanExecuteUrl",
			"loadPlanExecutionUrl",
			"loadExecuteUrl",
			"loadExecutionUrl",
			"loadExecutionEndpoint"
		});

		if(!nestedDirect.empty())
		{
			return nestedDirect;
		}

		const std::string nestedBase = FirstConfigString(dataConnections, {
			"apiBaseUrl",
			"baseUrl"
		});

		if(!nestedBase.empty())
		{
			return AppendDataConnectionLoadPlanExecutePath(nestedBase);
		}
	}

	return "";
}

std::string DataConnectionDeletePlanDryRunUrlFromConfig(const json& config)
{
	const std::string direct = FirstConfigString(config, {
		"dataConnectionDeletePlanDryRunUrl",
		"dataConnectionDeleteDryRunUrl",
		"dataConnectionDeleteDryRunEndpoint"
	});

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = FirstConfigString(config, {
		"dataConnectionApiBaseUrl",
		"dataConnectionBaseUrl"
	});

	if(!base.empty())
	{
		return AppendDataConnectionDeletePlanDryRunPath(base);
	}

	if(config.is_object() && config.contains("dataConnections") && config.at("dataConnections").is_object())
	{
		const json& dataConnections = config.at("dataConnections");
		const std::string nestedDirect = FirstConfigString(dataConnections, {
			"deletePlanDryRunUrl",
			"deleteDryRunUrl",
			"deleteDryRunEndpoint"
		});

		if(!nestedDirect.empty())
		{
			return nestedDirect;
		}

		const std::string nestedBase = FirstConfigString(dataConnections, {
			"apiBaseUrl",
			"baseUrl"
		});

		if(!nestedBase.empty())
		{
			return AppendDataConnectionDeletePlanDryRunPath(nestedBase);
		}
	}

	return "";
}

std::string DataConnectionDeletePlanExecuteUrlFromConfig(const json& config)
{
	const std::string direct = FirstConfigString(config, {
		"dataConnectionDeletePlanExecuteUrl",
		"dataConnectionDeletePlanExecutionUrl",
		"dataConnectionDeleteExecuteUrl",
		"dataConnectionDeleteExecutionUrl",
		"dataConnectionDeleteExecutionEndpoint"
	});

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = FirstConfigString(config, {
		"dataConnectionApiBaseUrl",
		"dataConnectionBaseUrl"
	});

	if(!base.empty())
	{
		return AppendDataConnectionDeletePlanExecutePath(base);
	}

	if(config.is_object() && config.contains("dataConnections") && config.at("dataConnections").is_object())
	{
		const json& dataConnections = config.at("dataConnections");
		const std::string nestedDirect = FirstConfigString(dataConnections, {
			"deletePlanExecuteUrl",
			"deletePlanExecutionUrl",
			"deleteExecuteUrl",
			"deleteExecutionUrl",
			"deleteExecutionEndpoint"
		});

		if(!nestedDirect.empty())
		{
			return nestedDirect;
		}

		const std::string nestedBase = FirstConfigString(dataConnections, {
			"apiBaseUrl",
			"baseUrl"
		});

		if(!nestedBase.empty())
		{
			return AppendDataConnectionDeletePlanExecutePath(nestedBase);
		}
	}

	return "";
}

std::string ResolveRuleServerConfigPath()
{
	const std::string configuredPath = get_env_or_default("CCP_SERVER_CONFIG_JSON", "");

	if(!configuredPath.empty())
	{
		std::ifstream configured(configuredPath);

		if(!configured.is_open())
		{
			throw std::runtime_error("RuleServer config file not found: " + configuredPath);
		}

		return configuredPath;
	}

	const std::vector<std::string> candidates = {
		"RuleServer.config.json",
		"config/session-manager.local.json",
		"../../../config/session-manager.local.json"
	};

	for(const std::string& candidate : candidates)
	{
		std::ifstream input(candidate);

		if(input.is_open())
		{
			return candidate;
		}
	}

	return "";
}

std::string ResolveDataConnectionSavePlanDryRunUrl()
{
	const std::string direct = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_DRY_RUN_URL", ""));

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_API_BASE_URL", ""));

	if(!base.empty())
	{
		return AppendDataConnectionSavePlanDryRunPath(base);
	}

	const std::string configPath = ResolveRuleServerConfigPath();

	if(!configPath.empty())
	{
		std::ifstream input(configPath);

		if(input.is_open())
		{
			json config;
			input >> config;

			const std::string configuredUrl = DataConnectionSavePlanDryRunUrlFromConfig(config);

			if(!configuredUrl.empty())
			{
				return configuredUrl;
			}
		}
	}

	return DefaultDataConnectionSavePlanDryRunUrl;
}

std::string ResolveDataConnectionSavePlanExecuteUrl()
{
	const std::string direct = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_EXECUTE_URL", ""));

	if(!direct.empty())
	{
		return direct;
	}

	const std::string alternateDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_EXECUTION_URL", ""));

	if(!alternateDirect.empty())
	{
		return alternateDirect;
	}

	const std::string base = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_API_BASE_URL", ""));

	if(!base.empty())
	{
		return AppendDataConnectionSavePlanExecutePath(base);
	}

	const std::string configPath = ResolveRuleServerConfigPath();

	if(!configPath.empty())
	{
		std::ifstream input(configPath);

		if(input.is_open())
		{
			json config;
			input >> config;

			const std::string configuredUrl = DataConnectionSavePlanExecuteUrlFromConfig(config);

			if(!configuredUrl.empty())
			{
				return configuredUrl;
			}
		}
	}

	return DefaultDataConnectionSavePlanExecuteUrl;
}

std::string ResolveDataConnectionLoadPlanDryRunUrl()
{
	const std::string direct = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_LOAD_DRY_RUN_URL", ""));

	if(!direct.empty())
	{
		return direct;
	}

	const std::string base = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_API_BASE_URL", ""));

	if(!base.empty())
	{
		return AppendDataConnectionLoadPlanDryRunPath(base);
	}

	const std::string configPath = ResolveRuleServerConfigPath();

	if(!configPath.empty())
	{
		std::ifstream input(configPath);

		if(input.is_open())
		{
			json config;
			input >> config;

			const std::string configuredUrl = DataConnectionLoadPlanDryRunUrlFromConfig(config);

			if(!configuredUrl.empty())
			{
				return configuredUrl;
			}
		}
	}

	return DefaultDataConnectionLoadPlanDryRunUrl;
}

std::string ResolveDataConnectionLoadPlanExecuteUrl()
{
	const std::string direct = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_LOAD_EXECUTE_URL", ""));

	if(!direct.empty())
	{
		return direct;
	}

	const std::string alternateDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_LOAD_EXECUTION_URL", ""));

	if(!alternateDirect.empty())
	{
		return alternateDirect;
	}

	const std::string loadPlanDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_LOAD_PLAN_EXECUTE_URL", ""));

	if(!loadPlanDirect.empty())
	{
		return loadPlanDirect;
	}

	const std::string loadPlanAlternateDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_LOAD_PLAN_EXECUTION_URL", ""));

	if(!loadPlanAlternateDirect.empty())
	{
		return loadPlanAlternateDirect;
	}

	const std::string base = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_API_BASE_URL", ""));

	if(!base.empty())
	{
		return AppendDataConnectionLoadPlanExecutePath(base);
	}

	const std::string configPath = ResolveRuleServerConfigPath();

	if(!configPath.empty())
	{
		std::ifstream input(configPath);

		if(input.is_open())
		{
			json config;
			input >> config;

			const std::string configuredUrl = DataConnectionLoadPlanExecuteUrlFromConfig(config);

			if(!configuredUrl.empty())
			{
				return configuredUrl;
			}
		}
	}

	return DefaultDataConnectionLoadPlanExecuteUrl;
}

std::string ResolveDataConnectionDeletePlanDryRunUrl()
{
	const std::string direct = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_DELETE_DRY_RUN_URL", ""));

	if(!direct.empty())
	{
		return direct;
	}

	const std::string deletePlanDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_DELETE_PLAN_DRY_RUN_URL", ""));

	if(!deletePlanDirect.empty())
	{
		return deletePlanDirect;
	}

	const std::string base = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_API_BASE_URL", ""));

	if(!base.empty())
	{
		return AppendDataConnectionDeletePlanDryRunPath(base);
	}

	const std::string configPath = ResolveRuleServerConfigPath();

	if(!configPath.empty())
	{
		std::ifstream input(configPath);

		if(input.is_open())
		{
			json config;
			input >> config;

			const std::string configuredUrl = DataConnectionDeletePlanDryRunUrlFromConfig(config);

			if(!configuredUrl.empty())
			{
				return configuredUrl;
			}
		}
	}

	return DefaultDataConnectionDeletePlanDryRunUrl;
}

std::string ResolveDataConnectionDeletePlanExecuteUrl()
{
	const std::string direct = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_DELETE_EXECUTE_URL", ""));

	if(!direct.empty())
	{
		return direct;
	}

	const std::string alternateDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_DELETE_EXECUTION_URL", ""));

	if(!alternateDirect.empty())
	{
		return alternateDirect;
	}

	const std::string deletePlanDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_DELETE_PLAN_EXECUTE_URL", ""));

	if(!deletePlanDirect.empty())
	{
		return deletePlanDirect;
	}

	const std::string deletePlanAlternateDirect = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_DELETE_PLAN_EXECUTION_URL", ""));

	if(!deletePlanAlternateDirect.empty())
	{
		return deletePlanAlternateDirect;
	}

	const std::string base = TrimAscii(get_env_or_default("CCP_DATA_CONNECTION_API_BASE_URL", ""));

	if(!base.empty())
	{
		return AppendDataConnectionDeletePlanExecutePath(base);
	}

	const std::string configPath = ResolveRuleServerConfigPath();

	if(!configPath.empty())
	{
		std::ifstream input(configPath);

		if(input.is_open())
		{
			json config;
			input >> config;

			const std::string configuredUrl = DataConnectionDeletePlanExecuteUrlFromConfig(config);

			if(!configuredUrl.empty())
			{
				return configuredUrl;
			}
		}
	}

	return DefaultDataConnectionDeletePlanExecuteUrl;
}

std::string ResolveRuleServerModelPath()
{
	const std::string configPath = ResolveRuleServerConfigPath();

	if(!configPath.empty())
	{
		std::ifstream input(configPath);

		if(!input.is_open())
		{
			throw std::runtime_error("RuleServer config file not found: " + configPath);
		}

		json config;
		input >> config;

		const std::string configuredModelPath = SessionModelPathFromConfig(config);

		if(!configuredModelPath.empty())
		{
			return configuredModelPath;
		}
	}

	const std::string environmentPath = get_env_or_default("CCP_MODEL_JSON", "");

	if(!environmentPath.empty())
	{
		return environmentPath;
	}

	return FirstExistingPathOrDefault({ "samples/SImpleTest.json", "../../../samples/SImpleTest.json" }, "samples/SImpleTest.json");
}

RuleServer::RuleServer()
{
	ruleServerScreenFile = ScreenFile();
}

RuleServer::~RuleServer()
{
}

struct RuleServerModelLoadResult
{
	ScreenFile screenFile;
	JSONFile::DataModel model;
	bool loadedFromSessionModel{false};
	RuleSet sessionModelRuntimeTemplate;
	std::vector<SessionModelImporter::Diagnostic> sessionModelDiagnostics;
};

RuleServerModelLoadResult LoadRuleServerModelFromSessionModelJsonRoot(const json& root)
{
	RuleServerModelLoadResult result;

	if(!SessionModelImporter::LooksLikeSessionModel(root))
	{
		throw std::runtime_error("SessionModel JSON payload is not a valid Session Model shape.");
	}

	SessionModelImporter importer;
	SessionModelImporter::ImportResult importResult = importer.LoadJson(root);
	SessionModelImporter::RuntimeConversionResult runtimeResult;

	AppendDiagnostics(result.sessionModelDiagnostics, importResult.diagnostics);

	if(!importResult.HasErrors())
	{
		runtimeResult = importer.ConvertToRuntimeRuleSet(importResult.model);

		AppendDiagnostics(result.sessionModelDiagnostics, runtimeResult.diagnostics);
	}

	LogSessionModelDiagnostics(result.sessionModelDiagnostics);

	if(importResult.HasErrors() || runtimeResult.HasErrors())
	{
		throw std::runtime_error("SessionModel import failed. Review SessionModel diagnostics for details.");
	}

	ApplySessionContextEnvironment(runtimeResult.ruleSet, false);
	result.sessionModelRuntimeTemplate = runtimeResult.ruleSet;
	result.screenFile = ScreenFile();
	result.screenFile.screenData = root.get<ScreenFile::ScreenData>();
	result.model = JSONFile::DataModel();
	result.loadedFromSessionModel = true;

	return result;
}

RuleServerModelLoadResult LoadRuleServerModelFromSessionModelJsonText(const std::string& jsonText)
{
	json root = json::parse(jsonText);
	return LoadRuleServerModelFromSessionModelJsonRoot(root);
}

RuleServerModelLoadResult LoadRuleServerModelFromConfiguredPath()
{
	RuleServerModelLoadResult result;

	const std::string modelPath = ResolveRuleServerModelPath();

	std::ifstream input(modelPath);

	if(input.is_open())
	{
		json root;

		input >> root;

		if(SessionModelImporter::LooksLikeSessionModel(root))
		{
			return LoadRuleServerModelFromSessionModelJsonRoot(root);
		}
	}

	JSONFile jf = JSONFile(modelPath);

	result.model = jf.LoadFile();
	result.screenFile = ScreenFile();
	result.screenFile.f = get_env_or_default("CCP_SCREEN_JSON", FirstExistingPathOrDefault({ "screen.json", "../../../samples/screen.json" }, "screen.json"));

	return result;
}

void RuleServer::Load()
{
	RuleServerModelLoadResult result = LoadRuleServerModelFromConfiguredPath();

	sessionModelDiagnostics = result.sessionModelDiagnostics;
	sessionModelRuntimeTemplate = result.sessionModelRuntimeTemplate;
	ruleServerScreenFile = result.screenFile;
	model = result.model;
	loadedFromSessionModel = result.loadedFromSessionModel;
	loaded = true;
}

bool RuleServer::IsSessionModelLoaded()
{
	return loadedFromSessionModel;
}

const std::vector<SessionModelImporter::Diagnostic>& RuleServer::GetSessionModelDiagnostics()
{
	return sessionModelDiagnostics;
}

json EvaluationTraceNodeToJSON(const SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode& node)
{
	json children = json::array();

	for(const SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode& child : node.children)
	{
		children.push_back(EvaluationTraceNodeToJSON(child));
	}

	json out = {
		{"kind", node.kind},
		{"label", node.label},
		{"detail", node.detail},
		{"knowledgeState", node.knowledgeState},
		{"children", children}
	};

	if(!node.ruleId.empty())
	{
		out["ruleId"] = node.ruleId;
	}

	if(!node.entityId.empty())
	{
		out["entityId"] = node.entityId;
	}

	if(!node.instanceId.empty())
	{
		out["instanceId"] = node.instanceId;
	}

	if(!node.attributeId.empty())
	{
		out["attributeId"] = node.attributeId;
	}

	if(!node.relationshipId.empty())
	{
		out["relationshipId"] = node.relationshipId;
	}

	return out;
}

json RuleServer::SessionModelEvaluationStatsToJSON(const SessionModelRuleEvaluator::EvaluationStats& stats)
{
	json diagnostics = json::array();
	json trace = json::array();
	json inferredDiagnostics = json::array();
	json createdInstances = json::array();
	json reusedInstances = json::array();
	json removedInstances = json::array();
	json createdLinks = json::array();
	json reusedLinks = json::array();
	json removedLinks = json::array();
	json validationDiagnostics = json::array();
	json createdValidationEvents = json::array();
	json reusedValidationEvents = json::array();
	json updatedValidationEvents = json::array();
	json clearedValidationEvents = json::array();

	auto validationEventToJson = [](const SessionModelRuleEvaluator::EvaluationStats::ValidationEventChange& change) {
		return json{
			{"eventId", change.eventId},
			{"severity", change.severity},
			{"ruleId", change.ruleId},
			{"ruleName", change.ruleName},
			{"entityId", change.entityId},
			{"instanceId", change.instanceId},
			{"message", change.message},
			{"messageKnown", change.messageKnown},
			{"messageKnowledgeState", change.messageKnowledgeState}
		};
	};

	for(const SessionModelRuleEvaluator::EvaluationDiagnostic& diagnostic : stats.diagnostics)
	{
		json diagnosticJson = {
			{"ruleId", diagnostic.ruleId},
			{"code", diagnostic.code},
			{"message", diagnostic.message},
			{"context", diagnostic.context}
		};

		diagnostics.push_back(diagnosticJson);

		if(diagnostic.code.rfind("infer_instance_", 0) == 0
			|| diagnostic.code.rfind("decision_table_relationship_", 0) == 0
			|| diagnostic.code == "unsupported_infer_instance_shape")
		{
			inferredDiagnostics.push_back(diagnosticJson);
		}

		if(diagnostic.code.rfind("validation_", 0) == 0 || diagnostic.code == "unsupported_validation_shape")
		{
			validationDiagnostics.push_back(diagnosticJson);
		}
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode& node : stats.trace)
	{
		trace.push_back(EvaluationTraceNodeToJSON(node));
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange& change : stats.createdInferredInstances)
	{
		createdInstances.push_back({
			{"instanceId", change.instanceId},
			{"entityId", change.entityId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"drivingInstanceId", change.drivingInstanceId},
			{"relationshipId", change.relationshipId},
			{"createdByRuleId", change.createdByRuleId}
		});
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange& change : stats.reusedInferredInstances)
	{
		reusedInstances.push_back({
			{"instanceId", change.instanceId},
			{"entityId", change.entityId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"drivingInstanceId", change.drivingInstanceId},
			{"relationshipId", change.relationshipId},
			{"createdByRuleId", change.createdByRuleId}
		});
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange& change : stats.removedInferredInstances)
	{
		removedInstances.push_back({
			{"instanceId", change.instanceId},
			{"entityId", change.entityId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"drivingInstanceId", change.drivingInstanceId},
			{"relationshipId", change.relationshipId},
			{"createdByRuleId", change.createdByRuleId}
		});
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange& change : stats.createdInferredLinks)
	{
		createdLinks.push_back({
			{"relationshipId", change.relationshipId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"targetInstanceId", change.targetInstanceId},
			{"createdByRuleId", change.createdByRuleId}
		});
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange& change : stats.reusedInferredLinks)
	{
		reusedLinks.push_back({
			{"relationshipId", change.relationshipId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"targetInstanceId", change.targetInstanceId},
			{"createdByRuleId", change.createdByRuleId}
		});
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange& change : stats.removedInferredLinks)
	{
		removedLinks.push_back({
			{"relationshipId", change.relationshipId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"targetInstanceId", change.targetInstanceId},
			{"createdByRuleId", change.createdByRuleId}
		});
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::ValidationEventChange& change : stats.createdValidationEvents)
	{
		createdValidationEvents.push_back(validationEventToJson(change));
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::ValidationEventChange& change : stats.reusedValidationEvents)
	{
		reusedValidationEvents.push_back(validationEventToJson(change));
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::ValidationEventChange& change : stats.updatedValidationEvents)
	{
		updatedValidationEvents.push_back(validationEventToJson(change));
	}

	for(const SessionModelRuleEvaluator::EvaluationStats::ValidationEventChange& change : stats.clearedValidationEvents)
	{
		clearedValidationEvents.push_back(validationEventToJson(change));
	}

	json out = {
		{"queuedAttributes", stats.queuedAttributes},
		{"queuedRelationships", stats.queuedRelationships},
		{"candidateRules", stats.candidateRules},
		{"evaluatedRules", stats.evaluatedRules},
		{"appliedConclusions", stats.appliedConclusions},
		{"skippedRules", stats.skippedRules},
		{"unsupportedRules", stats.unsupportedRules},
		{"maxDepthStops", stats.maxDepthStops},
		{"inferredInstancesCreated", stats.inferredInstancesCreated},
		{"inferredInstancesReused", stats.inferredInstancesReused},
		{"inferredInstancesRemoved", stats.inferredInstancesRemoved},
		{"inferredLinksCreated", stats.inferredLinksCreated},
		{"inferredLinksReused", stats.inferredLinksReused},
		{"inferredLinksRemoved", stats.inferredLinksRemoved},
		{"validationEventsCreated", stats.validationEventsCreated},
		{"validationEventsReused", stats.validationEventsReused},
		{"validationEventsUpdated", stats.validationEventsUpdated},
		{"validationEventsCleared", stats.validationEventsCleared},
		{"validationErrors", stats.validationErrors},
		{"validationWarnings", stats.validationWarnings},
		{"inferredChanges", {
			{"createdInstances", createdInstances},
			{"reusedInstances", reusedInstances},
			{"removedInstances", removedInstances},
			{"createdLinks", createdLinks},
			{"reusedLinks", reusedLinks},
			{"removedLinks", removedLinks},
			{"diagnostics", inferredDiagnostics}
		}},
		{"validationEvents", {
			{"created", createdValidationEvents},
			{"reused", reusedValidationEvents},
			{"updated", updatedValidationEvents},
			{"cleared", clearedValidationEvents},
			{"diagnostics", validationDiagnostics}
		}},
		{"diagnostics", diagnostics}
	};

	if(!trace.empty())
	{
		out["trace"] = trace;
	}

	return out;
}

json SessionContextToJSON(const RuleSet::SessionContext& context)
{
	return json{
		{"sessionStartDateTime", context.sessionStartDateTime},
		{"userId", context.userId},
		{"userName", context.userName},
		{"currentLocale", context.currentLocale},
		{"deploymentName", context.deploymentName},
		{"deploymentVersion", context.deploymentVersion},
		{"asOfTransactionTime", context.asOfTransactionTime}
	};
}

std::string QuestionTypeToFrontendString(ScreenFile::QuestionType type)
{
	switch(type)
	{
		case ScreenFile::QuestionType::Text:
			return "text";
		case ScreenFile::QuestionType::Date:
			return "date";
		case ScreenFile::QuestionType::Double:
			return "double";
		case ScreenFile::QuestionType::Checkbox:
			return "checkbox";
		case ScreenFile::QuestionType::Radio:
			return "radio";
	}

	return "text";
}

const std::string EmailValidationPattern = R"(^[^@\s]+@[^@\s]+\.[^@\s]+$)";

std::string NormalizedControlType(std::string value)
{
	value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
		return !std::isalnum(ch);
	}), value.end());

	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});

	return value;
}

bool IsEmailControlType(const std::string& controlType)
{
	const std::string normalized = NormalizedControlType(controlType);
	return normalized == "email"
		|| normalized == "emailinput"
		|| normalized == "emailaddress"
		|| normalized == "emailaddressinput";
}

json BuildFrontendErrorsJson(const std::optional<std::vector<ScreenFile::Error>>& errors)
{
	json result = json::array();

	if(errors.has_value())
	{
		for(const ScreenFile::Error& error : errors.value())
		{
			result.push_back({{"errorMessage", error.errorMessage}});
		}
	}

	return result;
}

json BuildFrontendConstraintsJson(const std::optional<std::vector<ScreenFile::Constraint>>& constraints,
                                  bool ensureEmailPattern = false)
{
	json result = json::array();
	bool hasPattern = false;

	if(constraints.has_value())
	{
		for(const ScreenFile::Constraint& constraint : constraints.value())
		{
			json item = json::object();

			if(constraint.maxCharacters.has_value())
			{
				item["maxCharacters"] = constraint.maxCharacters.value();
			}

			if(constraint.maxRows.has_value())
			{
				item["maxRows"] = constraint.maxRows.value();
			}

			if(constraint.decimals.has_value())
			{
				item["decimals"] = constraint.decimals.value();
			}

			if(constraint.pattern.has_value())
			{
				item["pattern"] = constraint.pattern.value();
				hasPattern = true;
			}

			result.push_back(item);
		}
	}

	if(ensureEmailPattern && !hasPattern)
	{
		result.push_back({{"pattern", EmailValidationPattern}});
	}

	return result;
}

json BuildFrontendOptionsJson(const std::optional<std::vector<ScreenFile::Options>>& options)
{
	json result = json::array();

	if(options.has_value())
	{
		for(const ScreenFile::Options& option : options.value())
		{
			json item = {
				{"caption", option.caption},
				{"value", option.value}
			};

			if(option.hint.has_value())
			{
				item["hint"] = option.hint.value();
			}

			result.push_back(item);
		}
	}

	return result;
}

json BuildFrontendColumnsJson(const std::optional<std::vector<ScreenFile::Column>>& columns)
{
	json result = json::array();

	if(columns.has_value())
	{
		for(const ScreenFile::Column& column : columns.value())
		{
			json item = {
				{"text", column.text},
				{"attributeId", column.attributeId}
			};
			if(!column.source.empty())
			{
				item["source"] = column.source;
			}

			if(column.order.has_value())
			{
				item["order"] = column.order.value();
			}

			if(column.tag.has_value())
			{
				item["tag"] = column.tag.value();
			}

			result.push_back(item);
		}
	}

	return result;
}

json BuildFrontendDisplayFieldsJson(const std::optional<std::vector<ScreenFile::Column>>& columns)
{
	json result = json::array();

	if(!columns.has_value())
	{
		return result;
	}

	std::vector<ScreenFile::Column> orderedColumns = columns.value();
	std::stable_sort(
		orderedColumns.begin(),
		orderedColumns.end(),
		[](const ScreenFile::Column& left, const ScreenFile::Column& right) {
			return left.order.value_or(std::numeric_limits<int>::max())
				< right.order.value_or(std::numeric_limits<int>::max());
		});

	for(const ScreenFile::Column& column : orderedColumns)
	{
		json item = {
			{"header", column.text},
			{"text", column.text},
			{"attributeId", column.attributeId}
		};
		if(!column.source.empty())
		{
			item["source"] = column.source;
		}

		if(column.order.has_value())
		{
			item["order"] = column.order.value();
		}

		if(column.tag.has_value())
		{
			item["tag"] = column.tag.value();
		}

		result.push_back(item);
	}

	return result;
}

json BuildFrontendInstanceAttributesJson(const std::vector<ScreenFile::InstanceAttribute>& attributes)
{
	json result = json::array();

	for(const ScreenFile::InstanceAttribute& attribute : attributes)
	{
		result.push_back({
			{"attributeId", attribute.attributeId},
			{"value", attribute.value}
		});
	}

	return result;
}

json BuildFrontendOptionalInstanceAttributesJson(const std::optional<std::vector<ScreenFile::InstanceAttribute>>& attributes)
{
	if(!attributes.has_value())
	{
		return json::array();
	}

	return BuildFrontendInstanceAttributesJson(attributes.value());
}

json BuildFrontendCollectInstancesJson(const std::optional<std::vector<ScreenFile::CollectInstance>>& instances)
{
	json result = json::array();

	if(instances.has_value())
	{
		for(const ScreenFile::CollectInstance& instance : instances.value())
		{
			result.push_back({
				{"instanceId", instance.instanceId},
				{"attributes", BuildFrontendInstanceAttributesJson(instance.attributes)},
				{"selected", instance.selected.value_or(false)}
			});
		}
	}

	return result;
}

json BuildFrontendPaginationJson(const std::optional<ScreenFile::Pagination>& pagination)
{
	json result = json::object();

	if(pagination.has_value())
	{
		if(pagination->currentPage.has_value())
		{
			result["currentPage"] = pagination->currentPage.value();
		}

		if(pagination->optionsPerPage.has_value())
		{
			result["optionsPerPage"] = pagination->optionsPerPage.value();
			result["pageSize"] = pagination->optionsPerPage.value();
		}

		if(pagination->totalPages.has_value())
		{
			result["totalPages"] = pagination->totalPages.value();
		}

		if(pagination->filter.has_value())
		{
			result["filter"] = pagination->filter.value();
		}

		if(pagination->previousLabel.has_value())
		{
			result["previousLabel"] = pagination->previousLabel.value();
		}

		if(pagination->nextLabel.has_value())
		{
			result["nextLabel"] = pagination->nextLabel.value();
		}
	}

	return result;
}

json BuildFrontendTableColumnsJson(const std::optional<std::vector<ScreenFile::TableColumn>>& columns)
{
	json result = json::array();

	if(!columns.has_value())
	{
		return result;
	}

	std::vector<ScreenFile::TableColumn> orderedColumns = columns.value();
	std::stable_sort(
		orderedColumns.begin(),
		orderedColumns.end(),
		[](const ScreenFile::TableColumn& left, const ScreenFile::TableColumn& right) {
			return left.order.value_or(std::numeric_limits<int>::max())
				< right.order.value_or(std::numeric_limits<int>::max());
		});

	for(const ScreenFile::TableColumn& column : orderedColumns)
	{
		json item = {
			{"header", column.header}
		};

		if(column.order.has_value())
		{
			item["order"] = column.order.value();
		}

		result.push_back(item);
	}

	return result;
}

json BuildFrontendTableJson(const ScreenFile::TableData& tableData)
{
	return json{
		{"columns", BuildFrontendTableColumnsJson(tableData.columns)}
	};
}

json BuildFrontendStatusJson(const ScreenFile::StatusData& statusData)
{
	return json{
		{"columns", BuildFrontendColumnsJson(statusData.columns)},
		{"attributes", BuildFrontendOptionalInstanceAttributesJson(statusData.attributes)}
	};
}

json BuildFrontendCollectJson(const ScreenFile::Element& element)
{
	json collect = json::object();

	if(element.collectEntity.has_value())
	{
		collect["entityId"] = element.collectEntity.value();
	}

	if(element.collectRelationshipId.has_value())
	{
		collect["relationshipId"] = element.collectRelationshipId.value();
	}

	if(element.identifiedBy.has_value())
	{
		collect["identifiedBy"] = element.identifiedBy.value();
	}

	if(element.caption.has_value())
	{
		collect["caption"] = element.caption.value();
	}

	if(element.addButton.has_value())
	{
		collect["addButton"] = element.addButton.value();
	}

	if(element.deleteButton.has_value())
	{
		collect["deleteButton"] = element.deleteButton.value();
	}

	if(element.canEdit.has_value())
	{
		collect["canEdit"] = element.canEdit.value();
	}

	if(element.canDelete.has_value())
	{
		collect["canDelete"] = element.canDelete.value();
	}

	if(element.collectData.has_value())
	{
		json displayFields = BuildFrontendDisplayFieldsJson(element.collectData->columns);
		collect["columns"] = BuildFrontendColumnsJson(element.collectData->columns);
		collect["displayFields"] = displayFields;
		collect["table"] = json{
			{"columns", displayFields}
		};
		collect["instances"] = BuildFrontendCollectInstancesJson(element.collectData->instances);
		collect["pagination"] = BuildFrontendPaginationJson(element.collectData->pagination);
		collect["select"] = element.collectData->select.value_or(false);
		collect["selectRows"] = element.collectData->select.value_or(false);
		collect["multiSelect"] = element.collectData->multiSelect.value_or(false);
		collect["showNoneOption"] = element.collectData->showNoneOption.value_or(true);
		collect["selectionRequired"] = element.collectData->selectionRequired.value_or(false);
		collect["selectedCount"] = element.collectData->selectedCount.value_or(0);
		if(element.collectData->membershipRelationshipId.has_value())
		{
			collect["membershipRelationshipId"] = element.collectData->membershipRelationshipId.value();
		}
	}

	if(element.collectFile.has_value())
	{
		json file = {
			{"maximumSizeMb", element.collectFile->maximumSizeMb.value_or(25)},
			{"currentCount", element.collectData.has_value() ? element.collectData->instanceCount.value_or(0) : 0},
			{"restrictTypes", element.collectFile->restrictTypes},
			{"allowedExtensions", element.collectFile->allowedExtensions}
		};
		if(element.collectFile->maximumCount.has_value())
		{
			file["maximumCount"] = element.collectFile->maximumCount.value();
		}
		collect["kind"] = "file";
		collect["file"] = file;
	}

	return collect;
}

bool IsPageableCollectElement(const ScreenFile::Element& element)
{
	return (element.controlType == "collect"
		|| element.controlType == "collectview"
		|| element.controlType == "collect-view"
		|| element.controlType == "collect-file"
		|| element.controlType == "entity-instance-list")
		&& element.collectData.has_value()
		&& element.collectData->pagination.has_value()
		&& element.collectData->columns.has_value()
		&& element.collectEntity.has_value();
}

json BuildFrontendFileUploadJson(const ScreenFile::FileUpload& fileUpload)
{
	json upload = {
		{"id", fileUpload.id},
		{"showFileSize", fileUpload.showFileSize.value_or(true)},
		{"showFileExtension", fileUpload.showFileExtension.value_or(true)}
	};

	if(fileUpload.maxAttachments.has_value())
	{
		upload["maxAttachments"] = fileUpload.maxAttachments.value();
	}

	if(fileUpload.caption.has_value())
	{
		upload["caption"] = fileUpload.caption.value();
	}

	json files = json::array();

	if(fileUpload.files.has_value())
	{
		for(const ScreenFile::FileUploadData& file : fileUpload.files.value())
		{
			files.push_back({
				{"uploadId", file.uploadId},
				{"fileId", file.fileId},
				{"fileName", file.fileName},
				{"fileExtension", file.fileExtension},
				{"fileSize", file.fileSize}
			});
		}
	}

	upload["files"] = files;

	return upload;
}

json BuildFrontendElement(const ScreenFile::Element& element);

json BuildFrontendElements(const std::vector<ScreenFile::Element>& elements)
{
	json result = json::array();

	for(const ScreenFile::Element& element : elements)
	{
		result.push_back(BuildFrontendElement(element));
	}

	return result;
}

json BuildFrontendElement(const ScreenFile::Element& element)
{
	json result = {
		{"controlType", element.controlType},
		{"display", element.display.value_or(true)},
		{"value", element.value.has_value() ? json(element.value.value()) : json(nullptr)},
		{"errors", BuildFrontendErrorsJson(element.errors)},
		{"children", BuildFrontendElements(element.children)}
	};

	if(element.id.has_value())
	{
		result["elementId"] = element.id.value();

		if(element.controlType == "question")
		{
			result["attributeId"] = element.id.value();
		}
	}

	if(element.boundAttributeId.has_value())
	{
		result["attributeId"] = element.boundAttributeId.value();
	}

	if(element.questionText.has_value())
	{
		result["question"] = element.questionText.value();
	}

	if(element.labelText.has_value())
	{
		result["label"] = element.labelText.value();
	}

	if(element.hintText.has_value())
	{
		result["hint"] = element.hintText.value();
	}

	if(element.validationMessage.has_value())
	{
		result["validationMessage"] = element.validationMessage.value();
	}

	if(element.type.has_value())
	{
		result["type"] = QuestionTypeToFrontendString(element.type.value());
	}

	result["required"] = element.required.value_or(false);

	if(element.constraints.has_value() || IsEmailControlType(element.controlType))
	{
		result["constraints"] = BuildFrontendConstraintsJson(element.constraints, IsEmailControlType(element.controlType));
	}

	if(element.className.has_value())
	{
		result["className"] = element.className.value();
	}

	if(element.displayRuleId.has_value())
	{
		result["displayRuleId"] = element.displayRuleId.value();
	}

	if(element.layoutWidthPercent.has_value())
	{
		result["layout"] = {
			{"widthPercent", element.layoutWidthPercent.value()}
		};
	}

	if(element.containerLayout.has_value())
	{
		result["containerLayout"] = element.containerLayout.value();
	}

	if(element.pdfTemplateId.has_value())
	{
		result["pdfForm"] = {
			{"templateId", element.pdfTemplateId.value()},
			{"downloadFileName", element.pdfDownloadFileName.value_or("document.pdf")}
		};
	}

	if(element.options.has_value())
	{
		result["options"] = BuildFrontendOptionsJson(element.options);
	}

	if(element.tableData.has_value())
	{
		result["table"] = BuildFrontendTableJson(element.tableData.value());
	}

	if(element.pagination.has_value())
	{
		result["pagination"] = BuildFrontendPaginationJson(element.pagination);
	}

	if(element.collectData.has_value() && element.collectData->pagination.has_value())
	{
		result["pagination"] = BuildFrontendPaginationJson(element.collectData->pagination);
	}

	if(element.statusData.has_value())
	{
		result["status"] = BuildFrontendStatusJson(element.statusData.value());
	}

	if(element.collectEntity.has_value() || element.collectData.has_value())
	{
		result["collect"] = BuildFrontendCollectJson(element);
	}

	if(element.caption.has_value())
	{
		result["caption"] = element.caption.value();
	}

	if(element.addButton.has_value())
	{
		result["addButton"] = element.addButton.value();
	}

	if(element.deleteButton.has_value())
	{
		result["deleteButton"] = element.deleteButton.value();
	}

	if(element.canEdit.has_value())
	{
		result["canEdit"] = element.canEdit.value();
	}

	if(element.canDelete.has_value())
	{
		result["canDelete"] = element.canDelete.value();
	}

	if(element.bannerHeader.has_value())
	{
		result["bannerHeader"] = element.bannerHeader.value();
	}

	if(element.bannerContent.has_value())
	{
		result["bannerContent"] = element.bannerContent.value();
	}

	if(element.detailsHeader.has_value())
	{
		result["detailsHeader"] = element.detailsHeader.value();
	}

	if(element.detailsContent.has_value())
	{
		result["detailsContent"] = element.detailsContent.value();
	}

	if(element.insetText.has_value())
	{
		result["insetText"] = element.insetText.value();
	}

	if(element.direction.has_value())
	{
		result["direction"] = element.direction.value();
	}

	if(element.fileUpload.has_value())
	{
		result["fileUpload"] = BuildFrontendFileUploadJson(element.fileUpload.value());
	}

	if(element.rendering.has_value() && element.rendering->is_object() && !element.rendering->empty())
	{
		result["rendering"] = element.rendering.value();
	}

	return result;
}

json BuildFrontendScreen(const ScreenFile::Screen& screen)
{
	json result = {
		{"schemaVersion", 1},
		{"screenId", screen.id},
		{"entityId", screen.entityId},
		{"instanceId", screen.instanceId.value_or("")},
		{"display", screen.relevant.value_or(true)},
		{"elements", BuildFrontendElements(screen.elements)}
	};

	if(screen.title.has_value())
	{
		result["title"] = screen.title.value();
	}

	if(screen.back.has_value())
	{
		result["back"] = screen.back.value();
	}

	if(screen.returnTo.has_value())
	{
		result["returnTo"] = screen.returnTo.value();
	}

	if(screen.clearNextOnReturn.has_value())
	{
		result["clearNextOnReturn"] = screen.clearNextOnReturn.value();
	}

	if(screen.isStartScreen.has_value())
	{
		result["isStartScreen"] = screen.isStartScreen.value();
	}

	if(screen.isEndScreen.has_value())
	{
		result["isEndScreen"] = screen.isEndScreen.value();
	}

	return result;
}

json BuildScreenUiStateElement(const ScreenFile::Element& element);

json BuildScreenUiStateElements(const std::vector<ScreenFile::Element>& elements)
{
	json result = json::array();

	for(const ScreenFile::Element& element : elements)
	{
		result.push_back(BuildScreenUiStateElement(element));
	}

	return result;
}

json BuildScreenUiStateElement(const ScreenFile::Element& element)
{
	json elementState = {
		{"controlType", element.controlType},
		{"display", element.display.value_or(true)},
		{"value", element.value.has_value() ? json(element.value.value()) : json(nullptr)},
		{"errors", BuildFrontendErrorsJson(element.errors)}
	};

	if(element.id.has_value())
	{
		elementState["id"] = element.id.value();
	}

	if(element.boundAttributeId.has_value())
	{
		elementState["attributeId"] = element.boundAttributeId.value();
	}

	if(element.pdfTemplateId.has_value())
	{
		elementState["pdfForm"] = {
			{"templateId", element.pdfTemplateId.value()},
			{"downloadFileName", element.pdfDownloadFileName.value_or("document.pdf")}
		};
	}

	if(element.displayRuleId.has_value())
	{
		elementState["displayRuleId"] = element.displayRuleId.value();
	}

	if(element.layoutWidthPercent.has_value())
	{
		elementState["layout"] = {
			{"widthPercent", element.layoutWidthPercent.value()}
		};
	}

	if(element.containerLayout.has_value())
	{
		elementState["containerLayout"] = element.containerLayout.value();
	}

	if(element.questionText.has_value())
	{
		elementState["question"] = element.questionText.value();
	}

	if(element.labelText.has_value())
	{
		elementState["labelText"] = element.labelText.value();
	}

	if(element.validationMessage.has_value())
	{
		elementState["validationMessage"] = element.validationMessage.value();
	}

	if(!element.children.empty())
	{
		elementState["children"] = BuildScreenUiStateElements(element.children);
	}

	return elementState;
}

json BuildScreenUiState(const ScreenFile::Screen& screen)
{
	json elements = BuildScreenUiStateElements(screen.elements);

	return json{
		{"screenId", screen.id},
		{"entityId", screen.entityId},
		{"instanceId", screen.instanceId.value_or("")},
		{"elements", elements}
	};
}

json BuildScreenLocationJson(const std::string& screenId,
                             const std::string& entityId,
                             const std::string& instanceId,
                             std::uint64_t sequence)
{
	return json{
		{"screenId", screenId},
		{"entityId", entityId},
		{"instanceId", instanceId},
		{"sequence", sequence}
	};
}

json BuildScreenLocationJson(const ScreenFile::Screen& screen,
                             const std::string& entityId,
                             const std::string& instanceId,
                             std::uint64_t sequence)
{
	return BuildScreenLocationJson(screen.id, entityId, instanceId, sequence);
}

json BuildScreenLocationJson(const ScreenLocation& location, std::uint64_t sequence)
{
	return BuildScreenLocationJson(location.screenId, location.entityId, location.instanceId, sequence);
}

json BuildCompleteScreenLocationJson(std::uint64_t sequence)
{
	return BuildScreenLocationJson(std::string(), std::string(), std::string(), sequence);
}

json BuildCompleteActionsJson()
{
	return json{
		{"canUpdate", false},
		{"canSubmit", false},
		{"canGoBack", false},
		{"isComplete", true}
	};
}

json BuildDetachedScreenActionsJson()
{
	return json{
		{"canUpdate", true},
		{"canSubmit", false},
		{"canGoBack", false},
		{"isComplete", false}
	};
}

struct ParsedHttpEndpoint
{
	bool valid = false;
	std::string url;
	std::string scheme;
	std::string host;
	std::string path;
	int port = 80;
	bool secure = false;
	std::string error;
};

std::string ToLowerAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
	{
		return static_cast<char>(std::tolower(ch));
	});

	return value;
}

ParsedHttpEndpoint ParseHttpEndpoint(std::string url)
{
	ParsedHttpEndpoint endpoint;
	endpoint.url = TrimAscii(std::move(url));

	if(endpoint.url.empty())
	{
		endpoint.error = "Data-connection dry-run endpoint URL is empty.";
		return endpoint;
	}

	const std::string schemeSeparator = "://";
	const size_t schemeEnd = endpoint.url.find(schemeSeparator);

	if(schemeEnd == std::string::npos)
	{
		endpoint.error = "Data-connection dry-run endpoint URL must include an http:// or https:// scheme.";
		return endpoint;
	}

	const std::string scheme = ToLowerAscii(endpoint.url.substr(0, schemeEnd));

	if(scheme != "http" && scheme != "https")
	{
		endpoint.error = "Data-connection dry-run endpoint currently supports http:// and https:// URLs only.";
		return endpoint;
	}

	endpoint.scheme = scheme;
	endpoint.secure = scheme == "https";
	endpoint.port = endpoint.secure ? 443 : 80;

	const size_t authorityStart = schemeEnd + schemeSeparator.size();
	const size_t pathStart = endpoint.url.find('/', authorityStart);
	const std::string authority = endpoint.url.substr(
		authorityStart,
		pathStart == std::string::npos ? std::string::npos : pathStart - authorityStart);

	if(authority.empty())
	{
		endpoint.error = "Data-connection dry-run endpoint URL is missing a host.";
		return endpoint;
	}

	endpoint.path = pathStart == std::string::npos ? "/" : endpoint.url.substr(pathStart);

	const size_t portSeparator = authority.rfind(':');

	if(portSeparator == std::string::npos)
	{
		endpoint.host = authority;
	}
	else
	{
		endpoint.host = authority.substr(0, portSeparator);
		const std::string portText = authority.substr(portSeparator + 1);

		if(endpoint.host.empty() || portText.empty())
		{
			endpoint.error = "Data-connection dry-run endpoint URL has an invalid host or port.";
			return endpoint;
		}

		try
		{
			const int parsedPort = std::stoi(portText);

			if(parsedPort <= 0 || parsedPort > 65535)
			{
				endpoint.error = "Data-connection dry-run endpoint port is outside the valid range.";
				return endpoint;
			}

			endpoint.port = parsedPort;
		}
		catch(const std::exception&)
		{
			endpoint.error = "Data-connection dry-run endpoint port is not numeric.";
			return endpoint;
		}
	}

	if(endpoint.host.empty())
	{
		endpoint.error = "Data-connection dry-run endpoint URL is missing a host.";
		return endpoint;
	}

	endpoint.valid = true;
	return endpoint;
}

struct DataConnectionHttpPostResult
{
	bool completed = false;
	int status = 0;
	std::string body;
	std::string error;
};

#if defined(_WIN32)
std::string FormatWinHttpError(const std::string& operation, DWORD errorCode = GetLastError())
{
	return operation + " failed with Win32 error " + std::to_string(static_cast<unsigned long>(errorCode)) + ".";
}

std::wstring Utf8ToWideString(const std::string& value)
{
	if(value.empty())
	{
		return std::wstring();
	}

	const int requiredLength = MultiByteToWideChar(
		CP_UTF8,
		0,
		value.data(),
		static_cast<int>(value.size()),
		nullptr,
		0);

	if(requiredLength <= 0)
	{
		throw std::runtime_error(FormatWinHttpError("MultiByteToWideChar"));
	}

	std::wstring wideValue(static_cast<size_t>(requiredLength), L'\0');
	const int convertedLength = MultiByteToWideChar(
		CP_UTF8,
		0,
		value.data(),
		static_cast<int>(value.size()),
		wideValue.data(),
		requiredLength);

	if(convertedLength != requiredLength)
	{
		throw std::runtime_error(FormatWinHttpError("MultiByteToWideChar"));
	}

	return wideValue;
}

struct WinHttpHandle
{
	explicit WinHttpHandle(HINTERNET handleValue = nullptr)
		: handle(handleValue)
	{
	}

	~WinHttpHandle()
	{
		if(handle != nullptr)
		{
			WinHttpCloseHandle(handle);
		}
	}

	WinHttpHandle(const WinHttpHandle&) = delete;
	WinHttpHandle& operator=(const WinHttpHandle&) = delete;

	bool IsValid() const
	{
		return handle != nullptr;
	}

	operator HINTERNET() const
	{
		return handle;
	}

	HINTERNET handle;
};
#endif

DataConnectionHttpPostResult PostBodyToEndpoint(const ParsedHttpEndpoint& endpoint,
                                                const std::string& requestBody,
                                                const std::string& contentType,
                                                int readTimeoutSeconds,
                                                bool allowRedirects)
{
	DataConnectionHttpPostResult result;

#if defined(_WIN32)
	if(requestBody.size() > std::numeric_limits<DWORD>::max())
	{
		result.error = "The data-connection request body is too large to send.";
		return result;
	}

	try
	{
		const std::wstring host = Utf8ToWideString(endpoint.host);
		const std::wstring path = Utf8ToWideString(endpoint.path.empty() ? "/" : endpoint.path);
		const DWORD requestBodyLength = static_cast<DWORD>(requestBody.size());
		const int receiveTimeoutMs = std::max(1, readTimeoutSeconds) * 1000;

		WinHttpHandle session(WinHttpOpen(
			L"MagiaConductor SessionManager/1.0",
			WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0));

		if(!session.IsValid())
		{
			result.error = FormatWinHttpError("WinHttpOpen");
			return result;
		}

		if(!WinHttpSetTimeouts(session, 2000, 2000, 10000, receiveTimeoutMs))
		{
			result.error = FormatWinHttpError("WinHttpSetTimeouts");
			return result;
		}

		WinHttpHandle connection(WinHttpConnect(
			session,
			host.c_str(),
			static_cast<INTERNET_PORT>(endpoint.port),
			0));

		if(!connection.IsValid())
		{
			result.error = FormatWinHttpError("WinHttpConnect");
			return result;
		}

		WinHttpHandle request(WinHttpOpenRequest(
			connection,
			L"POST",
			path.c_str(),
			nullptr,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			endpoint.secure ? WINHTTP_FLAG_SECURE : 0));

		if(!request.IsValid())
		{
			result.error = FormatWinHttpError("WinHttpOpenRequest");
			return result;
		}

		if(!allowRedirects)
		{
			DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS;
			if(!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabledFeatures, sizeof(disabledFeatures)))
			{
				result.error = FormatWinHttpError("WinHttpSetOption(WINHTTP_DISABLE_REDIRECTS)");
				return result;
			}
		}

		const std::wstring headers = L"Content-Type: " + Utf8ToWideString(contentType) + L"\r\n";
		const BOOL requestSent = WinHttpSendRequest(
			request,
			headers.c_str(),
			static_cast<DWORD>(headers.size()),
			requestBodyLength == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(requestBody.data()),
			requestBodyLength,
			requestBodyLength,
			0);

		if(!requestSent)
		{
			result.error = FormatWinHttpError("WinHttpSendRequest");
			return result;
		}

		if(!WinHttpReceiveResponse(request, nullptr))
		{
			result.error = FormatWinHttpError("WinHttpReceiveResponse");
			return result;
		}

		DWORD statusCode = 0;
		DWORD statusCodeSize = sizeof(statusCode);

		if(!WinHttpQueryHeaders(
			request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,
			&statusCode,
			&statusCodeSize,
			WINHTTP_NO_HEADER_INDEX))
		{
			result.error = FormatWinHttpError("WinHttpQueryHeaders");
			return result;
		}

		result.status = static_cast<int>(statusCode);

		for(;;)
		{
			DWORD availableBytes = 0;

			if(!WinHttpQueryDataAvailable(request, &availableBytes))
			{
				result.error = FormatWinHttpError("WinHttpQueryDataAvailable");
				return result;
			}

			if(availableBytes == 0)
			{
				break;
			}

			std::string chunk(static_cast<size_t>(availableBytes), '\0');
			DWORD bytesRead = 0;

			if(!WinHttpReadData(request, chunk.data(), availableBytes, &bytesRead))
			{
				result.error = FormatWinHttpError("WinHttpReadData");
				return result;
			}

			result.body.append(chunk.data(), static_cast<size_t>(bytesRead));
		}

		result.completed = true;
		return result;
	}
	catch(const std::exception& e)
	{
		result.error = e.what();
		return result;
	}
#else
	result.error = "Data-connection HTTP requests are currently implemented for Windows builds only.";
	return result;
#endif
}

DataConnectionHttpPostResult PostJsonToDataConnectionEndpoint(const ParsedHttpEndpoint& endpoint,
                                                              const std::string& requestBody,
                                                              int readTimeoutSeconds)
{
	return PostBodyToEndpoint(endpoint, requestBody, "application/json", readTimeoutSeconds, true);
}

std::string CollectFileBaseName(const std::string& suppliedName)
{
	const size_t separator = suppliedName.find_last_of("/\\");
	std::string fileName = separator == std::string::npos ? suppliedName : suppliedName.substr(separator + 1);
	fileName = TrimAscii(std::move(fileName));

	for(char& character : fileName)
	{
		const unsigned char value = static_cast<unsigned char>(character);
		if(value < 32 || value == 127 || character == '"')
		{
			character = '_';
		}
	}

	if(fileName == "." || fileName == ".." || fileName.size() > 255)
	{
		return std::string();
	}

	return fileName;
}

std::string CollectFileExtension(const std::string& fileName)
{
	const size_t dot = fileName.find_last_of('.');
	if(dot == std::string::npos || dot == 0 || dot + 1 >= fileName.size())
	{
		return std::string();
	}

	return ToLowerAscii(fileName.substr(dot + 1));
}

bool CollectFileExtensionAllowed(const ScreenFile::CollectFile& configuration, const std::string& extension)
{
	if(!configuration.restrictTypes)
	{
		return true;
	}

	return !extension.empty() && std::find(
		configuration.allowedExtensions.begin(),
		configuration.allowedExtensions.end(),
		extension) != configuration.allowedExtensions.end();
}

bool IsGenericUploadMimeType(const std::string& mimeType)
{
	const std::string normalized = ToLowerAscii(TrimAscii(mimeType));
	return normalized.empty() || normalized == "application/octet-stream" || normalized == "binary/octet-stream";
}

bool CollectFileMimeMatchesExtension(const std::string& extension, const std::string& suppliedMimeType)
{
	if(IsGenericUploadMimeType(suppliedMimeType))
	{
		return true;
	}

	static const std::unordered_map<std::string, std::unordered_set<std::string>> mimeTypesByExtension = {
		{"pdf", {"application/pdf"}},
		{"txt", {"text/plain"}},
		{"csv", {"text/csv", "application/csv", "text/plain"}},
		{"json", {"application/json", "text/json"}},
		{"xml", {"application/xml", "text/xml"}},
		{"jpg", {"image/jpeg"}},
		{"jpeg", {"image/jpeg"}},
		{"png", {"image/png"}},
		{"gif", {"image/gif"}},
		{"bmp", {"image/bmp", "image/x-ms-bmp"}},
		{"webp", {"image/webp"}},
		{"svg", {"image/svg+xml"}},
		{"zip", {"application/zip", "application/x-zip-compressed"}},
		{"doc", {"application/msword"}},
		{"docx", {"application/vnd.openxmlformats-officedocument.wordprocessingml.document"}},
		{"xls", {"application/vnd.ms-excel"}},
		{"xlsx", {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"}},
		{"ppt", {"application/vnd.ms-powerpoint"}},
		{"pptx", {"application/vnd.openxmlformats-officedocument.presentationml.presentation"}},
		{"rtf", {"application/rtf", "text/rtf"}},
		{"mp3", {"audio/mpeg"}},
		{"mp4", {"video/mp4"}},
		{"mov", {"video/quicktime"}}
	};

	const auto expected = mimeTypesByExtension.find(extension);
	if(expected == mimeTypesByExtension.end())
	{
		return true;
	}

	return expected->second.find(ToLowerAscii(TrimAscii(suppliedMimeType))) != expected->second.end();
}

bool CollectFileHasExpectedSignature(const std::string& extension, const std::string& content)
{
	const auto startsWith = [&content](std::initializer_list<unsigned char> bytes)
	{
		if(content.size() < bytes.size())
		{
			return false;
		}

		size_t index = 0;
		for(const unsigned char expected : bytes)
		{
			if(static_cast<unsigned char>(content[index++]) != expected)
			{
				return false;
			}
		}
		return true;
	};

	if(extension == "pdf") return content.rfind("%PDF-", 0) == 0;
	if(extension == "png") return startsWith({0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a});
	if(extension == "jpg" || extension == "jpeg") return startsWith({0xff, 0xd8, 0xff});
	if(extension == "gif") return content.rfind("GIF87a", 0) == 0 || content.rfind("GIF89a", 0) == 0;
	if(extension == "bmp") return content.rfind("BM", 0) == 0;
	if(extension == "webp") return content.size() >= 12 && content.rfind("RIFF", 0) == 0 && content.compare(8, 4, "WEBP") == 0;
	if(extension == "zip" || extension == "docx" || extension == "xlsx" || extension == "pptx")
	{
		return startsWith({0x50, 0x4b, 0x03, 0x04}) || startsWith({0x50, 0x4b, 0x05, 0x06}) || startsWith({0x50, 0x4b, 0x07, 0x08});
	}
	if(extension == "rtf") return content.rfind("{\\rtf", 0) == 0;
	if(extension == "mp3")
	{
		return content.rfind("ID3", 0) == 0 || (content.size() >= 2 && static_cast<unsigned char>(content[0]) == 0xff && (static_cast<unsigned char>(content[1]) & 0xe0) == 0xe0);
	}
	if(extension == "mp4" || extension == "mov") return content.size() >= 12 && content.compare(4, 4, "ftyp") == 0;

	return true;
}

std::string EscapeMultipartQuotedValue(const std::string& value)
{
	std::string escaped;
	escaped.reserve(value.size());
	for(const char character : value)
	{
		if(character == '\\' || character == '"')
		{
			escaped.push_back('\\');
		}
		escaped.push_back(character);
	}
	return escaped;
}

DataConnectionHttpPostResult PostCollectFileMultipart(const ParsedHttpEndpoint& endpoint,
	const std::string& fileId,
	const std::string& fileName,
	const std::string& mimeType,
	const std::string& content)
{
	const std::string boundary = "----MagiaConductor-" + fileId;
	std::string body;
	body.reserve(content.size() + fileId.size() + fileName.size() + 512);
	body += "--" + boundary + "\r\n";
	body += "Content-Disposition: form-data; name=\"fileId\"\r\n\r\n";
	body += fileId + "\r\n";
	body += "--" + boundary + "\r\n";
	body += "Content-Disposition: form-data; name=\"file\"; filename=\"" + EscapeMultipartQuotedValue(fileName) + "\"\r\n";
	body += "Content-Type: " + (IsGenericUploadMimeType(mimeType) ? "application/octet-stream" : mimeType) + "\r\n\r\n";
	body.append(content);
	body += "\r\n--" + boundary + "--\r\n";

	return PostBodyToEndpoint(endpoint, body, "multipart/form-data; boundary=" + boundary, 30, false);
}

DataConnectionHttpPostResult PostCollectFileDelete(const ParsedHttpEndpoint& endpoint, const std::string& fileId)
{
	return PostBodyToEndpoint(endpoint, json{{"fileId", fileId}}.dump(), "application/json", 30, false);
}

std::string CollectFileFormField(const httplib::Request& req, std::initializer_list<const char*> names)
{
	for(const char* name : names)
	{
		if(req.form.has_field(name))
		{
			return TrimAscii(req.form.get_field(name));
		}
	}

	return std::string();
}

std::optional<std::uint64_t> CollectFileFormSequence(const httplib::Request& req)
{
	const std::string value = CollectFileFormField(req, {"screenSequence", "sequence"});
	if(value.empty())
	{
		return std::nullopt;
	}

	try
	{
		size_t consumed = 0;
		const unsigned long long parsed = std::stoull(value, &consumed);
		return consumed == value.size() ? std::make_optional<std::uint64_t>(parsed) : std::nullopt;
	}
	catch(...)
	{
		return std::nullopt;
	}
}

ParsedHttpEndpoint ParseCollectFileEndpoint(const std::string& url)
{
	ParsedHttpEndpoint endpoint = ParseHttpEndpoint(url);
	if(!endpoint.valid)
	{
		return endpoint;
	}

	const size_t schemeEnd = endpoint.url.find("://");
	const size_t authorityStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
	const size_t authorityEnd = endpoint.url.find_first_of("/?#", authorityStart);
	const std::string authority = endpoint.url.substr(
		authorityStart,
		authorityEnd == std::string::npos ? std::string::npos : authorityEnd - authorityStart);

	if(authority.find('@') != std::string::npos || endpoint.url.find('#') != std::string::npos)
	{
		endpoint.valid = false;
		endpoint.error = "File-transfer endpoint URLs cannot contain credentials or fragments.";
	}

	return endpoint;
}

json MakeDataConnectionDryRunDiagnostic(const std::string& severity,
                                        const std::string& code,
                                        const std::string& message,
                                        const std::string& context)
{
	return json{
		{"severity", severity},
		{"code", code},
		{"message", message},
		{"context", context}
	};
}

void AppendDataConnectionDryRunDiagnostic(json& result,
                                          const std::string& severity,
                                          const std::string& code,
                                          const std::string& message,
                                          const std::string& context)
{
	if(!result.contains("diagnostics") || !result.at("diagnostics").is_array())
	{
		result["diagnostics"] = json::array();
	}

	result["diagnostics"].push_back(MakeDataConnectionDryRunDiagnostic(severity, code, message, context));
}

const json* FindObjectValue(const json& object, std::initializer_list<const char*> keys)
{
	if(!object.is_object())
	{
		return nullptr;
	}

	for(const char* key : keys)
	{
		if(object.contains(key))
		{
			return &object.at(key);
		}
	}

	return nullptr;
}

std::optional<bool> JsonBoolValue(const json& object, std::initializer_list<const char*> keys)
{
	const json* value = FindObjectValue(object, keys);

	if(value != nullptr && value->is_boolean())
	{
		return value->get<bool>();
	}

	return std::nullopt;
}

std::optional<int> JsonIntValue(const json& object, std::initializer_list<const char*> keys)
{
	const json* value = FindObjectValue(object, keys);

	if(value == nullptr || value->is_null())
	{
		return std::nullopt;
	}

	if(value->is_number_integer())
	{
		return value->get<int>();
	}

	if(value->is_number_unsigned())
	{
		return static_cast<int>(value->get<unsigned int>());
	}

	if(value->is_string())
	{
		try
		{
			return std::stoi(value->get<std::string>());
		}
		catch(const std::exception&)
		{
			return std::nullopt;
		}
	}

	return std::nullopt;
}

std::string JsonStringValue(const json& object, std::initializer_list<const char*> keys)
{
	const json* value = FindObjectValue(object, keys);

	if(value != nullptr && value->is_string())
	{
		return value->get<std::string>();
	}

	return "";
}

json JsonArrayValue(const json& object, std::initializer_list<const char*> keys)
{
	const json* value = FindObjectValue(object, keys);

	if(value != nullptr && value->is_array())
	{
		return *value;
	}

	return json::array();
}

std::size_t DataConnectionSavePlanArraySize(const json& savePlanJson, const char* key)
{
	if(savePlanJson.is_object() && savePlanJson.contains(key) && savePlanJson.at(key).is_array())
	{
		return savePlanJson.at(key).size();
	}

	return 0;
}

std::size_t DataConnectionSavePlanTableCount(const json& savePlanJson)
{
	std::optional<int> count = JsonIntValue(savePlanJson, {"tableCount"});

	if(count.has_value() && count.value() >= 0)
	{
		return static_cast<std::size_t>(count.value());
	}

	return DataConnectionSavePlanArraySize(savePlanJson, "tables");
}

std::size_t DataConnectionSavePlanDiagnosticCount(const json& savePlanJson)
{
	std::optional<int> count = JsonIntValue(savePlanJson, {"diagnosticCount"});

	if(count.has_value() && count.value() >= 0)
	{
		return static_cast<std::size_t>(count.value());
	}

	return DataConnectionSavePlanArraySize(savePlanJson, "diagnostics");
}

bool DataConnectionSavePlanHasContent(const json& savePlanJson)
{
	return DataConnectionSavePlanTableCount(savePlanJson) > 0 || DataConnectionSavePlanDiagnosticCount(savePlanJson) > 0;
}

bool DataConnectionSavePlanHasErrorDiagnostics(const json& savePlanJson)
{
	if(!savePlanJson.is_object() || !savePlanJson.contains("diagnostics") || !savePlanJson.at("diagnostics").is_array())
	{
		return false;
	}

	for(const json& diagnostic : savePlanJson.at("diagnostics"))
	{
		if(diagnostic.is_object() && ToLowerAscii(diagnostic.value("severity", std::string())) == "error")
		{
			return true;
		}
	}

	return false;
}

std::size_t DataConnectionLoadPlanTableCount(const json& loadPlanJson)
{
	std::optional<int> count = JsonIntValue(loadPlanJson, {"tableCount"});

	if(count.has_value() && count.value() >= 0)
	{
		return static_cast<std::size_t>(count.value());
	}

	return DataConnectionSavePlanArraySize(loadPlanJson, "tables");
}

std::size_t DataConnectionLoadPlanFieldCount(const json& loadPlanJson)
{
	std::optional<int> count = JsonIntValue(loadPlanJson, {"fieldCount"});

	if(count.has_value() && count.value() >= 0)
	{
		return static_cast<std::size_t>(count.value());
	}

	std::size_t fieldCount = 0;

	if(loadPlanJson.is_object() && loadPlanJson.contains("tables") && loadPlanJson.at("tables").is_array())
	{
		for(const json& table : loadPlanJson.at("tables"))
		{
			fieldCount += DataConnectionSavePlanArraySize(table, "fields");
		}
	}

	return fieldCount;
}

std::size_t DataConnectionLoadPlanTargetCount(const json& loadPlanJson)
{
	std::optional<int> count = JsonIntValue(loadPlanJson, {"targetCount"});

	if(count.has_value() && count.value() >= 0)
	{
		return static_cast<std::size_t>(count.value());
	}

	std::size_t targetCount = 0;

	if(loadPlanJson.is_object() && loadPlanJson.contains("tables") && loadPlanJson.at("tables").is_array())
	{
		for(const json& table : loadPlanJson.at("tables"))
		{
			if(!table.is_object() || !table.contains("fields") || !table.at("fields").is_array())
			{
				continue;
			}

			for(const json& field : table.at("fields"))
			{
				targetCount += DataConnectionSavePlanArraySize(field, "targets");
			}
		}
	}

	return targetCount;
}

std::size_t DataConnectionLoadPlanDiagnosticCount(const json& loadPlanJson)
{
	std::optional<int> count = JsonIntValue(loadPlanJson, {"diagnosticCount"});

	if(count.has_value() && count.value() >= 0)
	{
		return static_cast<std::size_t>(count.value());
	}

	return DataConnectionSavePlanArraySize(loadPlanJson, "diagnostics");
}

bool DataConnectionLoadPlanHasContent(const json& loadPlanJson)
{
	return DataConnectionLoadPlanTableCount(loadPlanJson) > 0 || DataConnectionLoadPlanDiagnosticCount(loadPlanJson) > 0;
}

bool DataConnectionLoadPlanHasErrorDiagnostics(const json& loadPlanJson)
{
	if(!loadPlanJson.is_object() || !loadPlanJson.contains("diagnostics") || !loadPlanJson.at("diagnostics").is_array())
	{
		return false;
	}

	for(const json& diagnostic : loadPlanJson.at("diagnostics"))
	{
		if(diagnostic.is_object() && ToLowerAscii(diagnostic.value("severity", std::string())) == "error")
		{
			return true;
		}
	}

	return false;
}

bool DataConnectionCommitRequested(const json& requestJson)
{
	const json* value = FindObjectValue(requestJson, {
		"commit",
		"commitDataConnections",
		"commitDataConnectionSave",
		"executeDataConnectionSave",
		"executeSavePlan",
		"saveDataConnections"
	});

	return value != nullptr && value->is_boolean() && value->get<bool>();
}

bool DataConnectionLoadExecuteRequested(const json& requestJson)
{
	const json* value = FindObjectValue(requestJson, {
		"executeDataConnectionLoad",
		"executeDataConnectionLoadPlan",
		"executeLoadPlan",
		"loadDataConnections",
		"readDataConnections",
		"executeDataConnectionRead"
	});

	return value != nullptr && value->is_boolean() && value->get<bool>();
}

bool DataConnectionLoadHydrateRequested(const json& requestJson)
{
	const json* value = FindObjectValue(requestJson, {
		"hydrateDataConnectionLoad",
		"hydrateDataConnectionLoadPlan",
		"hydrateLoadPlan",
		"hydrateDataConnections",
		"applyDataConnectionLoad",
		"applyLoadedDataConnections"
	});

	return value != nullptr && value->is_boolean() && value->get<bool>();
}

std::optional<int> DataConnectionLoadExecuteMaxRows(const json& requestJson)
{
	std::optional<int> maxRows = JsonIntValue(requestJson, {
		"dataConnectionLoadMaxRows",
		"dataConnectionReadMaxRows",
		"loadDataConnectionMaxRows",
		"loadPlanMaxRows"
	});

	if(maxRows.has_value() && maxRows.value() > 0)
	{
		return maxRows;
	}

	return std::nullopt;
}

int DataConnectionAvailableRecordsMaxRows(const json& requestJson)
{
	constexpr int defaultMaxRows = 50;
	constexpr int absoluteMaxRows = 100;

	std::optional<int> maxRows = JsonIntValue(requestJson, {
		"maxRows",
		"availableRecordMaxRows",
		"availableRecordsMaxRows",
		"dataConnectionLoadMaxRows",
		"dataConnectionReadMaxRows"
	});

	if(!maxRows.has_value() || maxRows.value() <= 0)
	{
		return defaultMaxRows;
	}

	return std::min(maxRows.value(), absoluteMaxRows);
}

std::string DataConnectionSelectedRecordId(const json& requestJson)
{
	const json* value = FindObjectValue(requestJson, {
		"recordId",
		"availableRecordId",
		"loadRecordId",
		"selectedRecordId"
	});

	if(value != nullptr && value->is_string())
	{
		return TrimAscii(value->get<std::string>());
	}

	return std::string();
}

bool DataConnectionRecordIdHasInvalidShape(const json& requestJson)
{
	const json* value = FindObjectValue(requestJson, {
		"recordId",
		"availableRecordId",
		"loadRecordId",
		"selectedRecordId"
	});

	return value != nullptr && !value->is_null() && !value->is_string();
}

const json* DataConnectionRequestedRecordIdentity(const json& requestJson)
{
	return FindObjectValue(requestJson, {"recordIdentity"});
}

bool RecordIdentityRequiresExactPrimaryKeyMatch(const json& recordIdentity)
{
	return recordIdentity.is_object()
		&& recordIdentity.value("hasPrimaryKey", false)
		&& recordIdentity.contains("keyFields")
		&& recordIdentity.at("keyFields").is_array()
		&& !recordIdentity.at("keyFields").empty();
}

bool LoadPlanTableHasSessionUserFilter(const json& table)
{
	if(!table.is_object() || !table.contains("filters") || !table.at("filters").is_array())
	{
		return false;
	}

	for(const json& filter : table.at("filters"))
	{
		if(filter.is_object() && filter.value("valueSource", std::string()) == "SessionUserId")
		{
			return true;
		}
	}

	return false;
}

bool LoadPlanEveryTableHasSessionUserFilter(const json& loadPlanJson)
{
	if(!loadPlanJson.is_object() || !loadPlanJson.contains("tables") || !loadPlanJson.at("tables").is_array())
	{
		return false;
	}

	bool sawTable = false;

	for(const json& table : loadPlanJson.at("tables"))
	{
		if(!table.is_object())
		{
			continue;
		}

		sawTable = true;

		if(!LoadPlanTableHasSessionUserFilter(table))
		{
			return false;
		}
	}

	return sawTable;
}

void RefreshDataConnectionLoadPlanCounts(json& loadPlanJson)
{
	if(!loadPlanJson.is_object())
	{
		return;
	}

	std::size_t tableCount = 0;
	std::size_t fieldCount = 0;
	std::size_t targetCount = 0;
	std::size_t filterCount = 0;

	if(loadPlanJson.contains("tables") && loadPlanJson.at("tables").is_array())
	{
		tableCount = loadPlanJson.at("tables").size();

		for(const json& table : loadPlanJson.at("tables"))
		{
			if(!table.is_object())
			{
				continue;
			}

			if(table.contains("fields") && table.at("fields").is_array())
			{
				fieldCount += table.at("fields").size();

				for(const json& field : table.at("fields"))
				{
					if(field.is_object() && field.contains("targets") && field.at("targets").is_array())
					{
						targetCount += field.at("targets").size();
					}
				}
			}

			if(table.contains("filters") && table.at("filters").is_array())
			{
				filterCount += table.at("filters").size();
			}
		}
	}

	loadPlanJson["tableCount"] = tableCount;
	loadPlanJson["fieldCount"] = fieldCount;
	loadPlanJson["targetCount"] = targetCount;
	loadPlanJson["filterCount"] = filterCount;
	loadPlanJson["diagnosticCount"] = DataConnectionSavePlanArraySize(loadPlanJson, "diagnostics");
}

json BuildSessionUserScopedLoadPlan(const json& loadPlanJson)
{
	json scopedPlan = loadPlanJson.is_object() ? loadPlanJson : json::object();
	scopedPlan["tables"] = json::array();

	if(loadPlanJson.is_object() && loadPlanJson.contains("tables") && loadPlanJson.at("tables").is_array())
	{
		for(const json& table : loadPlanJson.at("tables"))
		{
			if(table.is_object() && LoadPlanTableHasSessionUserFilter(table))
			{
				scopedPlan["tables"].push_back(table);
			}
		}
	}

	RefreshDataConnectionLoadPlanCounts(scopedPlan);
	return scopedPlan;
}

bool LoadPlanTableHasFilterForSourceField(const json& table, const std::string& sourceFieldName)
{
	if(sourceFieldName.empty()
		|| !table.is_object()
		|| !table.contains("filters")
		|| !table.at("filters").is_array())
	{
		return false;
	}

	for(const json& filter : table.at("filters"))
	{
		if(filter.is_object() && filter.value("sourceFieldName", std::string()) == sourceFieldName)
		{
			return true;
		}
	}

	return false;
}

const json* FindSelectedRecordKeyTarget(const json& keyField,
                                        const std::string& entityId,
                                        const std::string& attributeId)
{
	if(!keyField.is_object()
		|| !keyField.contains("targets")
		|| !keyField.at("targets").is_array())
	{
		return nullptr;
	}

	for(const json& target : keyField.at("targets"))
	{
		if(!target.is_object())
		{
			continue;
		}

		if(!entityId.empty() && target.value("entityId", std::string()) != entityId)
		{
			continue;
		}

		if(!attributeId.empty() && target.value("attributeId", std::string()) != attributeId)
		{
			continue;
		}

		return &target;
	}

	return nullptr;
}

json BuildSelectedRecordFilter(const json& keyField,
                               const json* target,
                               const std::string& sourceFieldName,
                               const std::string& filterRole,
                               const std::string& valueSource)
{
	const json* rawValue = FindObjectValue(keyField, {"value", "Value"});

	json filter = {
		{"filterRole", filterRole},
		{"valueSource", valueSource},
		{"sourceFieldName", sourceFieldName},
		{"operator", "Equals"},
		{"valueType", keyField.value("valueType", target != nullptr ? target->value("valueType", std::string()) : std::string())},
		{"databaseType", keyField.value("databaseType", std::string())},
		{"value", rawValue != nullptr ? *rawValue : json(nullptr)},
		{"isKnown", rawValue != nullptr && !rawValue->is_null()},
		{"isCertain", rawValue != nullptr && !rawValue->is_null()},
		{"isUncertain", false},
		{"knowledgeState", rawValue != nullptr && !rawValue->is_null() ? "Known" : "Unknown"},
		{"entityId", target != nullptr ? target->value("entityId", std::string()) : std::string()},
		{"entityName", target != nullptr ? target->value("entityName", std::string()) : std::string()},
		{"instanceId", target != nullptr ? target->value("instanceId", std::string()) : std::string()},
		{"attributeId", target != nullptr ? target->value("attributeId", std::string()) : std::string()},
		{"attributeName", target != nullptr ? target->value("attributeName", std::string()) : std::string()},
		{"attributeCoreName", target != nullptr ? target->value("attributeCoreName", std::string()) : std::string()},
		{"attributeDataType", target != nullptr ? target->value("attributeDataType", std::string()) : std::string()}
	};

	return filter;
}

bool LoadPlanTableMatchesRecordIdentity(const json& table, const json& recordIdentity)
{
	if(!table.is_object() || !recordIdentity.is_object())
	{
		return false;
	}

	if(table.value("sourceId", std::string()) != recordIdentity.value("sourceId", std::string()))
	{
		return false;
	}

	return table.value("sourceObjectName", std::string()) == recordIdentity.value("sourceObjectName", std::string());
}

const RuleSet::DataConnectionEntityMapping* FindDataConnectionEntityMappingForLoadPlanTable(
	const RuleSet& ruleSet,
	const json& table)
{
	if(ruleSet.dataConnectionEntityMappings == nullptr || !table.is_object())
	{
		return nullptr;
	}

	const std::string sourceId = table.value("sourceId", std::string());
	const std::string sourceObjectName = table.value("sourceObjectName", std::string());

	for(const RuleSet::DataConnectionEntityMapping& mapping : *ruleSet.dataConnectionEntityMappings)
	{
		if(mapping.sourceId == sourceId && mapping.sourceObjectName == sourceObjectName)
		{
			return &mapping;
		}
	}

	return nullptr;
}

bool AppendSelectedRecordPrimaryKeyFilters(json& table, const json& recordIdentity)
{
	if(!table.is_object()
		|| !recordIdentity.is_object()
		|| !recordIdentity.value("hasPrimaryKey", false)
		|| !recordIdentity.contains("keyFields")
		|| !recordIdentity.at("keyFields").is_array())
	{
		return false;
	}

	if(!table.contains("filters") || !table.at("filters").is_array())
	{
		table["filters"] = json::array();
	}

	bool appended = false;

	for(const json& keyField : recordIdentity.at("keyFields"))
	{
		if(!keyField.is_object())
		{
			continue;
		}

		const std::string sourceFieldName = keyField.value("sourceFieldName", std::string());
		const json* rawValue = FindObjectValue(keyField, {"value", "Value"});
		const bool isNull = keyField.value("isNull", rawValue == nullptr || rawValue->is_null());

		if(sourceFieldName.empty() || rawValue == nullptr || isNull)
		{
			continue;
		}

		const json* target = FindSelectedRecordKeyTarget(keyField, std::string(), std::string());
		json filter = BuildSelectedRecordFilter(
			keyField,
			target,
			sourceFieldName,
			"SelectedRecordPrimaryKey",
			"SelectedRecordPrimaryKey");

		if(filter.value("instanceId", std::string()).empty() && table.contains("filters") && table.at("filters").is_array())
		{
			for(const json& existingFilter : table.at("filters"))
			{
				if(!existingFilter.is_object()
					|| existingFilter.value("entityId", std::string()) != filter.value("entityId", std::string())
					|| existingFilter.value("instanceId", std::string()).empty())
				{
					continue;
				}

				filter["instanceId"] = existingFilter.value("instanceId", std::string());

				if(filter.value("entityName", std::string()).empty())
				{
					filter["entityName"] = existingFilter.value("entityName", std::string());
				}

				break;
			}
		}

		table["filters"].push_back(filter);
		appended = true;
	}

	return appended;
}

bool AppendSelectedRecordRelationshipFilters(json& table,
                                             const RuleSet::DataConnectionEntityMapping& mapping,
                                             const json& recordIdentity)
{
	if(!table.is_object()
		|| !recordIdentity.is_object()
		|| !recordIdentity.value("hasPrimaryKey", false)
		|| !recordIdentity.contains("keyFields")
		|| !recordIdentity.at("keyFields").is_array())
	{
		return false;
	}

	if(!table.contains("filters") || !table.at("filters").is_array())
	{
		table["filters"] = json::array();
	}

	bool appended = false;

	for(const RuleSet::DataConnectionRelationshipFieldMapping& relationshipFieldMapping : mapping.relationshipFieldMappings)
	{
		if(!relationshipFieldMapping.output
			|| relationshipFieldMapping.sourceFieldName.empty()
			|| LoadPlanTableHasFilterForSourceField(table, relationshipFieldMapping.sourceFieldName))
		{
			continue;
		}

		for(const json& keyField : recordIdentity.at("keyFields"))
		{
			const json* rawValue = FindObjectValue(keyField, {"value", "Value"});
			const bool isNull = keyField.value("isNull", rawValue == nullptr || rawValue->is_null());

			if(rawValue == nullptr || isNull)
			{
				continue;
			}

			const json* target = FindSelectedRecordKeyTarget(
				keyField,
				relationshipFieldMapping.relatedEntityId,
				relationshipFieldMapping.relatedAttributeId);

			if(target == nullptr)
			{
				continue;
			}

			table["filters"].push_back(BuildSelectedRecordFilter(
				keyField,
				target,
				relationshipFieldMapping.sourceFieldName,
				"SelectedRecordRelationshipKey",
				"SelectedRecordRelationshipKey"));
			appended = true;
			break;
		}
	}

	return appended;
}

json BuildSelectedRecordLoadPlan(const json& loadPlanJson, const json& selectedRecord, const RuleSet& ruleSet)
{
	json selectedPlan = loadPlanJson.is_object() ? loadPlanJson : json::object();
	selectedPlan["tables"] = json::array();

	if(!loadPlanJson.is_object()
		|| !loadPlanJson.contains("tables")
		|| !loadPlanJson.at("tables").is_array()
		|| !selectedRecord.is_object()
		|| !selectedRecord.contains("recordIdentity")
		|| !selectedRecord.at("recordIdentity").is_object())
	{
		RefreshDataConnectionLoadPlanCounts(selectedPlan);
		return selectedPlan;
	}

	const json& recordIdentity = selectedRecord.at("recordIdentity");
	json relatedTables = json::array();

	for(const json& table : loadPlanJson.at("tables"))
	{
		if(!table.is_object())
		{
			continue;
		}

		json selectedTable = table;

		if(LoadPlanTableMatchesRecordIdentity(selectedTable, recordIdentity))
		{
			AppendSelectedRecordPrimaryKeyFilters(selectedTable, recordIdentity);
			selectedPlan["tables"].push_back(selectedTable);
			continue;
		}

		const RuleSet::DataConnectionEntityMapping* mapping = FindDataConnectionEntityMappingForLoadPlanTable(ruleSet, selectedTable);

		if(mapping != nullptr && AppendSelectedRecordRelationshipFilters(selectedTable, *mapping, recordIdentity))
		{
			relatedTables.push_back(selectedTable);
		}
	}

	for(const json& relatedTable : relatedTables)
	{
		selectedPlan["tables"].push_back(relatedTable);
	}

	RefreshDataConnectionLoadPlanCounts(selectedPlan);
	return selectedPlan;
}

json BuildStartRootLoadPlan(const json& loadPlanJson, const RuleSet& ruleSet)
{
	json startPlan = loadPlanJson.is_object() ? loadPlanJson : json::object();
	startPlan["tables"] = json::array();
	startPlan["diagnostics"] = json::array();

	if(ruleSet.startInputs == nullptr
		|| ruleSet.startInputs->empty()
		|| !loadPlanJson.is_object()
		|| !loadPlanJson.contains("tables")
		|| !loadPlanJson.at("tables").is_array())
	{
		RefreshDataConnectionLoadPlanCounts(startPlan);
		return startPlan;
	}

	std::unordered_map<std::string, std::unordered_set<std::string>> startAttributesByEntity;

	for(const RuleSet::StartInputDefinition& input : *ruleSet.startInputs)
	{
		startAttributesByEntity[input.entityId].insert(input.attributeId);
	}

	for(const json& table : loadPlanJson.at("tables"))
	{
		if(!table.is_object())
		{
			continue;
		}

		json startTable = table;
		startTable["fields"] = json::array();
		startTable["filters"] = json::array();

		if(table.contains("filters") && table.at("filters").is_array())
		{
			for(const json& filter : table.at("filters"))
			{
				if(!filter.is_object()
					|| ToLowerAscii(TrimAscii(filter.value("valueSource", std::string()))) != "currentruntimevalue"
					|| ToLowerAscii(TrimAscii(filter.value("operator", std::string("Equals")))) != "equals"
					|| !filter.value("isKnown", false)
					|| !filter.value("isCertain", false))
				{
					continue;
				}

				const std::string entityId = filter.value("entityId", std::string());
				const std::string attributeId = filter.value("attributeId", std::string());
				auto entityStartAttributes = startAttributesByEntity.find(entityId);

				if(entityStartAttributes != startAttributesByEntity.end()
					&& entityStartAttributes->second.find(attributeId) != entityStartAttributes->second.end())
				{
					startTable["filters"].push_back(filter);
				}
			}
		}

		if(startTable["filters"].empty())
		{
			continue;
		}

		if(table.contains("fields") && table.at("fields").is_array())
		{
			for(const json& field : table.at("fields"))
			{
				if(!field.is_object() || !field.contains("targets") || !field.at("targets").is_array())
				{
					continue;
				}

				json startField = field;
				startField["targets"] = json::array();

				for(const json& target : field.at("targets"))
				{
					if(!target.is_object())
					{
						continue;
					}

					if(startAttributesByEntity.find(target.value("entityId", std::string())) != startAttributesByEntity.end())
					{
						startField["targets"].push_back(target);
					}
				}

				if(!startField["targets"].empty())
				{
					startTable["fields"].push_back(std::move(startField));
				}
			}
		}

		if(!startTable["fields"].empty())
		{
			startPlan["tables"].push_back(std::move(startTable));
		}
	}

	RefreshDataConnectionLoadPlanCounts(startPlan);
	return startPlan;
}

const RuleSet::DataConnectionRelationshipFieldMapping* FindOwnedTraversalRelationshipMapping(
	const RuleSet& ruleSet,
	const RuleSet::DataConnectionEntityMapping& mapping,
	const json& table,
	const std::string& parentEntityId)
{
	if(!table.is_object() || !table.contains("filters") || !table.at("filters").is_array())
	{
		return nullptr;
	}

	for(const json& filter : table.at("filters"))
	{
		if(!filter.is_object()
			|| filter.value("valueSource", std::string()) != "SelectedRecordRelationshipKey")
		{
			continue;
		}

		const std::string sourceFieldName = filter.value("sourceFieldName", std::string());

		for(const RuleSet::DataConnectionRelationshipFieldMapping& relationshipMapping : mapping.relationshipFieldMappings)
		{
			if(!relationshipMapping.output
				|| relationshipMapping.sourceFieldName != sourceFieldName
				|| relationshipMapping.relatedEntityId != parentEntityId)
			{
				continue;
			}

			std::shared_ptr<Relationship> relationship;

			if(ruleSet.entities != nullptr)
			{
				for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
				{
					if(entity == nullptr)
					{
						continue;
					}

					for(const std::shared_ptr<Relationship>& candidate : entity->relationships)
					{
						if(candidate != nullptr && candidate->id == relationshipMapping.relationshipId)
						{
							relationship = candidate;
							break;
						}
					}

					if(relationship != nullptr)
					{
						break;
					}
				}
			}

			const std::string cardinality = relationship != nullptr ? ToLowerAscii(TrimAscii(relationship->type)) : std::string();
			const bool supportedCardinality = cardinality == "one-to-many"
				|| cardinality == "onetomany"
				|| cardinality == "1:n"
				|| cardinality == "1m"
				|| cardinality == "one-to-one"
				|| cardinality == "onetoone"
				|| cardinality == "1:1"
				|| cardinality == "11";

			if(relationship != nullptr
				&& relationship->sourceEntity == parentEntityId
				&& relationship->targetEntity == mapping.entityId
				&& supportedCardinality)
			{
				return &relationshipMapping;
			}
		}
	}

	return nullptr;
}

json BuildOwnedDescendantLoadPlan(
	const json& loadPlanJson,
	const json& parentIdentity,
	const RuleSet& ruleSet,
	const std::string& parentEntityId)
{
	json selectedRecord = {{"recordIdentity", parentIdentity}};
	json selectedPlan = BuildSelectedRecordLoadPlan(loadPlanJson, selectedRecord, ruleSet);
	json descendantPlan = selectedPlan.is_object() ? selectedPlan : json::object();
	descendantPlan["tables"] = json::array();
	descendantPlan["diagnostics"] = json::array();

	if(!selectedPlan.is_object() || !selectedPlan.contains("tables") || !selectedPlan.at("tables").is_array())
	{
		RefreshDataConnectionLoadPlanCounts(descendantPlan);
		return descendantPlan;
	}

	for(const json& table : selectedPlan.at("tables"))
	{
		const RuleSet::DataConnectionEntityMapping* mapping = FindDataConnectionEntityMappingForLoadPlanTable(ruleSet, table);

		if(mapping != nullptr
			&& mapping->entityId != parentEntityId
			&& FindOwnedTraversalRelationshipMapping(ruleSet, *mapping, table, parentEntityId) != nullptr)
		{
			descendantPlan["tables"].push_back(table);
		}
	}

	RefreshDataConnectionLoadPlanCounts(descendantPlan);
	return descendantPlan;
}

void SetRecordIdentityInstanceId(json& identity, const std::string& entityId, const std::string& instanceId)
{
	if(!identity.is_object() || !identity.contains("keyFields") || !identity.at("keyFields").is_array())
	{
		return;
	}

	for(json& keyField : identity["keyFields"])
	{
		if(!keyField.is_object() || !keyField.contains("targets") || !keyField.at("targets").is_array())
		{
			continue;
		}

		for(json& target : keyField["targets"])
		{
			if(target.is_object() && target.value("entityId", std::string()) == entityId)
			{
				target["instanceId"] = instanceId;
			}
		}
	}
}

json BuildAvailableRecordIdentity(const json& operation, const json& row, int operationIndex, int rowIndex);

json BuildTraversalRecordIdentity(
	const RuleSet& ruleSet,
	const json& operation,
	const json& row,
	int operationIndex,
	int rowIndex,
	const std::string& entityId,
	const std::string& instanceId)
{
	json identity = BuildAvailableRecordIdentity(operation, row, operationIndex, rowIndex);
	std::unordered_set<std::string> requiredAttributeIds;

	if(ruleSet.dataConnectionEntityMappings != nullptr)
	{
		for(const RuleSet::DataConnectionEntityMapping& mapping : *ruleSet.dataConnectionEntityMappings)
		{
			for(const RuleSet::DataConnectionRelationshipFieldMapping& relationshipMapping : mapping.relationshipFieldMappings)
			{
				if(relationshipMapping.output
					&& relationshipMapping.relatedEntityId == entityId
					&& !relationshipMapping.relatedAttributeId.empty())
				{
					requiredAttributeIds.insert(relationshipMapping.relatedAttributeId);
				}
			}
		}
	}

	if(!identity.contains("keyFields") || !identity.at("keyFields").is_array())
	{
		identity["keyFields"] = json::array();
	}

	std::unordered_set<std::string> existingAttributes;
	for(const json& keyField : identity.at("keyFields"))
	{
		if(!keyField.is_object() || !keyField.contains("targets") || !keyField.at("targets").is_array())
		{
			continue;
		}

		for(const json& target : keyField.at("targets"))
		{
			if(target.is_object() && target.value("entityId", std::string()) == entityId)
			{
				existingAttributes.insert(target.value("attributeId", std::string()));
			}
		}
	}

	if(row.is_object() && row.contains("values") && row.at("values").is_array())
	{
		for(const json& value : row.at("values"))
		{
			if(!value.is_object() || !value.contains("targets") || !value.at("targets").is_array())
			{
				continue;
			}

			for(const json& target : value.at("targets"))
			{
				const std::string attributeId = target.is_object() ? target.value("attributeId", std::string()) : std::string();

				if(!target.is_object()
					|| target.value("entityId", std::string()) != entityId
					|| requiredAttributeIds.find(attributeId) == requiredAttributeIds.end()
					|| existingAttributes.find(attributeId) != existingAttributes.end())
				{
					continue;
				}

				identity["keyFields"].push_back({
					{"sourceFieldName", value.value("sourceFieldName", std::string())},
					{"databaseType", value.value("databaseType", std::string())},
					{"valueType", value.value("valueType", target.value("valueType", std::string()))},
					{"isPrimaryKey", false},
					{"isNull", value.value("isNull", false)},
					{"value", value.value("value", json(nullptr))},
					{"targets", json::array({target})}
				});
				existingAttributes.insert(attributeId);
			}
		}
	}

	// A Start supplied load key is intentionally allowed to be filter-only (Input=false).
	// In that case the database row cannot repeat the key in its selected values, but the
	// exact, certain equality filter that selected the single Root row is still a safe
	// relationship traversal identity. Do not generalise this to arbitrary filters: only
	// explicitly authored Start supplied attributes may contribute here.
	if(!requiredAttributeIds.empty()
		&& ruleSet.startInputs != nullptr
		&& operation.is_object()
		&& operation.contains("filters")
		&& operation.at("filters").is_array())
	{
		std::unordered_set<std::string> startSuppliedAttributeIds;
		for(const RuleSet::StartInputDefinition& input : *ruleSet.startInputs)
		{
			if(input.entityId == entityId && !input.attributeId.empty())
			{
				startSuppliedAttributeIds.insert(input.attributeId);
			}
		}

		std::unordered_map<std::string, const json*> uniqueFiltersByAttribute;
		std::unordered_set<std::string> ambiguousFilterAttributes;

		for(const json& filter : operation.at("filters"))
		{
			const json* rawValue = filter.is_object() ? FindObjectValue(filter, {"value", "Value"}) : nullptr;
			const std::string attributeId = filter.is_object() ? filter.value("attributeId", std::string()) : std::string();

			if(!filter.is_object()
				|| filter.value("entityId", std::string()) != entityId
				|| requiredAttributeIds.find(attributeId) == requiredAttributeIds.end()
				|| existingAttributes.find(attributeId) != existingAttributes.end()
				|| startSuppliedAttributeIds.find(attributeId) == startSuppliedAttributeIds.end()
				|| ToLowerAscii(TrimAscii(filter.value("valueSource", std::string()))) != "currentruntimevalue"
				|| ToLowerAscii(TrimAscii(filter.value("operator", std::string("Equals")))) != "equals"
				|| !filter.value("isKnown", false)
				|| !filter.value("isCertain", false)
				|| filter.value("sourceFieldName", std::string()).empty()
				|| rawValue == nullptr
				|| rawValue->is_null())
			{
				continue;
			}

			if(uniqueFiltersByAttribute.find(attributeId) != uniqueFiltersByAttribute.end())
			{
				ambiguousFilterAttributes.insert(attributeId);
				continue;
			}

			uniqueFiltersByAttribute.emplace(attributeId, &filter);
		}

		for(const auto& candidate : uniqueFiltersByAttribute)
		{
			const std::string& attributeId = candidate.first;
			const json& filter = *candidate.second;
			const json* rawValue = FindObjectValue(filter, {"value", "Value"});

			if(ambiguousFilterAttributes.find(attributeId) != ambiguousFilterAttributes.end()
				|| rawValue == nullptr
				|| rawValue->is_null())
			{
				continue;
			}

			json target = {
				{"entityId", filter.value("entityId", std::string())},
				{"entityName", filter.value("entityName", std::string())},
				{"instanceId", filter.value("instanceId", instanceId)},
				{"attributeId", attributeId},
				{"attributeName", filter.value("attributeName", std::string())},
				{"attributeCoreName", filter.value("attributeCoreName", std::string())},
				{"attributeDataType", filter.value("attributeDataType", std::string())},
				{"valueType", filter.value("valueType", std::string())}
			};

			identity["keyFields"].push_back({
				{"sourceFieldName", filter.value("sourceFieldName", std::string())},
				{"databaseType", filter.value("databaseType", std::string())},
				{"valueType", filter.value("valueType", std::string())},
				{"isPrimaryKey", false},
				{"isNull", false},
				{"value", *rawValue},
				{"targets", json::array({std::move(target)})}
			});
			existingAttributes.insert(attributeId);
		}
	}

	if(!identity["keyFields"].empty())
	{
		identity["hasPrimaryKey"] = true;
		if(identity.value("identityMode", std::string()) != "PrimaryKey")
		{
			identity["identityMode"] = "RelationshipTraversalKey";
		}
	}

	SetRecordIdentityInstanceId(identity, entityId, instanceId);
	return identity;
}

std::shared_ptr<Instance> FindOwnedHydratedInstanceForIdentity(
	RuleSet& ruleSet,
	const std::string& entityId,
	const std::string& parentInstanceId,
	const std::string& relationshipId,
	const json& identity)
{
	DataManager dataManager;
	std::shared_ptr<Entity> entity = dataManager.GetEntityByID(ruleSet.entities, entityId);

	if(entity == nullptr || !identity.is_object() || !identity.contains("keyFields") || !identity.at("keyFields").is_array())
	{
		return nullptr;
	}

	for(const std::shared_ptr<Instance>& instance : entity->instances)
	{
		if(instance == nullptr)
		{
			continue;
		}

		const bool ownedByParent = std::any_of(
			instance->relationshipLinks.begin(),
			instance->relationshipLinks.end(),
			[&](const InstanceRelationshipLink& link) {
				return link.relationshipId == relationshipId
					&& link.sourceInstanceId == parentInstanceId
					&& link.targetInstanceId == instance->instanceId;
			});

		if(!ownedByParent)
		{
			continue;
		}

		bool matched = false;
		bool mismatch = false;

		for(const json& keyField : identity.at("keyFields"))
		{
			const json* rawValue = FindObjectValue(keyField, {"value", "Value"});
			if(rawValue == nullptr || rawValue->is_null() || !keyField.contains("targets") || !keyField.at("targets").is_array())
			{
				continue;
			}

			for(const json& target : keyField.at("targets"))
			{
				if(!target.is_object() || target.value("entityId", std::string()) != entityId)
				{
					continue;
				}

				std::shared_ptr<Attribute> attribute = instance->GetAttributeByID(target.value("attributeId", std::string()));
				const std::string expectedValue = rawValue->is_string()
					? rawValue->get<std::string>()
					: (rawValue->is_boolean() ? (rawValue->get<bool>() ? "true" : "false") : rawValue->dump());
				matched = true;
				if(attribute == nullptr || attribute->IsUnknown() || attribute->GetStringValue() != expectedValue)
				{
					mismatch = true;
				}
			}
		}

		if(matched && !mismatch)
		{
			return instance;
		}
	}

	return nullptr;
}

json BuildSelectedRecordDeletePlan(const json& loadPlanJson, const json& selectedRecord, const RuleSet& ruleSet)
{
	json selectedLoadPlan = BuildSelectedRecordLoadPlan(loadPlanJson, selectedRecord, ruleSet);
	json deletePlan = selectedLoadPlan.is_object() ? selectedLoadPlan : json::object();
	deletePlan["tables"] = json::array();

	if(!selectedLoadPlan.is_object()
		|| !selectedLoadPlan.contains("tables")
		|| !selectedLoadPlan.at("tables").is_array()
		|| !selectedRecord.is_object()
		|| !selectedRecord.contains("recordIdentity")
		|| !selectedRecord.at("recordIdentity").is_object())
	{
		RefreshDataConnectionLoadPlanCounts(deletePlan);
		return deletePlan;
	}

	const json& recordIdentity = selectedRecord.at("recordIdentity");
	json rootTables = json::array();
	json relatedTables = json::array();

	for(json table : selectedLoadPlan.at("tables"))
	{
		if(!table.is_object())
		{
			continue;
		}

		table["fields"] = json::array();

		if(LoadPlanTableMatchesRecordIdentity(table, recordIdentity))
		{
			rootTables.push_back(table);
		}
		else
		{
			relatedTables.push_back(table);
		}
	}

	for(const json& relatedTable : relatedTables)
	{
		deletePlan["tables"].push_back(relatedTable);
	}

	for(const json& rootTable : rootTables)
	{
		deletePlan["tables"].push_back(rootTable);
	}

	RefreshDataConnectionLoadPlanCounts(deletePlan);
	return deletePlan;
}

json JsonValueForDisplay(const json& value)
{
	if(value.is_string())
	{
		return value.get<std::string>();
	}

	if(value.is_boolean() || value.is_number() || value.is_null())
	{
		return value;
	}

	return value.dump();
}

std::string JsonValueToLabelText(const json& value)
{
	if(value.is_string())
	{
		return value.get<std::string>();
	}

	if(value.is_boolean())
	{
		return value.get<bool>() ? "true" : "false";
	}

	if(value.is_number_integer())
	{
		return std::to_string(value.get<long long>());
	}

	if(value.is_number_unsigned())
	{
		return std::to_string(value.get<unsigned long long>());
	}

	if(value.is_number_float())
	{
		std::ostringstream stream;
		stream << value.get<double>();
		return stream.str();
	}

	return std::string();
}

std::string StableHexHash(const std::string& value)
{
	std::uint64_t hash = 1469598103934665603ULL;

	for(unsigned char ch : value)
	{
		hash ^= static_cast<std::uint64_t>(ch);
		hash *= 1099511628211ULL;
	}

	std::ostringstream stream;
	stream << std::hex << std::setw(16) << std::setfill('0') << hash;
	return stream.str();
}

std::string LoadRecordValueLabel(const json& value)
{
	if(!value.is_object())
	{
		return "";
	}

	if(value.contains("targets") && value.at("targets").is_array() && !value.at("targets").empty())
	{
		const json& target = value.at("targets").front();

		if(target.is_object())
		{
			for(const char* key : {"attributeName", "attributeCoreName", "attributeId"})
			{
				const std::string label = target.value(key, std::string());

				if(!label.empty())
				{
					return label;
				}
			}
		}
	}

	return value.value("sourceFieldName", std::string());
}

const json* FindRowValueForSourceField(const json& row, const std::string& sourceFieldName)
{
	if(sourceFieldName.empty()
		|| !row.is_object()
		|| !row.contains("values")
		|| !row.at("values").is_array())
	{
		return nullptr;
	}

	for(const json& value : row.at("values"))
	{
		if(value.is_object() && value.value("sourceFieldName", std::string()) == sourceFieldName)
		{
			return &value;
		}
	}

	return nullptr;
}

bool IsCreatedTimestampSourceFieldName(std::string sourceFieldName)
{
	sourceFieldName = ToLowerAscii(TrimAscii(std::move(sourceFieldName)));

	return sourceFieldName == "created"
		|| sourceFieldName == "createdat"
		|| sourceFieldName == "created_at"
		|| sourceFieldName == "createdon"
		|| sourceFieldName == "created_on"
		|| sourceFieldName == "createdtimestamp"
		|| sourceFieldName == "created_timestamp"
		|| sourceFieldName == "createddate"
		|| sourceFieldName == "created_date"
		|| sourceFieldName == "createdtime"
		|| sourceFieldName == "created_time";
}

std::string AvailableRecordCreatedTimestamp(const json& row)
{
	if(!row.is_object() || !row.contains("values") || !row.at("values").is_array())
	{
		return "";
	}

	for(const json& value : row.at("values"))
	{
		if(!value.is_object())
		{
			continue;
		}

		if(IsCreatedTimestampSourceFieldName(value.value("sourceFieldName", std::string()))
			|| IsCreatedTimestampSourceFieldName(LoadRecordValueLabel(value)))
		{
			return JsonValueToLabelText(value.value("value", json(nullptr)));
		}
	}

	return "";
}

bool IsMetadataOnlyDataConnectionLoadValue(const json& value)
{
	return value.is_object()
		&& (IsCreatedTimestampSourceFieldName(value.value("sourceFieldName", std::string()))
			|| IsCreatedTimestampSourceFieldName(LoadRecordValueLabel(value)));
}

json BuildAvailableRecordIdentity(const json& operation, const json& row, int operationIndex, int rowIndex)
{
	json identity = {
		{"sourceId", operation.value("sourceId", std::string())},
		{"sourceName", operation.value("sourceName", std::string())},
		{"serverConnectionKey", operation.value("serverConnectionKey", std::string())},
		{"provider", operation.value("provider", std::string())},
		{"sourceObjectName", operation.value("sourceObjectName", std::string())},
		{"operationIndex", operationIndex},
		{"rowIndex", rowIndex},
		{"identityMode", "RowPositionFallback"},
		{"hasPrimaryKey", false},
		{"keyFields", json::array()}
	};

	if(!operation.contains("fields") || !operation.at("fields").is_array())
	{
		return identity;
	}

	for(const json& field : operation.at("fields"))
	{
		if(!field.is_object() || !field.value("isPrimaryKey", false))
		{
			continue;
		}

		const std::string sourceFieldName = field.value("sourceFieldName", std::string());
		const json* rowValue = FindRowValueForSourceField(row, sourceFieldName);

		if(rowValue == nullptr || !rowValue->is_object())
		{
			continue;
		}

		json keyField = {
			{"sourceFieldName", sourceFieldName},
			{"databaseType", rowValue->value("databaseType", field.value("databaseType", std::string()))},
			{"valueType", rowValue->value("valueType", field.value("valueType", std::string()))},
			{"isPrimaryKey", true},
			{"isNull", rowValue->value("isNull", false)},
			{"value", rowValue->value("value", json(nullptr))},
			{"targets", rowValue->value("targets", field.value("targets", json::array()))}
		};

		identity["keyFields"].push_back(keyField);
	}

	if(!identity["keyFields"].empty())
	{
		identity["identityMode"] = "PrimaryKey";
		identity["hasPrimaryKey"] = true;
	}

	return identity;
}

std::string BuildAvailableRecordId(const json& recordIdentity, int operationIndex, int rowIndex, const std::string& firstValueText)
{
	if(recordIdentity.is_object() && recordIdentity.value("hasPrimaryKey", false))
	{
		json stableIdentity = {
			{"sourceId", recordIdentity.value("sourceId", std::string())},
			{"serverConnectionKey", recordIdentity.value("serverConnectionKey", std::string())},
			{"provider", recordIdentity.value("provider", std::string())},
			{"sourceObjectName", recordIdentity.value("sourceObjectName", std::string())},
			{"keyFields", json::array()}
		};

		if(recordIdentity.contains("keyFields") && recordIdentity.at("keyFields").is_array())
		{
			for(const json& keyField : recordIdentity.at("keyFields"))
			{
				if(!keyField.is_object())
				{
					continue;
				}

				stableIdentity["keyFields"].push_back({
					{"sourceFieldName", keyField.value("sourceFieldName", std::string())},
					{"databaseType", keyField.value("databaseType", std::string())},
					{"value", keyField.value("value", json(nullptr))}
				});
			}
		}

		return "pk:" + StableHexHash(stableIdentity.dump());
	}

	return std::to_string(operationIndex) + ":" + std::to_string(rowIndex)
		+ (firstValueText.empty() ? std::string() : ":" + firstValueText);
}

std::string BuildAvailableRecordDisplayId(const json& recordIdentity, const std::string& fallback)
{
	if(recordIdentity.is_object()
		&& recordIdentity.contains("keyFields")
		&& recordIdentity.at("keyFields").is_array())
	{
		std::vector<std::string> keyValues;

		for(const json& keyField : recordIdentity.at("keyFields"))
		{
			if(!keyField.is_object())
			{
				continue;
			}

			const std::string value = JsonValueToLabelText(keyField.value("value", json(nullptr)));

			if(!value.empty())
			{
				keyValues.push_back(value);
			}
		}

		if(!keyValues.empty())
		{
			std::ostringstream stream;

			for(std::size_t index = 0; index < keyValues.size(); ++index)
			{
				if(index > 0)
				{
					stream << " / ";
				}

				stream << keyValues[index];
			}

			return stream.str();
		}
	}

	return fallback;
}

json BuildAvailableRecordsFromLoadExecution(const json& execution)
{
	json records = json::array();

	if(!execution.is_object() || !execution.contains("operations") || !execution.at("operations").is_array())
	{
		return records;
	}

	int operationIndex = 0;

	for(const json& operation : execution.at("operations"))
	{
		if(!operation.is_object() || !operation.contains("rows") || !operation.at("rows").is_array())
		{
			operationIndex++;
			continue;
		}

		int fallbackRowIndex = 0;

		for(const json& row : operation.at("rows"))
		{
			if(!row.is_object())
			{
				fallbackRowIndex++;
				continue;
			}

			const int rowIndex = row.value("rowIndex", fallbackRowIndex);
			json summary = json::object();
			std::string firstValueText;

			if(row.contains("values") && row.at("values").is_array())
			{
				for(const json& value : row.at("values"))
				{
					if(!value.is_object())
					{
						continue;
					}

					const std::string label = LoadRecordValueLabel(value);

					if(!label.empty())
					{
						summary[label] = JsonValueForDisplay(value.value("value", json(nullptr)));
					}

					if(firstValueText.empty())
					{
						firstValueText = JsonValueToLabelText(value.value("value", json(nullptr)));
					}
				}
			}

			const std::string sourceObjectName = operation.value("sourceObjectName", std::string());
			json recordIdentity = BuildAvailableRecordIdentity(operation, row, operationIndex, rowIndex);
			const std::string recordId = BuildAvailableRecordId(recordIdentity, operationIndex, rowIndex, firstValueText);
			const std::string displayRecordId = BuildAvailableRecordDisplayId(recordIdentity, recordId);
			const std::string createdTimestamp = AvailableRecordCreatedTimestamp(row);
			const std::string label = firstValueText.empty()
				? "Record " + std::to_string(static_cast<int>(records.size()) + 1)
				: firstValueText;

			json record = {
				{"recordId", recordId},
				{"displayRecordId", displayRecordId},
				{"label", label},
				{"operationIndex", operationIndex},
				{"rowIndex", rowIndex},
				{"sourceId", operation.value("sourceId", std::string())},
				{"sourceName", operation.value("sourceName", std::string())},
				{"sourceObjectName", sourceObjectName},
				{"createdTimestamp", createdTimestamp},
				{"recordIdentity", recordIdentity},
				{"summary", summary},
				{"values", row.value("values", json::array())}
			};

			records.push_back(record);
			fallbackRowIndex++;
		}

		operationIndex++;
	}

	return records;
}

bool TryFindAvailableRecordLocation(const json& records,
                                    const std::string& recordId,
                                    int& operationIndex,
                                    int& rowIndex,
                                    json& record,
                                    bool* ambiguous = nullptr)
{
	if(ambiguous != nullptr)
	{
		*ambiguous = false;
	}

	if(recordId.empty() || !records.is_array())
	{
		return false;
	}

	int matchCount = 0;
	int matchedOperationIndex = -1;
	int matchedRowIndex = -1;
	json matchedRecord;

	for(const json& candidate : records)
	{
		if(!candidate.is_object() || candidate.value("recordId", std::string()) != recordId)
		{
			continue;
		}

		++matchCount;

		if(matchCount == 1)
		{
			matchedOperationIndex = candidate.value("operationIndex", -1);
			matchedRowIndex = candidate.value("rowIndex", -1);
			matchedRecord = candidate;
		}
	}

	if(matchCount == 1)
	{
		operationIndex = matchedOperationIndex;
		rowIndex = matchedRowIndex;
		record = matchedRecord;
		return operationIndex >= 0 && rowIndex >= 0;
	}

	if(matchCount > 1 && ambiguous != nullptr)
	{
		*ambiguous = true;
	}

	return false;
}

bool FilterLoadExecutionToSelectedRecord(json& execution,
                                         int selectedOperationIndex,
                                         int selectedRowIndex,
                                         json& selectedRow)
{
	if(!execution.is_object() || !execution.contains("operations") || !execution.at("operations").is_array())
	{
		return false;
	}

	bool found = false;
	int operationIndex = 0;
	int totalRows = 0;

	for(json& operation : execution["operations"])
	{
		if(!operation.is_object() || !operation.contains("rows") || !operation.at("rows").is_array())
		{
			operationIndex++;
			continue;
		}

		if(operationIndex == selectedOperationIndex)
		{
			json filteredRows = json::array();
			int fallbackRowIndex = 0;

			for(const json& row : operation.at("rows"))
			{
				if(row.is_object() && row.value("rowIndex", fallbackRowIndex) == selectedRowIndex)
				{
					filteredRows.push_back(row);
					selectedRow = row;
					found = true;
					break;
				}

				fallbackRowIndex++;
			}

			operation["rows"] = filteredRows;
		}

		totalRows += static_cast<int>(operation.at("rows").size());
		operationIndex++;
	}

	execution["rowCount"] = totalRows;
	execution["selectedOperationIndex"] = selectedOperationIndex;
	execution["selectedRowIndex"] = selectedRowIndex;

	return found;
}

bool LoadExecutionRowMatchesRecordIdentity(const json& row, const json& recordIdentity)
{
	if(!row.is_object()
		|| !recordIdentity.is_object()
		|| !recordIdentity.value("hasPrimaryKey", false)
		|| !recordIdentity.contains("keyFields")
		|| !recordIdentity.at("keyFields").is_array())
	{
		return false;
	}

	for(const json& keyField : recordIdentity.at("keyFields"))
	{
		if(!keyField.is_object())
		{
			continue;
		}

		const std::string sourceFieldName = keyField.value("sourceFieldName", std::string());
		const json* expectedValue = FindObjectValue(keyField, {"value", "Value"});
		const json* actualRowValue = FindRowValueForSourceField(row, sourceFieldName);

		if(sourceFieldName.empty()
			|| expectedValue == nullptr
			|| actualRowValue == nullptr
			|| !actualRowValue->is_object())
		{
			return false;
		}

		const json* actualValue = FindObjectValue(*actualRowValue, {"value", "Value"});

		if(actualValue == nullptr || *actualValue != *expectedValue)
		{
			return false;
		}
	}

	return true;
}

bool FilterLoadExecutionToSelectedRecordIdentity(json& execution,
                                                 const json& selectedRecord,
                                                 json& selectedRow)
{
	if(!execution.is_object()
		|| !execution.contains("operations")
		|| !execution.at("operations").is_array()
		|| !selectedRecord.is_object()
		|| !selectedRecord.contains("recordIdentity")
		|| !selectedRecord.at("recordIdentity").is_object())
	{
		return false;
	}

	const json& recordIdentity = selectedRecord.at("recordIdentity");
	const int fallbackOperationIndex = selectedRecord.value("operationIndex", recordIdentity.value("operationIndex", -1));
	const int fallbackRowIndex = selectedRecord.value("rowIndex", recordIdentity.value("rowIndex", -1));
	const bool requireExactPrimaryKeyMatch = RecordIdentityRequiresExactPrimaryKeyMatch(recordIdentity);
	bool found = false;
	int identityMatchCount = 0;
	int operationIndex = 0;
	int totalRows = 0;

	for(json& operation : execution["operations"])
	{
		if(!operation.is_object() || !operation.contains("rows") || !operation.at("rows").is_array())
		{
			operationIndex++;
			continue;
		}

		const bool sourceMatches =
			operation.value("sourceId", std::string()) == recordIdentity.value("sourceId", std::string())
			&& operation.value("sourceObjectName", std::string()) == recordIdentity.value("sourceObjectName", std::string());

		if(sourceMatches || operationIndex == fallbackOperationIndex)
		{
			json filteredRows = json::array();
			int rowFallbackIndex = 0;

			for(const json& row : operation.at("rows"))
			{
				const bool identityMatches = LoadExecutionRowMatchesRecordIdentity(row, recordIdentity);
				const bool fallbackMatches = !requireExactPrimaryKeyMatch
					&& !found
					&& !identityMatches
					&& operationIndex == fallbackOperationIndex
					&& row.is_object()
					&& row.value("rowIndex", rowFallbackIndex) == fallbackRowIndex;

				if(identityMatches)
				{
					++identityMatchCount;

					if(identityMatchCount == 1)
					{
						filteredRows.push_back(row);
						selectedRow = row;
						found = true;
					}
				}
				else if(fallbackMatches)
				{
					filteredRows.push_back(row);
					selectedRow = row;
					found = true;
				}

				rowFallbackIndex++;
			}

			operation["rows"] = filteredRows;
		}

		totalRows += static_cast<int>(operation.at("rows").size());
		operationIndex++;
	}

	execution["rowCount"] = totalRows;
	execution["selectedOperationIndex"] = fallbackOperationIndex;
	execution["selectedRowIndex"] = fallbackRowIndex;

	if(requireExactPrimaryKeyMatch)
	{
		return identityMatchCount == 1;
	}

	return found;
}

void AppendSelectedRecordIdentityFilters(json& execution, const json& selectedRecord)
{
	if(!execution.is_object()
		|| !execution.contains("operations")
		|| !execution.at("operations").is_array()
		|| !selectedRecord.is_object()
		|| !selectedRecord.contains("recordIdentity")
		|| !selectedRecord.at("recordIdentity").is_object())
	{
		return;
	}

	const json& recordIdentity = selectedRecord.at("recordIdentity");

	if(!recordIdentity.value("hasPrimaryKey", false)
		|| !recordIdentity.contains("keyFields")
		|| !recordIdentity.at("keyFields").is_array())
	{
		return;
	}

	const int selectedOperationIndex = selectedRecord.value("operationIndex", recordIdentity.value("operationIndex", -1));

	if(selectedOperationIndex < 0 || selectedOperationIndex >= static_cast<int>(execution.at("operations").size()))
	{
		return;
	}

	json& operation = execution["operations"][selectedOperationIndex];

	if(!operation.is_object())
	{
		return;
	}

	if(!operation.contains("filters") || !operation.at("filters").is_array())
	{
		operation["filters"] = json::array();
	}

	for(const json& keyField : recordIdentity.at("keyFields"))
	{
		if(!keyField.is_object())
		{
			continue;
		}

		const std::string sourceFieldName = keyField.value("sourceFieldName", std::string());
		const json* rawValue = FindObjectValue(keyField, {"value", "Value"});
		const bool isNull = keyField.value("isNull", rawValue == nullptr || rawValue->is_null());

		if(sourceFieldName.empty() || rawValue == nullptr || isNull)
		{
			continue;
		}

		const json targets = keyField.value("targets", json::array());

		if(!targets.is_array() || targets.empty())
		{
			continue;
		}

		for(const json& target : targets)
		{
			if(!target.is_object())
			{
				continue;
			}

			const json* existingTargetFilter = nullptr;
			for(const json& existingFilter : operation["filters"])
			{
				if(!existingFilter.is_object()
					|| existingFilter.value("sourceFieldName", std::string()) != sourceFieldName)
				{
					continue;
				}

				const std::string targetEntityId = target.value("entityId", std::string());
				const std::string targetAttributeId = target.value("attributeId", std::string());

				if(!targetEntityId.empty()
					&& existingFilter.value("entityId", std::string()) != targetEntityId)
				{
					continue;
				}

				if(!targetAttributeId.empty()
					&& existingFilter.value("attributeId", std::string()) != targetAttributeId)
				{
					continue;
				}

				existingTargetFilter = &existingFilter;
				break;
			}

			auto targetString = [&](const char* name) {
				std::string value = target.value(name, std::string());

				if(value.empty() && existingTargetFilter != nullptr)
				{
					value = existingTargetFilter->value(name, std::string());
				}

				return value;
			};

			json filter = {
				{"filterRole", "RecordIdentity"},
				{"valueSource", "SelectedRecordPrimaryKey"},
				{"sourceFieldName", sourceFieldName},
				{"operator", "Equals"},
				{"valueType", keyField.value("valueType", targetString("valueType"))},
				{"databaseType", keyField.value("databaseType", std::string())},
				{"value", *rawValue},
				{"isKnown", true},
				{"isCertain", true},
				{"isUncertain", false},
				{"knowledgeState", "Known"},
				{"entityId", targetString("entityId")},
				{"entityName", targetString("entityName")},
				{"instanceId", targetString("instanceId")},
				{"attributeId", targetString("attributeId")},
				{"attributeName", targetString("attributeName")},
				{"attributeCoreName", targetString("attributeCoreName")},
				{"attributeDataType", targetString("attributeDataType")}
			};

			operation["filters"].push_back(filter);
		}
	}
}

json BuildDataConnectionLoadPlanDryRunResult(const json& loadPlanJson)
{
	const std::size_t tableCount = DataConnectionLoadPlanTableCount(loadPlanJson);
	const std::size_t fieldCount = DataConnectionLoadPlanFieldCount(loadPlanJson);
	const std::size_t targetCount = DataConnectionLoadPlanTargetCount(loadPlanJson);
	const std::size_t diagnosticCount = DataConnectionLoadPlanDiagnosticCount(loadPlanJson);

	json result = {
		{"configured", false},
		{"attempted", false},
		{"dryRun", true},
		{"readExecuted", false},
		{"endpoint", nullptr},
		{"httpStatus", nullptr},
		{"succeeded", false},
		{"message", std::string()},
		{"operationCount", 0},
		{"operations", json::array()},
		{"diagnostics", json::array()},
		{"requestTableCount", tableCount},
		{"requestFieldCount", fieldCount},
		{"requestTargetCount", targetCount},
		{"requestDiagnosticCount", diagnosticCount}
	};

	if(tableCount == 0)
	{
		result["succeeded"] = !DataConnectionLoadPlanHasErrorDiagnostics(loadPlanJson);
		result["message"] = "No data-connection load-plan tables were available to dry-run.";
		AppendDataConnectionDryRunDiagnostic(
			result,
			"warning",
			"data_connection_load_dry_run_no_tables",
			"No data-connection load-plan tables were available to submit to the load dry-run endpoint.",
			"sessionModel.dataConnectionLoadPlan.tables");
		return result;
	}

	std::string endpointUrl;

	try
	{
		endpointUrl = ResolveDataConnectionLoadPlanDryRunUrl();
	}
	catch(const std::exception& e)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_dry_run_endpoint_resolution_failed",
			std::string("The data-connection load dry-run endpoint could not be resolved: ") + e.what(),
			"dataConnectionLoadDryRunEndpoint");
		return result;
	}

	result["configured"] = !TrimAscii(endpointUrl).empty();
	result["endpoint"] = endpointUrl.empty() ? json(nullptr) : json(endpointUrl);

	ParsedHttpEndpoint endpoint = ParseHttpEndpoint(endpointUrl);

	if(!endpoint.valid)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_dry_run_endpoint_invalid",
			endpoint.error,
			endpointUrl);
		return result;
	}

	result["attempted"] = true;

	const DataConnectionHttpPostResult response = PostJsonToDataConnectionEndpoint(endpoint, loadPlanJson.dump(), 10);

	if(!response.completed)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_dry_run_request_failed",
			"The data-connection load dry-run endpoint did not return a response: " + response.error,
			endpointUrl);
		return result;
	}

	result["httpStatus"] = response.status;

	json connectorResponse = json::parse(response.body, nullptr, false);

	if(connectorResponse.is_discarded())
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_dry_run_response_invalid_json",
			"The data-connection load dry-run endpoint did not return valid JSON.",
			endpointUrl);
		return result;
	}

	if(connectorResponse.is_object())
	{
		result["response"] = connectorResponse;
		result["succeeded"] = JsonBoolValue(connectorResponse, {"succeeded", "Succeeded"}).value_or(response.status >= 200 && response.status < 300);
		result["dryRun"] = JsonBoolValue(connectorResponse, {"dryRun", "DryRun"}).value_or(true);
		result["readExecuted"] = JsonBoolValue(connectorResponse, {"readExecuted", "ReadExecuted"}).value_or(false);
		result["message"] = JsonStringValue(connectorResponse, {"message", "Message"});
		result["operations"] = JsonArrayValue(connectorResponse, {"operations", "Operations"});
		result["diagnostics"] = JsonArrayValue(connectorResponse, {"diagnostics", "Diagnostics"});
		result["operationCount"] = JsonIntValue(connectorResponse, {"operationCount", "OperationCount"}).value_or(static_cast<int>(result["operations"].size()));
	}

	if(response.status < 200 || response.status >= 300)
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_dry_run_http_error",
			"The data-connection load dry-run endpoint returned HTTP " + std::to_string(response.status) + ".",
			endpointUrl);
	}

	if(!result.value("dryRun", true))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_dry_run_flag_missing",
			"The data-connection load endpoint response did not confirm dryRun=true.",
			endpointUrl);
	}

	if(result.value("readExecuted", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_dry_run_read_executed_unexpected",
			"The data-connection load endpoint reported readExecuted=true during a dry run.",
			endpointUrl);
	}

	return result;
}

json BuildDataConnectionLoadPlanExecutionResult(const json& loadPlanJson, std::optional<int> maxRows)
{
	const std::size_t tableCount = DataConnectionLoadPlanTableCount(loadPlanJson);
	const std::size_t fieldCount = DataConnectionLoadPlanFieldCount(loadPlanJson);
	const std::size_t targetCount = DataConnectionLoadPlanTargetCount(loadPlanJson);
	const std::size_t diagnosticCount = DataConnectionLoadPlanDiagnosticCount(loadPlanJson);

	json result = {
		{"configured", false},
		{"attempted", false},
		{"executeRequested", true},
		{"dryRun", false},
		{"readExecuted", false},
		{"endpoint", nullptr},
		{"httpStatus", nullptr},
		{"succeeded", false},
		{"message", std::string()},
		{"operationCount", 0},
		{"rowCount", 0},
		{"operations", json::array()},
		{"diagnostics", json::array()},
		{"requestTableCount", tableCount},
		{"requestFieldCount", fieldCount},
		{"requestTargetCount", targetCount},
		{"requestDiagnosticCount", diagnosticCount}
	};

	if(maxRows.has_value())
	{
		result["maxRows"] = maxRows.value();
	}

	if(tableCount == 0)
	{
		result["message"] = "No data-connection load-plan tables were available to execute.";
		AppendDataConnectionDryRunDiagnostic(
			result,
			"warning",
			"data_connection_load_execute_no_tables",
			"No data-connection load-plan tables were available to submit to the load execution endpoint.",
			"sessionModel.dataConnectionLoadPlan.tables");
		return result;
	}

	std::string endpointUrl;

	try
	{
		endpointUrl = ResolveDataConnectionLoadPlanExecuteUrl();
	}
	catch(const std::exception& e)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_execute_endpoint_resolution_failed",
			std::string("The data-connection load execution endpoint could not be resolved: ") + e.what(),
			"dataConnectionLoadExecutionEndpoint");
		return result;
	}

	result["configured"] = !TrimAscii(endpointUrl).empty();
	result["endpoint"] = endpointUrl.empty() ? json(nullptr) : json(endpointUrl);

	ParsedHttpEndpoint endpoint = ParseHttpEndpoint(endpointUrl);

	if(!endpoint.valid)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_execute_endpoint_invalid",
			endpoint.error,
			endpointUrl);
		return result;
	}

	result["attempted"] = true;

	json requestBody = {
		{"execute", true},
		{"tables", loadPlanJson.value("tables", json::array())},
		{"diagnostics", loadPlanJson.value("diagnostics", json::array())}
	};

	if(maxRows.has_value())
	{
		requestBody["maxRows"] = maxRows.value();
	}

	const DataConnectionHttpPostResult response = PostJsonToDataConnectionEndpoint(endpoint, requestBody.dump(), 30);

	if(!response.completed)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_execute_request_failed",
			"The data-connection load execution endpoint did not return a response: " + response.error,
			endpointUrl);
		return result;
	}

	result["httpStatus"] = response.status;

	json connectorResponse = json::parse(response.body, nullptr, false);

	if(connectorResponse.is_discarded())
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_execute_response_invalid_json",
			"The data-connection load execution endpoint did not return valid JSON.",
			endpointUrl);
		return result;
	}

	if(connectorResponse.is_object())
	{
		result["response"] = connectorResponse;
		result["succeeded"] = JsonBoolValue(connectorResponse, {"succeeded", "Succeeded"}).value_or(response.status >= 200 && response.status < 300);
		result["dryRun"] = JsonBoolValue(connectorResponse, {"dryRun", "DryRun"}).value_or(false);
		result["readExecuted"] = JsonBoolValue(connectorResponse, {"readExecuted", "ReadExecuted"}).value_or(false);
		result["message"] = JsonStringValue(connectorResponse, {"message", "Message"});
		result["operations"] = JsonArrayValue(connectorResponse, {"operations", "Operations"});
		result["diagnostics"] = JsonArrayValue(connectorResponse, {"diagnostics", "Diagnostics"});
		result["operationCount"] = JsonIntValue(connectorResponse, {"operationCount", "OperationCount"}).value_or(static_cast<int>(result["operations"].size()));
		result["rowCount"] = JsonIntValue(connectorResponse, {"rowCount", "RowCount"}).value_or(0);
		const std::optional<int> responseMaxRows = JsonIntValue(connectorResponse, {"maxRows", "MaxRows"});

		if(responseMaxRows.has_value())
		{
			result["maxRows"] = responseMaxRows.value();
		}
	}

	if(response.status < 200 || response.status >= 300)
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_execute_http_error",
			"The data-connection load execution endpoint returned HTTP " + std::to_string(response.status) + ".",
			endpointUrl);
	}

	if(result.value("dryRun", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_execute_remained_dry_run",
			"The data-connection load execution endpoint response still reported dryRun=true after an explicit load execution request.",
			endpointUrl);
	}

	if(result.value("succeeded", false) && !result.value("readExecuted", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_load_execute_read_not_confirmed",
			"The data-connection load execution endpoint reported success without confirming readExecuted=true.",
			endpointUrl);
	}

	return result;
}

json BuildDataConnectionDeletePlanExecutionResult(const json& deletePlanJson)
{
	const std::size_t tableCount = DataConnectionLoadPlanTableCount(deletePlanJson);
	const std::size_t diagnosticCount = DataConnectionLoadPlanDiagnosticCount(deletePlanJson);

	json result = {
		{"configured", false},
		{"attempted", false},
		{"executeRequested", true},
		{"dryRun", false},
		{"deleteExecuted", false},
		{"committed", false},
		{"rolledBack", false},
		{"endpoint", nullptr},
		{"httpStatus", nullptr},
		{"succeeded", false},
		{"message", std::string()},
		{"operationCount", 0},
		{"rowsAffected", 0},
		{"deletedCount", 0},
		{"operations", json::array()},
		{"diagnostics", json::array()},
		{"requestTableCount", tableCount},
		{"requestDiagnosticCount", diagnosticCount}
	};

	if(tableCount == 0)
	{
		result["message"] = "No data-connection delete-plan tables were available to execute.";
		AppendDataConnectionDryRunDiagnostic(
			result,
			"warning",
			"data_connection_delete_execute_no_tables",
			"No data-connection delete-plan tables were available to submit to the delete execution endpoint.",
			"sessionModel.dataConnectionDeletePlan.tables");
		return result;
	}

	std::string endpointUrl;

	try
	{
		endpointUrl = ResolveDataConnectionDeletePlanExecuteUrl();
	}
	catch(const std::exception& e)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_endpoint_resolution_failed",
			std::string("The data-connection delete execution endpoint could not be resolved: ") + e.what(),
			"dataConnectionDeleteExecutionEndpoint");
		return result;
	}

	result["configured"] = !TrimAscii(endpointUrl).empty();
	result["endpoint"] = endpointUrl.empty() ? json(nullptr) : json(endpointUrl);

	ParsedHttpEndpoint endpoint = ParseHttpEndpoint(endpointUrl);

	if(!endpoint.valid)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_endpoint_invalid",
			endpoint.error,
			endpointUrl);
		return result;
	}

	result["attempted"] = true;

	json requestBody = {
		{"execute", true},
		{"tables", deletePlanJson.value("tables", json::array())},
		{"diagnostics", deletePlanJson.value("diagnostics", json::array())}
	};

	const DataConnectionHttpPostResult response = PostJsonToDataConnectionEndpoint(endpoint, requestBody.dump(), 30);

	if(!response.completed)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_request_failed",
			"The data-connection delete execution endpoint did not return a response: " + response.error,
			endpointUrl);
		return result;
	}

	result["httpStatus"] = response.status;

	json connectorResponse = json::parse(response.body, nullptr, false);

	if(connectorResponse.is_discarded())
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_response_invalid_json",
			"The data-connection delete execution endpoint did not return valid JSON.",
			endpointUrl);
		return result;
	}

	if(connectorResponse.is_object())
	{
		result["response"] = connectorResponse;
		result["succeeded"] = JsonBoolValue(connectorResponse, {"succeeded", "Succeeded"}).value_or(response.status >= 200 && response.status < 300);
		result["dryRun"] = JsonBoolValue(connectorResponse, {"dryRun", "DryRun"}).value_or(false);
		result["deleteExecuted"] = JsonBoolValue(connectorResponse, {"deleteExecuted", "DeleteExecuted"}).value_or(false);
		result["committed"] = JsonBoolValue(connectorResponse, {"committed", "Committed"}).value_or(false);
		result["rolledBack"] = JsonBoolValue(connectorResponse, {"rolledBack", "RolledBack"}).value_or(false);
		result["message"] = JsonStringValue(connectorResponse, {"message", "Message"});
		result["operations"] = JsonArrayValue(connectorResponse, {"operations", "Operations"});
		result["diagnostics"] = JsonArrayValue(connectorResponse, {"diagnostics", "Diagnostics"});
		result["operationCount"] = JsonIntValue(connectorResponse, {"operationCount", "OperationCount"}).value_or(static_cast<int>(result["operations"].size()));
		result["rowsAffected"] = JsonIntValue(connectorResponse, {"rowsAffected", "RowsAffected"}).value_or(0);
		result["deletedCount"] = JsonIntValue(connectorResponse, {"deletedCount", "DeletedCount"}).value_or(result.value("rowsAffected", 0));
	}

	if(response.status < 200 || response.status >= 300)
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_http_error",
			"The data-connection delete execution endpoint returned HTTP " + std::to_string(response.status) + ".",
			endpointUrl);
	}

	if(result.value("dryRun", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_remained_dry_run",
			"The data-connection delete execution endpoint response still reported dryRun=true after an explicit delete execution request.",
			endpointUrl);
	}

	if(result.value("succeeded", false) && !result.value("deleteExecuted", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_not_confirmed",
			"The data-connection delete execution endpoint reported success without confirming deleteExecuted=true.",
			endpointUrl);
	}

	if(result.value("succeeded", false) && !result.value("committed", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_delete_execute_not_committed",
			"The data-connection delete execution endpoint reported success without confirming committed=true.",
			endpointUrl);
	}

	return result;
}

json BuildDataConnectionSavePlanDryRunResult(const json& savePlanJson)
{
	const std::size_t tableCount = DataConnectionSavePlanTableCount(savePlanJson);
	const std::size_t diagnosticCount = DataConnectionSavePlanDiagnosticCount(savePlanJson);

	json result = {
		{"configured", false},
		{"attempted", false},
		{"dryRun", true},
		{"committed", false},
		{"endpoint", nullptr},
		{"httpStatus", nullptr},
		{"succeeded", false},
		{"message", std::string()},
		{"operationCount", 0},
		{"operations", json::array()},
		{"diagnostics", json::array()},
		{"requestTableCount", tableCount},
		{"requestDiagnosticCount", diagnosticCount}
	};

	if(tableCount == 0)
	{
		result["succeeded"] = !DataConnectionSavePlanHasErrorDiagnostics(savePlanJson);
		result["message"] = "No data-connection save-plan tables were available to dry-run.";
		AppendDataConnectionDryRunDiagnostic(
			result,
			"warning",
			"data_connection_dry_run_no_tables",
			"No data-connection save-plan tables were available to submit to the dry-run endpoint.",
			"sessionModel.dataConnectionSavePlan.tables");
		return result;
	}

	if(DataConnectionSavePlanHasErrorDiagnostics(savePlanJson))
	{
		result["diagnostics"] = JsonArrayValue(savePlanJson, {"diagnostics"});
		result["message"] = "Data-connection save dry-run was not attempted because the Session Manager save plan contains error diagnostics.";
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_save_plan_has_errors",
			"The Session Manager save plan contains error diagnostics, so it was not submitted to the data connector dry-run endpoint.",
			"sessionModel.dataConnectionSavePlan.diagnostics");
		return result;
	}

	std::string endpointUrl;

	try
	{
		endpointUrl = ResolveDataConnectionSavePlanDryRunUrl();
	}
	catch(const std::exception& e)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_endpoint_resolution_failed",
			std::string("The data-connection dry-run endpoint could not be resolved: ") + e.what(),
			"dataConnectionDryRunEndpoint");
		return result;
	}

	result["configured"] = !TrimAscii(endpointUrl).empty();
	result["endpoint"] = endpointUrl.empty() ? json(nullptr) : json(endpointUrl);

	ParsedHttpEndpoint endpoint = ParseHttpEndpoint(endpointUrl);

	if(!endpoint.valid)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_endpoint_invalid",
			endpoint.error,
			endpointUrl);
		return result;
	}

	result["attempted"] = true;

	const DataConnectionHttpPostResult response = PostJsonToDataConnectionEndpoint(endpoint, savePlanJson.dump(), 10);

	if(!response.completed)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_request_failed",
			"The data-connection dry-run endpoint did not return a response: " + response.error,
			endpointUrl);
		return result;
	}

	result["httpStatus"] = response.status;

	json connectorResponse = json::parse(response.body, nullptr, false);

	if(connectorResponse.is_discarded())
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_response_invalid_json",
			"The data-connection dry-run endpoint did not return valid JSON.",
			endpointUrl);
		return result;
	}

	if(connectorResponse.is_object())
	{
		result["response"] = connectorResponse;
		result["succeeded"] = JsonBoolValue(connectorResponse, {"succeeded", "Succeeded"}).value_or(response.status >= 200 && response.status < 300);
		result["dryRun"] = JsonBoolValue(connectorResponse, {"dryRun", "DryRun"}).value_or(true);
		result["committed"] = JsonBoolValue(connectorResponse, {"committed", "Committed"}).value_or(false);
		result["message"] = JsonStringValue(connectorResponse, {"message", "Message"});
		result["operations"] = JsonArrayValue(connectorResponse, {"operations", "Operations"});
		result["diagnostics"] = JsonArrayValue(connectorResponse, {"diagnostics", "Diagnostics"});
		result["operationCount"] = JsonIntValue(connectorResponse, {"operationCount", "OperationCount"}).value_or(static_cast<int>(result["operations"].size()));
	}

	if(response.status < 200 || response.status >= 300)
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_http_error",
			"The data-connection dry-run endpoint returned HTTP " + std::to_string(response.status) + ".",
			endpointUrl);
	}

	if(!result.value("dryRun", true))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_flag_missing",
			"The data-connection endpoint response did not confirm dryRun=true.",
			endpointUrl);
	}

	if(result.value("committed", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_dry_run_committed_unexpected",
			"The data-connection endpoint reported committed=true during a dry run.",
			endpointUrl);
	}

	return result;
}

json BuildDataConnectionSavePlanExecutionResult(const json& savePlanJson)
{
	const std::size_t tableCount = DataConnectionSavePlanTableCount(savePlanJson);
	const std::size_t diagnosticCount = DataConnectionSavePlanDiagnosticCount(savePlanJson);

	json result = {
		{"configured", false},
		{"attempted", false},
		{"commitRequested", true},
		{"dryRun", false},
		{"committed", false},
		{"rolledBack", false},
		{"endpoint", nullptr},
		{"httpStatus", nullptr},
		{"succeeded", false},
		{"message", std::string()},
		{"operationCount", 0},
		{"rowsAffected", 0},
		{"updatedCount", 0},
		{"createdCount", 0},
		{"updateOnlyNotFoundCount", 0},
		{"conflictCount", 0},
		{"operations", json::array()},
		{"diagnostics", json::array()},
		{"requestTableCount", tableCount},
		{"requestDiagnosticCount", diagnosticCount}
	};

	if(tableCount == 0)
	{
		result["message"] = "No data-connection save-plan tables were available to execute.";
		AppendDataConnectionDryRunDiagnostic(
			result,
			"warning",
			"data_connection_execute_no_tables",
			"No data-connection save-plan tables were available to submit to the execution endpoint.",
			"sessionModel.dataConnectionSavePlan.tables");
		return result;
	}

	if(DataConnectionSavePlanHasErrorDiagnostics(savePlanJson))
	{
		result["diagnostics"] = JsonArrayValue(savePlanJson, {"diagnostics"});
		result["message"] = "Data-connection save was not attempted because the Session Manager save plan contains error diagnostics.";
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_save_plan_has_errors",
			"The Session Manager save plan contains error diagnostics, so it was not submitted to the data connector execution endpoint.",
			"sessionModel.dataConnectionSavePlan.diagnostics");
		return result;
	}

	std::string endpointUrl;

	try
	{
		endpointUrl = ResolveDataConnectionSavePlanExecuteUrl();
	}
	catch(const std::exception& e)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_endpoint_resolution_failed",
			std::string("The data-connection execution endpoint could not be resolved: ") + e.what(),
			"dataConnectionExecutionEndpoint");
		return result;
	}

	result["configured"] = !TrimAscii(endpointUrl).empty();
	result["endpoint"] = endpointUrl.empty() ? json(nullptr) : json(endpointUrl);

	ParsedHttpEndpoint endpoint = ParseHttpEndpoint(endpointUrl);

	if(!endpoint.valid)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_endpoint_invalid",
			endpoint.error,
			endpointUrl);
		return result;
	}

	result["attempted"] = true;

	json requestBody = {
		{"commit", true},
		{"tables", savePlanJson.value("tables", json::array())},
		{"diagnostics", savePlanJson.value("diagnostics", json::array())}
	};

	const DataConnectionHttpPostResult response = PostJsonToDataConnectionEndpoint(endpoint, requestBody.dump(), 30);

	if(!response.completed)
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_request_failed",
			"The data-connection execution endpoint did not return a response: " + response.error,
			endpointUrl);
		return result;
	}

	result["httpStatus"] = response.status;

	json connectorResponse = json::parse(response.body, nullptr, false);

	if(connectorResponse.is_discarded())
	{
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_response_invalid_json",
			"The data-connection execution endpoint did not return valid JSON.",
			endpointUrl);
		return result;
	}

	if(connectorResponse.is_object())
	{
		result["response"] = connectorResponse;
		result["succeeded"] = JsonBoolValue(connectorResponse, {"succeeded", "Succeeded"}).value_or(response.status >= 200 && response.status < 300);
		result["dryRun"] = JsonBoolValue(connectorResponse, {"dryRun", "DryRun"}).value_or(false);
		result["committed"] = JsonBoolValue(connectorResponse, {"committed", "Committed"}).value_or(false);
		result["rolledBack"] = JsonBoolValue(connectorResponse, {"rolledBack", "RolledBack"}).value_or(false);
		result["message"] = JsonStringValue(connectorResponse, {"message", "Message"});
		result["operations"] = JsonArrayValue(connectorResponse, {"operations", "Operations"});
		result["diagnostics"] = JsonArrayValue(connectorResponse, {"diagnostics", "Diagnostics"});
		result["operationCount"] = JsonIntValue(connectorResponse, {"operationCount", "OperationCount"}).value_or(static_cast<int>(result["operations"].size()));
		result["rowsAffected"] = JsonIntValue(connectorResponse, {"rowsAffected", "RowsAffected"}).value_or(0);
		result["updatedCount"] = JsonIntValue(connectorResponse, {"updatedCount", "UpdatedCount"}).value_or(0);
		result["createdCount"] = JsonIntValue(connectorResponse, {"createdCount", "CreatedCount"}).value_or(0);
		result["updateOnlyNotFoundCount"] = JsonIntValue(connectorResponse, {"updateOnlyNotFoundCount", "UpdateOnlyNotFoundCount"}).value_or(0);
		result["conflictCount"] = JsonIntValue(connectorResponse, {"conflictCount", "ConflictCount"}).value_or(0);
	}

	if(response.status < 200 || response.status >= 300)
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_http_error",
			"The data-connection execution endpoint returned HTTP " + std::to_string(response.status) + ".",
			endpointUrl);
	}

	if(result.value("dryRun", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_remained_dry_run",
			"The data-connection execution endpoint response still reported dryRun=true after an explicit commit request.",
			endpointUrl);
	}

	if(result.value("succeeded", false) && !result.value("committed", false))
	{
		result["succeeded"] = false;
		AppendDataConnectionDryRunDiagnostic(
			result,
			"error",
			"data_connection_execute_commit_not_confirmed",
			"The data-connection execution endpoint reported success without confirming committed=true.",
			endpointUrl);
	}

	return result;
}

void ApplyDataConnectionSaveGeneratedValueHydration(RuleSet& ruleSet, json& executionResult);

bool DataConnectionSaveExecutionCommittedSuccessfully(const json& execution)
{
	return execution.is_object()
		&& execution.value("succeeded", false)
		&& execution.value("committed", false)
		&& !execution.value("rolledBack", false);
}

bool DataConnectionDeleteExecutionCommittedSuccessfully(const json& execution)
{
	return execution.is_object()
		&& execution.value("succeeded", false)
		&& execution.value("deleteExecuted", false)
		&& execution.value("committed", false)
		&& !execution.value("rolledBack", false);
}

json BuildDataConnectionDeleteSkippedAfterSaveFailureResult()
{
	json result = {
		{"configured", false},
		{"attempted", false},
		{"executeRequested", true},
		{"dryRun", false},
		{"deleteExecuted", false},
		{"committed", false},
		{"rolledBack", false},
		{"endpoint", nullptr},
		{"httpStatus", nullptr},
		{"succeeded", false},
		{"message", "Deleted-instance rows were not deleted because the data-connection save did not commit successfully."},
		{"operationCount", 0},
		{"rowsAffected", 0},
		{"deletedCount", 0},
		{"operations", json::array()},
		{"diagnostics", json::array()}
	};

	AppendDataConnectionDryRunDiagnostic(
		result,
		"warning",
		"data_connection_deleted_instance_delete_skipped_save_failed",
		"Deleted-instance rows were not submitted to the delete endpoint because the save execution did not commit successfully.",
		"dataConnectionSave.execution");

	return result;
}

json BuildDataConnectionSavePreviewJson(RuleSet* ruleSet, const json& savePlan, bool commitRequested)
{
	const json deletePlan = ruleSet != nullptr
		? ruleSet->BuildDataConnectionDeletedInstanceDeletePlan().ToJSON()
		: json::object();
	const bool hasSavePlan = DataConnectionSavePlanHasContent(savePlan);
	const bool hasDeletePlan = DataConnectionLoadPlanHasContent(deletePlan);

	json result = {
		{"savePlan", savePlan},
		{"commitRequested", commitRequested}
	};

	if(hasDeletePlan)
	{
		result["deletePlan"] = deletePlan;
	}

	if(commitRequested)
	{
		bool saveCommitted = !hasSavePlan;

		if(hasSavePlan)
		{
			json execution = BuildDataConnectionSavePlanExecutionResult(savePlan);
			if(ruleSet != nullptr
				&& execution.value("succeeded", false)
				&& execution.value("committed", false)
				&& execution.value("createdCount", 0) > 0)
			{
				ApplyDataConnectionSaveGeneratedValueHydration(*ruleSet, execution);
			}

			saveCommitted = DataConnectionSaveExecutionCommittedSuccessfully(execution);
			result["execution"] = execution;
		}

		if(hasDeletePlan)
		{
			json deleteExecution = saveCommitted
				? BuildDataConnectionDeletePlanExecutionResult(deletePlan)
				: BuildDataConnectionDeleteSkippedAfterSaveFailureResult();

			if(ruleSet != nullptr && DataConnectionDeleteExecutionCommittedSuccessfully(deleteExecution))
			{
				deleteExecution["clearedRecordIdentitySnapshotCount"] = ruleSet->ClearDataConnectionSnapshotsForMissingInstances();
			}

			result["deleteExecution"] = deleteExecution;
		}
	}
	else if(hasSavePlan)
	{
		result["dryRun"] = BuildDataConnectionSavePlanDryRunResult(savePlan);
	}

	return result;
}

bool DataConnectionSavePreviewCommittedSuccessfully(const json& preview)
{
	if(!preview.is_object())
	{
		return false;
	}

	const bool hasSavePlan = preview.contains("savePlan")
		&& DataConnectionSavePlanHasContent(preview.at("savePlan"));
	const bool hasDeletePlan = preview.contains("deletePlan")
		&& DataConnectionLoadPlanHasContent(preview.at("deletePlan"));

	if(!hasSavePlan && !hasDeletePlan)
	{
		return false;
	}

	if(hasSavePlan
		&& (!preview.contains("execution")
			|| !DataConnectionSaveExecutionCommittedSuccessfully(preview.at("execution"))))
	{
		return false;
	}

	if(hasDeletePlan
		&& (!preview.contains("deleteExecution")
			|| !DataConnectionDeleteExecutionCommittedSuccessfully(preview.at("deleteExecution"))))
	{
		return false;
	}

	return true;
}

std::string AttributeStateSignature(const std::shared_ptr<Attribute>& attribute)
{
	if(attribute == nullptr)
	{
		return "";
	}

	return attribute->CreateJSON().dump();
}

json ValidationEventToJson(const RuleSet::ValidationEvent& event)
{
	return json{
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

json BuildActiveValidationEventsJson(const RuleSet& ruleSet, bool& hasBlockingErrors)
{
	hasBlockingErrors = false;

	json active = json::array();
	json errors = json::array();
	json warnings = json::array();

	if(ruleSet.activeValidationEventsByKey != nullptr)
	{
		for(const auto& entry : *ruleSet.activeValidationEventsByKey)
		{
			const json eventJson = ValidationEventToJson(entry.second);

			active.push_back(eventJson);

			if(entry.second.severity == "error")
			{
				hasBlockingErrors = true;
				errors.push_back(eventJson);
			}
			else if(entry.second.severity == "warning")
			{
				warnings.push_back(eventJson);
			}
		}
	}

	return json{
		{"active", active},
		{"errors", errors},
		{"warnings", warnings}
	};
}

bool ScreenHasElementErrors(const ScreenFile::Screen& screen)
{
	const auto hasErrors = [&](const auto& self, const ScreenFile::Element& element) -> bool {
		if(element.errors.has_value() && !element.errors->empty())
		{
			return true;
		}

		for(const ScreenFile::Element& child : element.children)
		{
			if(self(self, child))
			{
				return true;
			}
		}

		return false;
	};

	for(const ScreenFile::Element& element : screen.elements)
	{
		if(hasErrors(hasErrors, element))
		{
			return true;
		}
	}

	return false;
}

std::unordered_map<std::string, std::string> SubmittedValueMap(const ScreenData::DataRoot& data)
{
	std::unordered_map<std::string, std::string> values;

	for(const ScreenData::Data& item : data.data)
	{
		if(!item.attributeId.empty())
		{
			values[item.attributeId] = item.value;
		}
	}

	return values;
}

SessionModelRuleEvaluator::EvaluationStats EvaluateScreenLoadRules(RuleSet& ruleSet);
void MergeEvaluationStats(SessionModelRuleEvaluator::EvaluationStats& target, const SessionModelRuleEvaluator::EvaluationStats& source);
std::shared_ptr<ScreenFile::Screen> RefreshScreenForInstance(DataManager& dataManager,
                                                             RuleSet& ruleSet,
                                                             std::shared_ptr<ScreenFile::Screen> screen,
                                                             const std::string& entityId,
                                                             const std::string& instanceId,
                                                             const ScreenFile::ScreenData* screenData = nullptr,
                                                             SessionModelRuleEvaluator::EvaluationStats* accumulatedStats = nullptr,
                                                             bool* sessionModelEvaluationApplied = nullptr)
{
	if(screen == nullptr)
	{
		return nullptr;
	}

	const ScreenFile::Screen authoredScreen = *screen;
	EvaluateScreenLoadRules(ruleSet);
	for(size_t pass = 0; pass < 32; ++pass)
	{
		screen = make_shared<ScreenFile::Screen>(authoredScreen);
		screen->entityId = entityId;
		screen->instanceId = instanceId;
		auto ruleSetPtr = BorrowRuleSet(ruleSet);
		screen = dataManager.GetSessionData(ruleSet.entities, screen, entityId, instanceId, ruleSetPtr, screenData);
		dataManager.SetDisplayElement(ruleSet.entities, screen);

		const vector<DataManager::RadioSelectionChange> changes = dataManager.ClearHiddenRadioSelections(ruleSet.entities, screen);
		if(changes.empty())
		{
			return screen;
		}

		if(ruleSet.sessionModelRules != nullptr && !ruleSet.sessionModelRules->empty())
		{
			SessionModelRuleEvaluator evaluator;
			for(const DataManager::RadioSelectionChange& change : changes)
			{
				const SessionModelRuleEvaluator::EvaluationStats stats = evaluator.EvaluateChangedAttribute(
					ruleSet,
					change.entityId,
					change.instanceId,
					change.attributeId);
				if(accumulatedStats != nullptr)
				{
					MergeEvaluationStats(*accumulatedStats, stats);
				}
			}

			if(sessionModelEvaluationApplied != nullptr)
			{
				*sessionModelEvaluationApplied = true;
			}
		}
	}

	// Fail safely if a malformed model keeps hiding newly selected values. Return a
	// freshly populated view of the final native state rather than the last pre-clear
	// rendering, without extending the request indefinitely.
	screen = make_shared<ScreenFile::Screen>(authoredScreen);
	screen->entityId = entityId;
	screen->instanceId = instanceId;
	auto ruleSetPtr = BorrowRuleSet(ruleSet);
	screen = dataManager.GetSessionData(ruleSet.entities, screen, entityId, instanceId, ruleSetPtr, screenData);
	dataManager.SetDisplayElement(ruleSet.entities, screen);
	return screen;
}

bool MatchesValidationPattern(const std::string& value, const std::string& pattern)
{
	try
	{
		return std::regex_match(value, std::regex(pattern, std::regex_constants::ECMAScript));
	}
	catch(const std::regex_error&)
	{
		return true;
	}
}

bool IsValidatedInputElement(const ScreenFile::Element& element)
{
	return element.controlType == "question"
		|| element.boundAttributeId.has_value()
		|| IsEmailControlType(element.controlType);
}

std::string lowercase_copy(std::string value);

int CompareSelectionErrorValues(const std::string& left, const std::string& right)
{
	try
	{
		size_t leftUsed = 0;
		size_t rightUsed = 0;
		const double leftNumber = std::stod(left, &leftUsed);
		const double rightNumber = std::stod(right, &rightUsed);
		if(leftUsed == left.size() && rightUsed == right.size())
		{
			return leftNumber < rightNumber ? -1 : (leftNumber > rightNumber ? 1 : 0);
		}
	}
	catch(const std::exception&)
	{
	}

	return left < right ? -1 : (left > right ? 1 : 0);
}

bool MatchesCollectSelectionError(const std::shared_ptr<Instance>& instance,
	                              const ScreenFile::CollectData::SelectionError& error)
{
	if(instance == nullptr || error.attributeId.empty())
	{
		return false;
	}

	const std::shared_ptr<Attribute> attribute = instance->GetAttributeByID(error.attributeId);
	const std::string op = lowercase_copy(error.operatorName);
	if(op == "isknown")
	{
		return attribute != nullptr && attribute->IsKnown();
	}
	if(op == "isunknown")
	{
		return attribute == nullptr || attribute->IsUnknown();
	}
	if(attribute == nullptr || !attribute->IsCertain())
	{
		return false;
	}

	const std::string value = attribute->GetStringValue();
	const std::string normalizedValue = lowercase_copy(value);
	if(op == "istrue")
	{
		return normalizedValue == "true";
	}
	if(op == "isfalse")
	{
		return normalizedValue == "false";
	}
	if(op == "equals")
	{
		return value == error.value || normalizedValue == lowercase_copy(error.value);
	}
	if(op == "notequals")
	{
		return value != error.value && normalizedValue != lowercase_copy(error.value);
	}

	const int comparison = CompareSelectionErrorValues(value, error.value);
	if(op == "greaterthan") return comparison > 0;
	if(op == "greaterthanorequal" || op == "greaterthanequals") return comparison >= 0;
	if(op == "lessthan") return comparison < 0;
	if(op == "lessthanorequal" || op == "lessthanequals") return comparison <= 0;
	return false;
}

void AddUniqueElementError(ScreenFile::Element& element, const std::string& message)
{
	if(!element.errors.has_value())
	{
		element.errors = make_optional<vector<ScreenFile::Error>>();
	}

	const bool alreadyPresent = std::any_of(
		element.errors->begin(),
		element.errors->end(),
		[&message](const ScreenFile::Error& error) { return error.errorMessage == message; });
	if(!alreadyPresent)
	{
		element.errors->push_back(ScreenFile::Error{message});
	}
}

void AddSubmittedValidationError(ScreenFile::Element& element, const std::string& fallbackMessage)
{
	const std::string message = element.validationMessage.has_value() && !element.validationMessage->empty()
		? element.validationMessage.value()
		: fallbackMessage;
	AddUniqueElementError(element, message);
}

bool CheckboxGroupChildrenAreAllCertainlyFalse(const ScreenFile::Element& group,
	                                            const std::shared_ptr<Instance>& instance)
{
	bool foundDisplayedCheckbox = false;
	for(const ScreenFile::Element& child : group.children)
	{
		const std::string controlType = lowercase_copy(child.controlType);
		const bool isCheckbox = controlType == "checkbox" || controlType == "boolean" || controlType == "bool";
		if(!isCheckbox || !child.display.value_or(true))
		{
			continue;
		}

		foundDisplayedCheckbox = true;
		const std::string attributeId = child.boundAttributeId.value_or(child.id.value_or(""));
		if(attributeId.empty())
		{
			return false;
		}

		const std::shared_ptr<Attribute> attribute = instance->GetAttributeByID(attributeId);
		if(attribute == nullptr || !attribute->IsCertain() || lowercase_copy(attribute->GetStringValue()) != "false")
		{
			return false;
		}
	}

	return foundDisplayedCheckbox;
}

void ValidateSubmittedScreen(ScreenFile::Screen& screen,
                             const std::shared_ptr<Instance>& instance,
                             const std::unordered_map<std::string, std::string>& submittedValues)
{
	if(instance == nullptr)
	{
		return;
	}

	const auto validateElement = [&](const auto& self, ScreenFile::Element& element, bool ancestorsDisplayed) -> void {
		element.errors = make_optional<vector<ScreenFile::Error>>();

		if(IsValidatedInputElement(element) && (element.id.has_value() || element.boundAttributeId.has_value()))
		{
			const bool hidden = !ancestorsDisplayed || (element.display.has_value() && !element.display.value());

			if(!hidden)
			{
				const std::string attributeId = element.boundAttributeId.value_or(element.id.value_or(""));
				std::shared_ptr<Attribute> attribute = instance->GetAttributeByID(attributeId);
				const auto submitted = submittedValues.find(attributeId);
				const bool submittedEmpty = submitted != submittedValues.end() && submitted->second.empty();
				const bool valueMissing = attribute == nullptr || attribute->IsUnknown() || submittedEmpty;
				const std::string currentValue = submitted != submittedValues.end()
					? submitted->second
					: (attribute != nullptr && attribute->IsKnown() ? attribute->GetStringValue() : std::string());

				if(element.required.value_or(false) && valueMissing)
				{
					AddSubmittedValidationError(element, "This field is required.");
				}

				bool hasPatternConstraint = false;

				if(element.constraints.has_value())
				{
					for(const ScreenFile::Constraint& constraint : element.constraints.value())
					{
						if(constraint.maxCharacters.has_value()
							&& submitted != submittedValues.end()
							&& submitted->second.length() > static_cast<size_t>(constraint.maxCharacters.value()))
						{
							AddSubmittedValidationError(element, "Exceeds maximum character limit of " + to_string(constraint.maxCharacters.value()) + ".");
						}

						if(constraint.pattern.has_value() && !constraint.pattern->empty())
						{
							hasPatternConstraint = true;

							if(!currentValue.empty() && !MatchesValidationPattern(currentValue, constraint.pattern.value()))
							{
								AddSubmittedValidationError(element, "This field is not in the correct format.");
							}
						}
					}
				}

				if(IsEmailControlType(element.controlType) && !hasPatternConstraint && !currentValue.empty() && !MatchesValidationPattern(currentValue, EmailValidationPattern))
				{
					AddSubmittedValidationError(element, "Enter an email address in the correct format.");
				}

				if(element.ruleErrors.has_value())
				{
					for(const ScreenFile::CollectData::SelectionError& ruleError : element.ruleErrors.value())
					{
						if(!ruleError.message.empty() && MatchesCollectSelectionError(instance, ruleError))
						{
							AddUniqueElementError(element, ruleError.message);
							break;
						}
					}
				}
			}
		}

		const bool hidden = !ancestorsDisplayed || (element.display.has_value() && !element.display.value());
		if(!hidden &&
		   lowercase_copy(element.controlType) == "checkbox-group" &&
		   element.required.value_or(false) &&
		   CheckboxGroupChildrenAreAllCertainlyFalse(element, instance))
		{
			AddSubmittedValidationError(element, "Select at least one option.");
		}

		if(!hidden && element.collectData.has_value() && element.collectData->select.value_or(false) &&
		   element.collectData->selectionRequired.value_or(false))
		{
			int selectedCount = element.collectData->selectedCount.value_or(0);
			if(selectedCount <= 0 && element.collectData->instances.has_value())
			{
				selectedCount = static_cast<int>(std::count_if(
					element.collectData->instances->begin(),
					element.collectData->instances->end(),
					[](const ScreenFile::CollectInstance& item) { return item.selected.value_or(false); }));
			}

			if(selectedCount <= 0)
			{
				std::string message = "Select an option";
				if(element.collectData->selectionErrors.has_value())
				{
					for(const ScreenFile::CollectData::SelectionError& error : element.collectData->selectionErrors.value())
					{
						if(!error.message.empty() && MatchesCollectSelectionError(instance, error))
						{
							message = error.message;
							break;
						}
					}
				}
				element.errors->push_back(ScreenFile::Error{message});
			}
		}

		for(ScreenFile::Element& child : element.children)
		{
			const bool elementDisplayed = ancestorsDisplayed && element.display.value_or(true);
			self(self, child, elementDisplayed);
		}
	};

	for(ScreenFile::Element& element : screen.elements)
	{
		validateElement(validateElement, element, true);
	}
}

struct NavigationTarget
{
	std::shared_ptr<ScreenFile::Screen> screen;
	std::string entityId;
	std::string instanceId;
	std::string transitionId;
	std::string transitionLabel;
	std::string transitionAction;
	bool usedFlow = false;
};

std::string lowercase_copy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});

	return value;
}

bool ScreenStaticCanBeNavigatedTo(const ScreenFile::Screen& screen)
{
	if(screen.always.value_or(false))
	{
		return true;
	}

	return screen.relevant.value_or(true);
}

std::shared_ptr<ScreenFile::Screen> FindScreenById(const ScreenFile::ScreenData& screens, const std::string& screenId)
{
	return screens.GetScreenByID(screenId);
}

std::string ResolveScreenInstanceId(DataManager& dataManager,
                                    RuleSet& ruleSet,
                                    const ScreenFile::Screen& screen,
                                    const std::string& currentEntityId,
                                    const std::string& currentInstanceId)
{
	if(screen.instanceId.has_value() && !screen.instanceId->empty())
	{
		return screen.instanceId.value();
	}

	if(!screen.entityId.empty() && screen.entityId == currentEntityId && !currentInstanceId.empty())
	{
		return currentInstanceId;
	}

	return dataManager.GetFirstInstanceId(ruleSet.entities, screen.entityId);
}

bool AttributeMatchesScreenVisibility(const std::shared_ptr<Attribute>& attribute,
                                      const ScreenFile::Screen::VisibilityPredicate& visibility)
{
	const std::string op = lowercase_copy(visibility.operatorName);

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

	const std::string value = attribute->GetStringValue();

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

bool ScreenCanBeNavigatedTo(const ScreenFile::Screen& screen,
                            RuleSet& ruleSet,
                            DataManager& dataManager,
                            const std::string& entityId,
                            const std::string& instanceId)
{
	if(!ScreenStaticCanBeNavigatedTo(screen))
	{
		return false;
	}

	if(!screen.visibility.has_value())
	{
		return true;
	}

	const std::string visibilityEntityId = !screen.visibility->entityId.empty() ? screen.visibility->entityId : entityId;
	const std::string visibilityInstanceId = visibilityEntityId == entityId
		? instanceId
		: dataManager.GetFirstInstanceId(ruleSet.entities, visibilityEntityId);

	if(visibilityEntityId.empty() || visibilityInstanceId.empty())
	{
		return false;
	}

	std::shared_ptr<Instance> instance = dataManager.GetEntityInstanceById(ruleSet.entities, visibilityEntityId, visibilityInstanceId);

	if(instance == nullptr)
	{
		return false;
	}

	return AttributeMatchesScreenVisibility(instance->GetAttributeByID(screen.visibility->attributeId), screen.visibility.value());
}

ScreenLocation LatestScreenLocation(const RuleSet& ruleSet)
{
	ScreenLocation location;

	if(ruleSet.screenLocations != nullptr && !ruleSet.screenLocations->empty())
	{
		location = ruleSet.screenLocations->back();
	}

	return location;
}

const ScreenFile::FlowScreen* FindFlowScreen(const ScreenFile::ScreenData& screens, const std::string& screenId)
{
	if(!screens.flow.has_value())
	{
		return nullptr;
	}

	for(const ScreenFile::FlowScreen& screen : screens.flow->screens)
	{
		if(screen.screenId == screenId)
		{
			return &screen;
		}
	}

	return nullptr;
}

const ScreenFile::FlowTransition* FindFlowTransition(const ScreenFile::ScreenData& screens, const std::string& transitionId)
{
	if(transitionId.empty() || !screens.flow.has_value())
	{
		return nullptr;
	}
	for(const ScreenFile::FlowScreen& screen : screens.flow->screens)
	{
		for(const ScreenFile::FlowTransition& transition : screen.transitions)
		{
			if(transition.transitionId == transitionId)
			{
				return &transition;
			}
		}
	}
	return nullptr;
}

void TrimCommittedEmailCommands(std::unordered_map<std::uint64_t, json>& commands, std::size_t maximum)
{
	while(commands.size() > maximum)
	{
		const auto oldest = std::min_element(commands.begin(), commands.end(), [](const auto& left, const auto& right) {
			return left.first < right.first;
		});
		if(oldest == commands.end())
		{
			break;
		}
		commands.erase(oldest);
	}
}

std::vector<ScreenFile::FlowScreen> OrderedFlowScreens(const ScreenFile::ScreenData& screens)
{
	if(!screens.flow.has_value())
	{
		return {};
	}

	std::vector<ScreenFile::FlowScreen> result = screens.flow->screens;

	std::stable_sort(result.begin(), result.end(), [](const ScreenFile::FlowScreen& left, const ScreenFile::FlowScreen& right) {
		return left.order < right.order;
	});

	return result;
}

std::vector<ScreenFile::FlowTransition> OrderedTransitions(const ScreenFile::FlowScreen& flowScreen)
{
	std::vector<ScreenFile::FlowTransition> result = flowScreen.transitions;

	std::stable_sort(result.begin(), result.end(), [](const ScreenFile::FlowTransition& left, const ScreenFile::FlowTransition& right) {
		return left.order < right.order;
	});

	return result;
}

bool IsAddFlowTransition(const ScreenFile::FlowTransition& transition)
{
	return lowercase_copy(transition.transitionRole) == "add";
}

string ResolveSubmitForgetPolicyMode(const ScreenFile::ScreenData& screens, const RuleSet& ruleSet, const string& screenId)
{
	const string inheritedMode = ruleSet.forgetPolicyEnabled ? "Apply" : "Skip";
	const ScreenFile::FlowScreen* flowScreen = FindFlowScreen(screens, screenId);

	if(flowScreen == nullptr)
	{
		return inheritedMode;
	}

	string resolvedMode;
	for(const ScreenFile::FlowTransition& transition : OrderedTransitions(*flowScreen))
	{
		if(IsAddFlowTransition(transition))
		{
			continue;
		}

		const string normalized = lowercase_copy(transition.forgetPolicyMode);
		const string transitionMode = normalized == "apply"
			? "Apply"
			: (normalized == "skip" ? "Skip" : inheritedMode);

		if(resolvedMode.empty())
		{
			resolvedMode = transitionMode;
		}
		else if(resolvedMode != transitionMode)
		{
			// Import validation rejects conflicts. Skip is the conservative fallback for manually assembled runtime data.
			return "Skip";
		}
	}

	return resolvedMode.empty() ? inheritedMode : resolvedMode;
}

bool FlowTransitionHasCondition(const ScreenFile::FlowTransition& transition)
{
	if(transition.condition.is_null())
	{
		return false;
	}

	if(transition.condition.is_object())
	{
		if(transition.condition.empty())
		{
			return false;
		}

		if(transition.condition.contains("ast") && transition.condition.at("ast").is_null())
		{
			return false;
		}
	}

	return true;
}

bool FlowTransitionConditionMatches(RuleSet& ruleSet,
                                    const ScreenFile::FlowTransition& transition,
                                    const std::string& entityId,
                                    const std::string& instanceId)
{
	if(!FlowTransitionHasCondition(transition))
	{
		return true;
	}

	if(entityId.empty() || instanceId.empty())
	{
		return false;
	}

	SessionModelRuleEvaluator evaluator;

	return evaluator.EvaluateFlowCondition(ruleSet, entityId, instanceId, transition.condition);
}

bool IsDataSubmitTransitionAction(const std::string& action)
{
	const std::string normalized = lowercase_copy(action);

	return normalized == "datasubmit"
		|| normalized == "data-submit"
		|| normalized == "data submit";
}

bool IsCollectListControlElement(const ScreenFile::Element& element)
{
	const std::string controlType = lowercase_copy(element.controlType);

	return controlType == "collect"
		|| controlType == "collectview"
		|| controlType == "collect-view"
		|| controlType == "entity-instance-list";
}

bool IsReadOnlyCollectListControlElement(const ScreenFile::Element& element)
{
	const std::string controlType = lowercase_copy(element.controlType);
	return controlType == "collectview" || controlType == "collect-view";
}

std::string NormalizeRelationshipCardinality(const std::string& value)
{
	std::string normalized;
	for(const unsigned char character : value)
	{
		if(std::isalnum(character))
		{
			normalized.push_back(static_cast<char>(std::tolower(character)));
		}
	}

	return normalized;
}

bool IsManyToOneRelationship(const Relationship& relationship)
{
	const std::string cardinality = NormalizeRelationshipCardinality(relationship.type);
	return cardinality == "m1" || cardinality == "manytoone";
}

bool IsManyToManyRelationship(const Relationship& relationship)
{
	const std::string cardinality = NormalizeRelationshipCardinality(relationship.type);
	return cardinality == "mm" || cardinality == "manytomany";
}

bool HasExactOutgoingRelationshipLink(const shared_ptr<Instance>& sourceInstance,
	const shared_ptr<Instance>& targetInstance,
	const std::string& relationshipId)
{
	if(sourceInstance == nullptr || targetInstance == nullptr)
	{
		return false;
	}

	for(const InstanceRelationshipLink& link : sourceInstance->relationshipLinks)
	{
		if(link.outgoing &&
		   link.relationshipId == relationshipId &&
		   link.sourceEntityId == sourceInstance->entityId &&
		   link.sourceInstanceId == sourceInstance->instanceId &&
		   link.targetEntityId == targetInstance->entityId &&
		   link.targetInstanceId == targetInstance->instanceId)
		{
			return true;
		}
	}

	return false;
}

bool InstancesRemainRelated(const shared_ptr<Instance>& left, const shared_ptr<Instance>& right)
{
	if(left == nullptr || right == nullptr)
	{
		return false;
	}

	for(const InstanceRelationshipLink& link : left->relationshipLinks)
	{
		const bool forward = link.sourceEntityId == left->entityId &&
			link.sourceInstanceId == left->instanceId &&
			link.targetEntityId == right->entityId &&
			link.targetInstanceId == right->instanceId;
		const bool reverse = link.sourceEntityId == right->entityId &&
			link.sourceInstanceId == right->instanceId &&
			link.targetEntityId == left->entityId &&
			link.targetInstanceId == left->instanceId;
		if(forward || reverse)
		{
			return true;
		}
	}

	return false;
}

void RemoveLinkedInstanceIfUnrelated(const shared_ptr<Instance>& instance, const shared_ptr<Instance>& linkedInstance)
{
	if(instance == nullptr || linkedInstance == nullptr || InstancesRemainRelated(instance, linkedInstance))
	{
		return;
	}

	instance->linkedInstances.erase(
		std::remove_if(
			instance->linkedInstances.begin(),
			instance->linkedInstances.end(),
			[&](const shared_ptr<Instance>& candidate)
			{
				return candidate != nullptr &&
					candidate->entityId == linkedInstance->entityId &&
					candidate->instanceId == linkedInstance->instanceId;
			}),
		instance->linkedInstances.end());
}

bool RemoveExactRelationshipMembership(const shared_ptr<Instance>& sourceInstance,
	const shared_ptr<Instance>& targetInstance,
	const std::string& relationshipId)
{
	if(sourceInstance == nullptr || targetInstance == nullptr || relationshipId.empty())
	{
		return false;
	}

	const auto sourceBefore = sourceInstance->relationshipLinks.size();
	sourceInstance->relationshipLinks.erase(
		std::remove_if(
			sourceInstance->relationshipLinks.begin(),
			sourceInstance->relationshipLinks.end(),
			[&](const InstanceRelationshipLink& link)
			{
				return link.outgoing &&
					link.relationshipId == relationshipId &&
					link.sourceEntityId == sourceInstance->entityId &&
					link.sourceInstanceId == sourceInstance->instanceId &&
					link.targetEntityId == targetInstance->entityId &&
					link.targetInstanceId == targetInstance->instanceId;
			}),
		sourceInstance->relationshipLinks.end());

	const auto targetBefore = targetInstance->relationshipLinks.size();
	targetInstance->relationshipLinks.erase(
		std::remove_if(
			targetInstance->relationshipLinks.begin(),
			targetInstance->relationshipLinks.end(),
			[&](const InstanceRelationshipLink& link)
			{
				return !link.outgoing &&
					link.relationshipId == relationshipId &&
					link.sourceEntityId == sourceInstance->entityId &&
					link.sourceInstanceId == sourceInstance->instanceId &&
					link.targetEntityId == targetInstance->entityId &&
					link.targetInstanceId == targetInstance->instanceId;
			}),
		targetInstance->relationshipLinks.end());

	const bool changed = sourceBefore != sourceInstance->relationshipLinks.size() ||
		targetBefore != targetInstance->relationshipLinks.size();
	if(changed)
	{
		sourceInstance->InvalidateRelationshipLinkIndex();
		targetInstance->InvalidateRelationshipLinkIndex();
		RemoveLinkedInstanceIfUnrelated(sourceInstance, targetInstance);
		RemoveLinkedInstanceIfUnrelated(targetInstance, sourceInstance);
	}

	return changed;
}

bool RelationshipLinkMatchesInstances(const InstanceRelationshipLink& link,
                                      const std::string& relationshipId,
                                      const shared_ptr<Instance>& parentInstance,
                                      const shared_ptr<Instance>& childInstance)
{
	if(parentInstance == nullptr || childInstance == nullptr || relationshipId.empty() || link.relationshipId != relationshipId)
	{
		return false;
	}

	return (link.sourceInstanceId == parentInstance->instanceId && link.targetInstanceId == childInstance->instanceId)
		|| (link.sourceInstanceId == childInstance->instanceId && link.targetInstanceId == parentInstance->instanceId);
}

bool CollectActionTargetBelongsToParent(const ScreenFile::Element& collectElement,
                                        const shared_ptr<Instance>& parentInstance,
                                        const shared_ptr<Instance>& childInstance)
{
	if(!collectElement.collectRelationshipId.has_value() || collectElement.collectRelationshipId->empty())
	{
		return true;
	}

	if(parentInstance == nullptr || childInstance == nullptr)
	{
		return false;
	}

	const std::string& relationshipId = collectElement.collectRelationshipId.value();

	for(const InstanceRelationshipLink& link : parentInstance->relationshipLinks)
	{
		if(RelationshipLinkMatchesInstances(link, relationshipId, parentInstance, childInstance))
		{
			return true;
		}
	}

	for(const InstanceRelationshipLink& link : childInstance->relationshipLinks)
	{
		if(RelationshipLinkMatchesInstances(link, relationshipId, parentInstance, childInstance))
		{
			return true;
		}
	}

	return false;
}

bool CollectRelationshipDefinitionMatchesContext(const Relationship& relationship,
                                                 const string& parentEntityId,
                                                 const string& childEntityId)
{
	return (relationship.sourceEntity == parentEntityId && relationship.targetEntity == childEntityId)
		|| (relationship.sourceEntity == childEntityId && relationship.targetEntity == parentEntityId);
}

bool TransitionMatchesCollectControl(const ScreenFile::FlowTransition& transition, const std::string& collectControlId)
{
	return collectControlId.empty()
		|| transition.collectControlId.empty()
		|| transition.collectControlId == collectControlId;
}

std::optional<ScreenFile::FlowTransition> FindAddFlowTransitionForCollectAction(const ScreenFile::ScreenData& screens,
                                                                                const std::string& screenId,
                                                                                const std::string& collectControlId,
                                                                                const std::string& transitionId)
{
	const ScreenFile::FlowScreen* flowScreen = FindFlowScreen(screens, screenId);

	if(flowScreen == nullptr)
	{
		return std::nullopt;
	}

	for(const ScreenFile::FlowTransition& transition : OrderedTransitions(*flowScreen))
	{
		if(!transitionId.empty() && transition.transitionId != transitionId)
		{
			continue;
		}

		if(!IsAddFlowTransition(transition))
		{
			continue;
		}

		if(!TransitionMatchesCollectControl(transition, collectControlId))
		{
			continue;
		}

		return transition;
	}

	return std::nullopt;
}

json BuildScreenActionsJson(RuleSet& ruleSet, const ScreenFile::Screen& screen, const ScreenFile::ScreenData& screenData)
{
	const bool hasPreviousScreen = ruleSet.screenLocations != nullptr && ruleSet.screenLocations->size() >= 2;
	json actions = {
		{"canUpdate", true},
		{"canSubmit", true},
		{"canGoBack", hasPreviousScreen || screen.back.has_value() || screen.returnTo.has_value()},
		{"isComplete", false}
	};

	const ScreenFile::FlowScreen* flowScreen = FindFlowScreen(screenData, screen.id);

	if(flowScreen != nullptr && !flowScreen->transitions.empty())
	{
		DataManager dataManager;
		ScreenLocation latestLocation = LatestScreenLocation(ruleSet);
		std::string evaluationEntityId = screen.entityId;
		std::string evaluationInstanceId = screen.instanceId.value_or(std::string());

		if(latestLocation.screenId == screen.id)
		{
			evaluationEntityId = latestLocation.entityId;
			evaluationInstanceId = latestLocation.instanceId;
		}

		if(evaluationInstanceId.empty() && !evaluationEntityId.empty())
		{
			evaluationInstanceId = dataManager.GetFirstInstanceId(ruleSet.entities, evaluationEntityId);
		}

		json transitions = json::array();

		for(const ScreenFile::FlowTransition& transition : OrderedTransitions(*flowScreen))
		{
			if(!IsAddFlowTransition(transition)
				&& !FlowTransitionConditionMatches(ruleSet, transition, evaluationEntityId, evaluationInstanceId))
			{
				continue;
			}

			json transitionJson = {
				{"transitionId", transition.transitionId},
				{"toScreenId", transition.toScreenId}
			};

			if(!transition.label.empty())
			{
				transitionJson["label"] = transition.label;
			}

			if(!transition.action.empty())
			{
				transitionJson["action"] = transition.action;
			}

			if(!transition.transitionRole.empty())
			{
				transitionJson["transitionRole"] = transition.transitionRole;
			}

			if(!transition.collectControlId.empty())
			{
				transitionJson["collectControlId"] = transition.collectControlId;
			}

			transitions.push_back(transitionJson);
		}

		actions["transitions"] = transitions;
	}

	return actions;
}

json BuildScreenActionsJson(RuleSet& ruleSet, const ScreenFile::Screen& screen)
{
	return BuildScreenActionsJson(ruleSet, screen, RuleServer::ruleServerScreenFile.screenData);
}

NavigationTarget MakeNavigationTarget(const ScreenFile::ScreenData& screens,
                                      RuleSet& ruleSet,
                                      DataManager& dataManager,
                                      const std::string& screenId,
                                      const std::string& currentEntityId,
                                      const std::string& currentInstanceId,
                                      bool usedFlow,
                                      const std::string& transitionId = std::string(),
                                      const std::string& transitionLabel = std::string(),
                                      const std::string& transitionAction = std::string())
{
	NavigationTarget target;
	target.screen = FindScreenById(screens, screenId);

	if(target.screen == nullptr)
	{
		return target;
	}

	target.entityId = target.screen->entityId;
	target.instanceId = ResolveScreenInstanceId(dataManager, ruleSet, *target.screen, currentEntityId, currentInstanceId);
	target.usedFlow = usedFlow;
	target.transitionId = transitionId;
	target.transitionLabel = transitionLabel;
	target.transitionAction = transitionAction;

	return target;
}

NavigationTarget FindNextScreen(const ScreenFile::ScreenData& screens,
                                RuleSet& ruleSet,
                                DataManager& dataManager,
                                const std::string& currentScreenId,
                                const std::string& currentEntityId,
                                const std::string& currentInstanceId,
                                std::unordered_set<std::string>& visited);

NavigationTarget FindFirstScreen(const ScreenFile::ScreenData& screens, RuleSet& ruleSet, DataManager& dataManager)
{
	if(screens.flow.has_value())
	{
		if(!screens.flow->startScreenId.empty())
		{
			NavigationTarget start = MakeNavigationTarget(screens, ruleSet, dataManager, screens.flow->startScreenId, std::string(), std::string(), true);

			if(start.screen != nullptr && ScreenCanBeNavigatedTo(*start.screen, ruleSet, dataManager, start.entityId, start.instanceId))
			{
				return start;
			}

			std::unordered_set<std::string> visited;
			visited.insert(screens.flow->startScreenId);

			const ScreenFile::FlowScreen* flowStart = FindFlowScreen(screens, screens.flow->startScreenId);

			if(flowStart != nullptr)
			{
				std::vector<ScreenFile::FlowTransition> transitions = OrderedTransitions(*flowStart);

				for(const ScreenFile::FlowTransition& transition : transitions)
				{
					if(IsAddFlowTransition(transition))
					{
						continue;
					}

					if(!FlowTransitionConditionMatches(ruleSet, transition, start.entityId, start.instanceId))
					{
						continue;
					}

					NavigationTarget next = MakeNavigationTarget(screens,
					                                             ruleSet,
					                                             dataManager,
					                                             transition.toScreenId,
					                                             start.entityId,
					                                             start.instanceId,
					                                             true,
					                                             transition.transitionId,
					                                             transition.label,
					                                             transition.action);

					if(next.screen != nullptr && ScreenCanBeNavigatedTo(*next.screen, ruleSet, dataManager, next.entityId, next.instanceId))
					{
						return next;
					}

					next = FindNextScreen(screens, ruleSet, dataManager, transition.toScreenId, next.entityId, next.instanceId, visited);

					if(next.screen != nullptr)
					{
						if(next.transitionId.empty())
						{
							next.transitionId = transition.transitionId;
							next.transitionLabel = transition.label;
							next.transitionAction = transition.action;
						}

						next.usedFlow = true;

						return next;
					}
				}
			}
		}

		std::vector<ScreenFile::FlowScreen> ordered = OrderedFlowScreens(screens);

		for(const ScreenFile::FlowScreen& flowScreen : ordered)
		{
			if(!flowScreen.isStartScreen.value_or(false))
			{
				continue;
			}

			NavigationTarget target = MakeNavigationTarget(screens, ruleSet, dataManager, flowScreen.screenId, std::string(), std::string(), true);

			if(target.screen != nullptr && ScreenCanBeNavigatedTo(*target.screen, ruleSet, dataManager, target.entityId, target.instanceId))
			{
				return target;
			}
		}

		for(const ScreenFile::FlowScreen& flowScreen : ordered)
		{
			NavigationTarget target = MakeNavigationTarget(screens, ruleSet, dataManager, flowScreen.screenId, std::string(), std::string(), true);

			if(target.screen != nullptr && ScreenCanBeNavigatedTo(*target.screen, ruleSet, dataManager, target.entityId, target.instanceId))
			{
				return target;
			}
		}
	}

	for(const ScreenFile::Screen& screen : screens.screens)
	{
		NavigationTarget target = MakeNavigationTarget(screens, ruleSet, dataManager, screen.id, std::string(), std::string(), false);

		if(target.screen != nullptr && ScreenCanBeNavigatedTo(*target.screen, ruleSet, dataManager, target.entityId, target.instanceId))
		{
			return target;
		}
	}

	return {};
}

NavigationTarget FindNextScreen(const ScreenFile::ScreenData& screens,
                                RuleSet& ruleSet,
                                DataManager& dataManager,
                                const std::string& currentScreenId,
                                const std::string& currentEntityId,
                                const std::string& currentInstanceId,
                                std::unordered_set<std::string>& visited)
{
	if(!visited.insert(currentScreenId).second)
	{
		return {};
	}

	const ScreenFile::FlowScreen* flowScreen = FindFlowScreen(screens, currentScreenId);

	if(flowScreen != nullptr)
	{
		for(const ScreenFile::FlowTransition& transition : OrderedTransitions(*flowScreen))
		{
			if(transition.toScreenId.empty() || IsAddFlowTransition(transition))
			{
				continue;
			}

			if(!FlowTransitionConditionMatches(ruleSet, transition, currentEntityId, currentInstanceId))
			{
				continue;
			}

			NavigationTarget target = MakeNavigationTarget(screens,
			                                               ruleSet,
			                                               dataManager,
			                                               transition.toScreenId,
			                                               currentEntityId,
			                                               currentInstanceId,
			                                               true,
			                                               transition.transitionId,
			                                               transition.label,
			                                               transition.action);

			if(target.screen != nullptr && ScreenCanBeNavigatedTo(*target.screen, ruleSet, dataManager, target.entityId, target.instanceId))
			{
				return target;
			}

			NavigationTarget descendant = FindNextScreen(screens,
			                                             ruleSet,
			                                             dataManager,
			                                             transition.toScreenId,
			                                             target.entityId.empty() ? currentEntityId : target.entityId,
			                                             target.instanceId.empty() ? currentInstanceId : target.instanceId,
			                                             visited);

			if(descendant.screen != nullptr)
			{
				if(descendant.transitionId.empty())
				{
					descendant.transitionId = transition.transitionId;
					descendant.transitionLabel = transition.label;
					descendant.transitionAction = transition.action;
				}

				descendant.usedFlow = true;

				return descendant;
			}
		}

		return {};
	}

	bool returnNext = false;

	for(const ScreenFile::Screen& screen : screens.screens)
	{
		if(screen.entityId != currentEntityId)
		{
			continue;
		}

		NavigationTarget target = MakeNavigationTarget(screens, ruleSet, dataManager, screen.id, currentEntityId, currentInstanceId, false);

		if(returnNext && target.screen != nullptr && ScreenCanBeNavigatedTo(*target.screen, ruleSet, dataManager, target.entityId, target.instanceId))
		{
			return target;
		}

		if(screen.id == currentScreenId)
		{
			returnNext = true;
		}
	}

	return {};
}

NavigationTarget FindNextScreen(const ScreenFile::ScreenData& screens,
                                RuleSet& ruleSet,
                                DataManager& dataManager,
                                const std::string& currentScreenId,
                                const std::string& currentEntityId,
                                const std::string& currentInstanceId)
{
	std::unordered_set<std::string> visited;

	return FindNextScreen(screens, ruleSet, dataManager, currentScreenId, currentEntityId, currentInstanceId, visited);
}

std::string normalize_history_text(const std::string& value)
{
	std::string normalized = value;

	normalized.erase(normalized.begin(), std::find_if(normalized.begin(), normalized.end(), [](unsigned char ch) {
		return !std::isspace(ch);
	}));

	normalized.erase(std::find_if(normalized.rbegin(), normalized.rend(), [](unsigned char ch) {
		return !std::isspace(ch);
	}).base(), normalized.end());

	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});

	return normalized;
}

std::string get_json_scalar_string(const json& value, const char* key)
{
	if(!value.is_object() || !value.contains(key) || value.at(key).is_null())
	{
		return "";
	}

	const json& item = value.at(key);

	if(item.is_string())
	{
		return item.get<std::string>();
	}

	if(item.is_boolean())
	{
		return item.get<bool>() ? "true" : "false";
	}

	return item.dump();
}

std::string first_json_scalar_string(const json& value, std::initializer_list<const char*> keys)
{
	for(const char* key : keys)
	{
		const std::string item = get_json_scalar_string(value, key);

		if(!item.empty())
		{
			return item;
		}
	}

	return "";
}

bool SubmitValueIsSupported(const json& value)
{
	return value.is_null() || value.is_string() || value.is_boolean() || value.is_number();
}

std::string SubmitValueToString(const json& value)
{
	if(value.is_null())
	{
		return "";
	}

	if(value.is_boolean())
	{
		return value.get<bool>() ? "true" : "false";
	}

	if(value.is_string())
	{
		return value.get<std::string>();
	}

	return value.dump();
}

bool TryGetSubmitAttributeId(const json& item,
                             std::initializer_list<const char*> keys,
                             std::string& attributeId,
                             std::string& errorCode,
                             std::string& errorMessage)
{
	for(const char* key : keys)
	{
		if(!item.contains(key))
		{
			continue;
		}

		const json& value = item.at(key);

		if(!SubmitValueIsSupported(value))
		{
			errorCode = "invalid_submit_attribute";
			errorMessage = "Submitted attribute identifiers must be scalar values.";

			return false;
		}

		attributeId = SubmitValueToString(value);

		return true;
	}

	attributeId.clear();

	return true;
}

bool ValidateSubmittedValueCollection(const json& requestJson, std::string& errorCode, std::string& errorMessage)
{
	const auto validateArray = [&](const char* propertyName) -> bool {
		if(!requestJson.contains(propertyName))
		{
			return true;
		}

		const std::string property(propertyName);
		const std::string invalidCode = property == "data" ? "invalid_submit_data" : "invalid_submit_attributes";
		const json& items = requestJson.at(propertyName);

		if(!items.is_array())
		{
			errorCode = invalidCode;
			errorMessage = "Submit " + property + " must be an array.";

			return false;
		}

		for(const json& item : items)
		{
			if(!item.is_object())
			{
				errorCode = invalidCode;
				errorMessage = "Submit " + property + " entries must be objects.";

				return false;
			}

			std::string attributeId;

			if(!TryGetSubmitAttributeId(item, {"attributeId", "attributeid", "id", "elementId", "elementid"}, attributeId, errorCode, errorMessage))
			{
				return false;
			}

			if(TrimAscii(attributeId).empty())
			{
				errorCode = "missing_submit_attribute";
				errorMessage = "Submitted values require an attributeId or equivalent element identifier.";

				return false;
			}

			if(!item.contains("value"))
			{
				errorCode = "missing_submit_value";
				errorMessage = "Submitted attribute values require a value field.";

				return false;
			}

			if(!SubmitValueIsSupported(item.at("value")))
			{
				errorCode = "invalid_submit_value";
				errorMessage = "Submitted attribute values must be scalar values.";

				return false;
			}
		}

		return true;
	};

	if(!validateArray("data") || !validateArray("attributes"))
	{
		return false;
	}

	if(requestJson.contains("values"))
	{
		const json& values = requestJson.at("values");

		if(!values.is_object())
		{
			errorCode = "invalid_submit_values";
			errorMessage = "Submit values must be an object keyed by attributeId.";

			return false;
		}

		for(auto it = values.begin(); it != values.end(); ++it)
		{
			if(TrimAscii(it.key()).empty())
			{
				errorCode = "missing_submit_attribute";
				errorMessage = "Submitted values require a non-empty attributeId key.";

				return false;
			}

			if(!SubmitValueIsSupported(it.value()))
			{
				errorCode = "invalid_submit_value";
				errorMessage = "Submitted attribute values must be scalar values.";

				return false;
			}
		}
	}

	return true;
}

Attribute::AttributeType parse_history_value_type(const std::string& value, Attribute::AttributeType fallback)
{
	const std::string normalized = normalize_history_text(value);

	if(normalized.empty())
	{
		return fallback;
	}

	if(normalized == "integer" || normalized == "int")
	{
		return Attribute::INTEGER;
	}

	if(normalized == "number" || normalized == "numeric" || normalized == "decimal" || normalized == "double" || normalized == "currency")
	{
		return Attribute::DOUBLE;
	}

	if(normalized == "text" || normalized == "string")
	{
		return Attribute::STRING;
	}

	if(normalized == "boolean" || normalized == "bool")
	{
		return Attribute::BOOLEAN;
	}

	if(normalized == "date")
	{
		return Attribute::DATE;
	}

	if(normalized == "datetime" || normalized == "date-time")
	{
		return Attribute::DATETIME;
	}

	if(normalized == "timeofday" || normalized == "time" || normalized == "time-of-day")
	{
		return Attribute::TIMEOFDAY;
	}

	return fallback;
}

Attribute::KnowledgeState parse_history_knownness(const std::string& value)
{
	const std::string normalized = normalize_history_text(value);

	if(normalized == "unknown")
	{
		return Attribute::KnowledgeState::Unknown;
	}

	if(normalized == "uncertain")
	{
		return Attribute::KnowledgeState::Uncertain;
	}

	return Attribute::KnowledgeState::Certain;
}

std::shared_ptr<Entity> find_ruleset_entity(const RuleSet& ruleSet, const std::string& entityId)
{
	DataManager dataManager;

	return dataManager.GetEntityByID(ruleSet.entities, entityId);
}

std::shared_ptr<Attribute> find_instance_attribute(const std::shared_ptr<Instance>& instance, const std::string& attributeId)
{
	return instance != nullptr ? instance->GetAttributeByID(attributeId) : nullptr;
}

std::shared_ptr<Relationship> find_ruleset_relationship(const RuleSet& ruleSet, const std::string& relationshipId)
{
	if(ruleSet.entities == nullptr)
	{
		return nullptr;
	}

	for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
	{
		if(entity == nullptr)
		{
			continue;
		}

		for(const std::shared_ptr<Relationship>& relationship : entity->relationships)
		{
			if(relationship != nullptr && relationship->id == relationshipId)
			{
				return relationship;
			}
		}
	}

	return nullptr;
}

std::string history_change_key(const std::string& entityId, const std::string& instanceId, const std::string& attributeId)
{
	return entityId + "\x1f" + instanceId + "\x1f" + attributeId;
}

struct SelectedFormRuntime
{
	bool selected{false};
	std::string formId;
	std::string formKey;
	std::string formName;
	std::string formDescription;
	std::string formVersionId;
	int versionNumber{0};
	std::string versionLabel;
	std::string sessionModelHash;
	std::string sessionModelSchemaVersion;
	std::string sourceName;
	std::string publishedAt;
	RuleSet::ExternalSecurityPolicy externalSecurity;
	RuleSet runtimeTemplate;
};

struct ExternalIdentityBinding
{
	bool enabled{false};
	std::string issuer;
	std::string subject;
	std::string tenantId;
	std::string providerSessionId;
	std::string launchId;
	std::string approvedProfileKey;
	std::string formKey;
	std::string formVersionId;
};

struct Session
{
	std::string client_id;

	std::string user_id;

	Clock::time_point created_at;

	Clock::time_point last_seen;

	std::uint64_t message_count{0};

	// Session copies share RuleSet internals; endpoint mutations persist through the shared vectors/maps.
	RuleSet ruleset;

	ScreenFile::ScreenData screenData;

	SelectedFormRuntime selectedForm;

	ExternalIdentityBinding externalIdentity;

	// Resolved commands are retained by committed response sequence so a stale retry returns the same idempotent command.
	std::unordered_map<std::uint64_t, json> emailCommandsBySequence;

	std::shared_ptr<std::recursive_mutex> state_mu = std::make_shared<std::recursive_mutex>();

	std::shared_ptr<std::unique_lock<std::recursive_mutex>> state_lock;
};

struct DebugSession
{
	std::string developerId;
	std::string projectId;
	std::string formKey;
	std::string formVersionId;
	Clock::time_point created_at;
	Clock::time_point last_seen;
	RuleSet templateRuleset;
	RuleSet ruleset;
	std::vector<SessionModelImporter::Diagnostic> diagnostics;
};

std::mutex g_sessions_mu;

std::unordered_map<std::string, Session> g_sessions; // token -> session

std::mutex g_debug_sessions_mu;

std::unordered_map<std::string, DebugSession> g_debug_sessions; // debugSessionId -> debug session

bool valid_credentials(const std::string& client_id, const std::string& client_secret)
{
	const std::string configuredClientId = get_env_or_default("CCP_CLIENT_ID", "");
	const std::string configuredClientSecret = get_env_or_default("CCP_CLIENT_SECRET", "");

	if(!configuredClientId.empty() || !configuredClientSecret.empty())
	{
		return client_id == configuredClientId && client_secret == configuredClientSecret;
	}

	return false;
}

struct LoginIdentity
{
	bool authenticated{false};
	std::string principal;
	std::string userId;
	std::string userName;
	std::string displayName;
	std::string failureCode;
	std::string failureMessage;
};

struct UserTableAuthSettings
{
	bool enabled{false};
	std::string sourceId{"auth-users"};
	std::string sourceName{"User credentials"};
	std::string sourceType{"Database"};
	std::string databaseProvider{"MySQL"};
	std::string serverConnectionKey{"mcmysqlwl"};
	std::string sourceObjectName{"demo.users"};
	std::string userIdFieldName{"user_id"};
	std::string usernameFieldName{"username"};
	std::string passwordHashFieldName{"password_hash"};
	std::string passwordSaltFieldName{"password_salt"};
	std::string passwordSchemeFieldName{"password_scheme"};
	std::string displayNameFieldName{"display_name"};
	std::string activeFieldName{"is_active"};
	std::string defaultPasswordScheme{"sha256"};
};

std::string EnvironmentOrConfigString(const char* environmentName,
                                      const json& object,
                                      std::initializer_list<const char*> keys,
                                      const std::string& fallback)
{
	const std::string environmentValue = TrimAscii(get_env_or_default(environmentName, ""));

	if(!environmentValue.empty())
	{
		return environmentValue;
	}

	const std::string configuredValue = TrimAscii(FirstConfigString(object, keys));

	return configuredValue.empty() ? fallback : configuredValue;
}

std::optional<bool> ConfigBooleanValue(const json& object, std::initializer_list<const char*> keys)
{
	for(const char* key : keys)
	{
		if(object.is_object() && object.contains(key) && !object.at(key).is_null())
		{
			std::optional<bool> parsed = BooleanOptionFromJson(object.at(key));

			if(parsed.has_value())
			{
				return parsed;
			}
		}
	}

	return std::nullopt;
}

json LoadRuleServerConfigJsonOrEmpty()
{
	try
	{
		const std::string configPath = ResolveRuleServerConfigPath();

		if(configPath.empty())
		{
			return json::object();
		}

		std::ifstream input(configPath);

		if(!input.is_open())
		{
			return json::object();
		}

		json config;
		input >> config;
		return config.is_object() ? config : json::object();
	}
	catch(const std::exception&)
	{
		return json::object();
	}
}

json UserTableAuthConfigFromRuleServerConfig(const json& config)
{
	if(config.is_object() && config.contains("authentication") && config.at("authentication").is_object())
	{
		const json& authentication = config.at("authentication");

		if(authentication.contains("users") && authentication.at("users").is_object())
		{
			return authentication.at("users");
		}

		if(authentication.contains("userTable") && authentication.at("userTable").is_object())
		{
			return authentication.at("userTable");
		}
	}

	if(config.is_object() && config.contains("userAuthentication") && config.at("userAuthentication").is_object())
	{
		return config.at("userAuthentication");
	}

	return json::object();
}

UserTableAuthSettings LoadUserTableAuthSettings()
{
	const json config = LoadRuleServerConfigJsonOrEmpty();
	const json authConfig = UserTableAuthConfigFromRuleServerConfig(config);

	UserTableAuthSettings settings;

	settings.enabled = EnvironmentBooleanOption("CCP_USER_AUTH_ENABLED")
		.value_or(ConfigBooleanValue(authConfig, {"enabled", "useUserTable", "databaseBacked"}).value_or(false));

	settings.sourceId = EnvironmentOrConfigString("CCP_USER_AUTH_SOURCE_ID", authConfig, {"sourceId"}, settings.sourceId);
	settings.sourceName = EnvironmentOrConfigString("CCP_USER_AUTH_SOURCE_NAME", authConfig, {"sourceName"}, settings.sourceName);
	settings.sourceType = EnvironmentOrConfigString("CCP_USER_AUTH_SOURCE_TYPE", authConfig, {"sourceType"}, settings.sourceType);
	settings.databaseProvider = EnvironmentOrConfigString("CCP_USER_AUTH_DATABASE_PROVIDER", authConfig, {"databaseProvider", "provider"}, settings.databaseProvider);
	settings.serverConnectionKey = EnvironmentOrConfigString("CCP_USER_AUTH_SERVER_CONNECTION_KEY", authConfig, {"serverConnectionKey", "connectionKey"}, settings.serverConnectionKey);
	settings.sourceObjectName = EnvironmentOrConfigString("CCP_USER_AUTH_SOURCE_OBJECT_NAME", authConfig, {"sourceObjectName", "table"}, settings.sourceObjectName);
	settings.userIdFieldName = EnvironmentOrConfigString("CCP_USER_AUTH_USER_ID_FIELD", authConfig, {"userIdFieldName", "userIdField"}, settings.userIdFieldName);
	settings.usernameFieldName = EnvironmentOrConfigString("CCP_USER_AUTH_USERNAME_FIELD", authConfig, {"usernameFieldName", "usernameField"}, settings.usernameFieldName);
	settings.passwordHashFieldName = EnvironmentOrConfigString("CCP_USER_AUTH_PASSWORD_HASH_FIELD", authConfig, {"passwordHashFieldName", "passwordHashField"}, settings.passwordHashFieldName);
	settings.passwordSaltFieldName = EnvironmentOrConfigString("CCP_USER_AUTH_PASSWORD_SALT_FIELD", authConfig, {"passwordSaltFieldName", "passwordSaltField"}, settings.passwordSaltFieldName);
	settings.passwordSchemeFieldName = EnvironmentOrConfigString("CCP_USER_AUTH_PASSWORD_SCHEME_FIELD", authConfig, {"passwordSchemeFieldName", "passwordSchemeField"}, settings.passwordSchemeFieldName);
	settings.displayNameFieldName = EnvironmentOrConfigString("CCP_USER_AUTH_DISPLAY_NAME_FIELD", authConfig, {"displayNameFieldName", "displayNameField"}, settings.displayNameFieldName);
	settings.activeFieldName = EnvironmentOrConfigString("CCP_USER_AUTH_ACTIVE_FIELD", authConfig, {"activeFieldName", "activeField"}, settings.activeFieldName);
	settings.defaultPasswordScheme = EnvironmentOrConfigString("CCP_USER_AUTH_PASSWORD_SCHEME", authConfig, {"passwordScheme", "defaultPasswordScheme"}, settings.defaultPasswordScheme);

	return settings;
}

void AddUserAuthLoadField(json& fields, const std::string& sourceFieldName, const std::string& valueType = "Text")
{
	if(sourceFieldName.empty())
	{
		return;
	}

	for(const json& field : fields)
	{
		if(field.is_object() && field.value("sourceFieldName", std::string()) == sourceFieldName)
		{
			return;
		}
	}

	fields.push_back({
		{"sourceFieldName", sourceFieldName},
		{"targets", json::array({
			{
				{"entityId", "__auth_user"},
				{"entityName", "Authentication user"},
				{"attributeId", "__auth_" + sourceFieldName},
				{"attributeName", sourceFieldName},
				{"attributeCoreName", sourceFieldName},
				{"attributeDataType", valueType},
				{"valueType", valueType}
			}
		})}
	});
}

json BuildUserAuthLoadPlan(const UserTableAuthSettings& settings, const std::string& username)
{
	json fields = json::array();
	AddUserAuthLoadField(fields, settings.userIdFieldName);
	AddUserAuthLoadField(fields, settings.usernameFieldName);
	AddUserAuthLoadField(fields, settings.passwordHashFieldName);
	AddUserAuthLoadField(fields, settings.passwordSaltFieldName);
	AddUserAuthLoadField(fields, settings.passwordSchemeFieldName);
	AddUserAuthLoadField(fields, settings.displayNameFieldName);
	AddUserAuthLoadField(fields, settings.activeFieldName, "Boolean");

	return {
		{"tables", json::array({
			{
				{"sourceId", settings.sourceId},
				{"sourceName", settings.sourceName},
				{"sourceType", settings.sourceType},
				{"databaseProvider", settings.databaseProvider},
				{"serverConnectionKey", settings.serverConnectionKey},
				{"sourceObjectName", settings.sourceObjectName},
				{"fields", fields},
				{"filters", json::array({
					{
						{"sourceFieldName", settings.usernameFieldName},
						{"operator", "Equals"},
						{"valueSource", "LoginUserName"},
						{"attributeDataType", "Text"},
						{"valueType", "Text"},
						{"value", username},
						{"isKnown", true},
						{"isCertain", true},
						{"isUncertain", false},
						{"knowledgeState", "Known"}
					}
				})}
			}
		})},
		{"diagnostics", json::array()}
	};
}

const json* FindLoadRowValueEnvelope(const json& row, const std::string& sourceFieldName)
{
	if(!row.is_object() || !row.contains("values") || !row.at("values").is_array())
	{
		return nullptr;
	}

	for(const json& value : row.at("values"))
	{
		if(value.is_object() && value.value("sourceFieldName", std::string()) == sourceFieldName)
		{
			return &value;
		}
	}

	return nullptr;
}

std::string LoadRowValueAsString(const json& row, const std::string& sourceFieldName)
{
	const json* envelope = FindLoadRowValueEnvelope(row, sourceFieldName);

	if(envelope == nullptr)
	{
		return "";
	}

	const json* rawValue = FindObjectValue(*envelope, {"value", "Value"});

	if(rawValue == nullptr || rawValue->is_null())
	{
		return "";
	}

	if(rawValue->is_string())
	{
		return rawValue->get<std::string>();
	}

	if(rawValue->is_boolean())
	{
		return rawValue->get<bool>() ? "1" : "0";
	}

	if(rawValue->is_number_integer())
	{
		return std::to_string(rawValue->get<long long>());
	}

	if(rawValue->is_number_unsigned())
	{
		return std::to_string(rawValue->get<unsigned long long>());
	}

	if(rawValue->is_number_float())
	{
		std::ostringstream oss;
		oss << rawValue->get<double>();
		return oss.str();
	}

	return rawValue->dump();
}

bool LoadRowValueAsBool(const json& row, const std::string& sourceFieldName, bool fallback)
{
	const json* envelope = FindLoadRowValueEnvelope(row, sourceFieldName);

	if(envelope == nullptr)
	{
		return fallback;
	}

	const json* rawValue = FindObjectValue(*envelope, {"value", "Value"});

	if(rawValue == nullptr || rawValue->is_null())
	{
		return fallback;
	}

	std::optional<bool> parsed = BooleanOptionFromJson(*rawValue);

	if(parsed.has_value())
	{
		return parsed.value();
	}

	return fallback;
}

bool EqualsIgnoreCase(std::string left, std::string right)
{
	std::transform(left.begin(), left.end(), left.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	std::transform(right.begin(), right.end(), right.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return left == right;
}

std::optional<std::string> Sha256Hex(const std::string& value)
{
#if defined(_WIN32)
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	DWORD objectLength = 0;
	DWORD hashLength = 0;
	DWORD bytesRead = 0;

	if(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
	{
		return std::nullopt;
	}

	auto closeAlgorithm = [&]() {
		if(algorithm != nullptr)
		{
			BCryptCloseAlgorithmProvider(algorithm, 0);
			algorithm = nullptr;
		}
	};

	if(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &bytesRead, 0) != 0 ||
	   BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &bytesRead, 0) != 0)
	{
		closeAlgorithm();
		return std::nullopt;
	}

	std::vector<unsigned char> hashObject(objectLength);
	std::vector<unsigned char> hashBytes(hashLength);

	if(BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) != 0)
	{
		closeAlgorithm();
		return std::nullopt;
	}

	auto destroyHash = [&]() {
		if(hash != nullptr)
		{
			BCryptDestroyHash(hash);
			hash = nullptr;
		}
	};

	if(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), 0) != 0 ||
	   BCryptFinishHash(hash, hashBytes.data(), hashLength, 0) != 0)
	{
		destroyHash();
		closeAlgorithm();
		return std::nullopt;
	}

	destroyHash();
	closeAlgorithm();

	static const char* hex = "0123456789abcdef";
	std::string out;
	out.reserve(hashBytes.size() * 2);

	for(unsigned char byte : hashBytes)
	{
		out.push_back(hex[(byte >> 4) & 0x0F]);
		out.push_back(hex[byte & 0x0F]);
	}

	return out;
#else
	(void)value;
	return std::nullopt;
#endif
}

bool VerifyUserPassword(const std::string& suppliedPassword,
                        const std::string& storedHash,
                        const std::string& salt,
                        const std::string& passwordScheme)
{
	const std::string scheme = NormalizeOptionValue(passwordScheme.empty() ? "sha256" : passwordScheme);

	if(scheme == "plain" || scheme == "plaintext" || scheme == "cleartext")
	{
		return suppliedPassword == storedHash;
	}

	if(scheme == "sha256" || scheme == "sha-256" || scheme == "sha256-salted")
	{
		const std::optional<std::string> computed = Sha256Hex(salt + suppliedPassword);
		return computed.has_value() && EqualsIgnoreCase(computed.value(), storedHash);
	}

	return false;
}

LoginIdentity TryAuthenticateUserFromTable(const std::string& username, const std::string& password)
{
	LoginIdentity identity;
	const UserTableAuthSettings settings = LoadUserTableAuthSettings();

	if(!settings.enabled)
	{
		identity.failureCode = "invalid_credentials";
		identity.failureMessage = "Username or password is incorrect.";
		return identity;
	}

	const std::string trimmedUsername = TrimAscii(username);

	if(trimmedUsername.empty() || password.empty())
	{
		identity.failureCode = "invalid_credentials";
		identity.failureMessage = "Username or password is empty.";
		return identity;
	}

	const json loadPlan = BuildUserAuthLoadPlan(settings, trimmedUsername);
	const json execution = BuildDataConnectionLoadPlanExecutionResult(loadPlan, 2);

	if(!execution.value("succeeded", false) || !execution.value("readExecuted", false))
	{
		identity.failureCode = "auth_unavailable";
		identity.failureMessage = "The user credential store could not be read.";
		return identity;
	}

	const json operations = JsonArrayValue(execution, {"operations"});

	if(operations.size() != 1 || !operations[0].is_object())
	{
		identity.failureCode = "auth_unavailable";
		identity.failureMessage = "The user credential store returned an unexpected response.";
		return identity;
	}

	const json rows = JsonArrayValue(operations[0], {"rows"});

	if(rows.size() != 1 || !rows[0].is_object())
	{
		identity.failureCode = "invalid_credentials";
		identity.failureMessage = "Username or password is incorrect.";
		return identity;
	}

	const json& row = rows[0];
	const std::string userId = TrimAscii(LoadRowValueAsString(row, settings.userIdFieldName));
	const std::string storedHash = LoadRowValueAsString(row, settings.passwordHashFieldName);
	const std::string salt = LoadRowValueAsString(row, settings.passwordSaltFieldName);
	const std::string scheme = LoadRowValueAsString(row, settings.passwordSchemeFieldName).empty()
		? settings.defaultPasswordScheme
		: LoadRowValueAsString(row, settings.passwordSchemeFieldName);
	const bool isActive = LoadRowValueAsBool(row, settings.activeFieldName, true);

	if(userId.empty() || storedHash.empty() || !isActive || !VerifyUserPassword(password, storedHash, salt, scheme))
	{
		identity.failureCode = "invalid_credentials";
		identity.failureMessage = "Username or password is incorrect.";
		return identity;
	}

	identity.authenticated = true;
	identity.principal = trimmedUsername;
	identity.userId = userId;
	identity.userName = trimmedUsername;
	identity.displayName = LoadRowValueAsString(row, settings.displayNameFieldName);
	return identity;
}

struct FormCatalogSettings
{
	bool enabled{true};
	std::string sourceType{"Database"};
	std::string databaseProvider{"MySQL"};
	std::string serverConnectionKey{"mcmysqlwl"};
	std::string formsSourceObjectName{"demo.forms"};
	std::string formVersionsSourceObjectName{"demo.form_versions"};
	std::string formPermissionsSourceObjectName{"demo.form_permissions"};
	int maxRows{500};
};

json FormCatalogConfigFromRuleServerConfig(const json& config)
{
	if(config.is_object() && config.contains("formCatalog") && config.at("formCatalog").is_object())
	{
		return config.at("formCatalog");
	}

	if(config.is_object() && config.contains("forms") && config.at("forms").is_object())
	{
		return config.at("forms");
	}

	return json::object();
}

FormCatalogSettings LoadFormCatalogSettings()
{
	const json config = LoadRuleServerConfigJsonOrEmpty();
	const json catalogConfig = FormCatalogConfigFromRuleServerConfig(config);

	FormCatalogSettings settings;

	settings.enabled = EnvironmentBooleanOption("CCP_FORM_CATALOG_ENABLED")
		.value_or(ConfigBooleanValue(catalogConfig, {"enabled"}).value_or(settings.enabled));
	settings.sourceType = EnvironmentOrConfigString("CCP_FORM_CATALOG_SOURCE_TYPE", catalogConfig, {"sourceType"}, settings.sourceType);
	settings.databaseProvider = EnvironmentOrConfigString("CCP_FORM_CATALOG_DATABASE_PROVIDER", catalogConfig, {"databaseProvider", "provider"}, settings.databaseProvider);
	settings.serverConnectionKey = EnvironmentOrConfigString("CCP_FORM_CATALOG_SERVER_CONNECTION_KEY", catalogConfig, {"serverConnectionKey", "connectionKey"}, settings.serverConnectionKey);
	settings.formsSourceObjectName = EnvironmentOrConfigString("CCP_FORM_CATALOG_FORMS_SOURCE_OBJECT_NAME", catalogConfig, {"formsSourceObjectName", "formsTable"}, settings.formsSourceObjectName);
	settings.formVersionsSourceObjectName = EnvironmentOrConfigString("CCP_FORM_CATALOG_FORM_VERSIONS_SOURCE_OBJECT_NAME", catalogConfig, {"formVersionsSourceObjectName", "formVersionsTable", "versionsTable"}, settings.formVersionsSourceObjectName);
	settings.formPermissionsSourceObjectName = EnvironmentOrConfigString("CCP_FORM_CATALOG_FORM_PERMISSIONS_SOURCE_OBJECT_NAME", catalogConfig, {"formPermissionsSourceObjectName", "formPermissionsTable", "permissionsTable"}, settings.formPermissionsSourceObjectName);
	settings.maxRows = get_env_int_or_default("CCP_FORM_CATALOG_MAX_ROWS", settings.maxRows, 1, 1000);

	if(catalogConfig.is_object())
	{
		const std::optional<int> configuredMaxRows = JsonIntValue(catalogConfig, {"maxRows"});
		if(configuredMaxRows.has_value())
		{
			settings.maxRows = std::max(1, std::min(1000, configuredMaxRows.value()));
		}
	}

	return settings;
}

std::string FormCatalogValueTypeForField(const std::string& sourceFieldName)
{
	if(sourceFieldName == "form_id" ||
	   sourceFieldName == "form_version_id" ||
	   sourceFieldName == "version_number" ||
	   sourceFieldName == "import_diagnostic_count")
	{
		return "Integer";
	}

	if(sourceFieldName == "published_at" ||
	   sourceFieldName == "created_at" ||
	   sourceFieldName == "updated_at")
	{
		return "DateTime";
	}

	return "Text";
}

json BuildFormCatalogTarget(const std::string& sourceFieldName, const std::string& valueType)
{
	const std::string attributeId = "__form_catalog_" + sourceFieldName;

	return {
		{"entityId", "__form_catalog"},
		{"entityName", "Form catalogue"},
		{"attributeId", attributeId},
		{"attributeName", sourceFieldName},
		{"attributeCoreName", sourceFieldName},
		{"attributeDataType", valueType},
		{"valueType", valueType}
	};
}

void AddFormCatalogLoadField(json& fields, const std::string& sourceFieldName)
{
	if(sourceFieldName.empty())
	{
		return;
	}

	const std::string valueType = FormCatalogValueTypeForField(sourceFieldName);

	fields.push_back({
		{"sourceFieldName", sourceFieldName},
		{"targets", json::array({BuildFormCatalogTarget(sourceFieldName, valueType)})}
	});
}

json BuildFormCatalogFilter(const std::string& sourceFieldName,
                            const json& value,
                            const std::string& valueType,
                            const std::string& valueSource)
{
	return {
		{"sourceFieldName", sourceFieldName},
		{"operator", "Equals"},
		{"valueSource", valueSource},
		{"entityId", "__form_catalog"},
		{"entityName", "Form catalogue"},
		{"attributeId", "__form_catalog_filter_" + sourceFieldName},
		{"attributeName", sourceFieldName},
		{"attributeCoreName", sourceFieldName},
		{"attributeDataType", valueType},
		{"valueType", valueType},
		{"value", value},
		{"isKnown", true},
		{"isCertain", true},
		{"isUncertain", false},
		{"knowledgeState", "Known"}
	};
}

json BuildFormCatalogTable(const FormCatalogSettings& settings,
                           const std::string& sourceId,
                           const std::string& sourceName,
                           const std::string& sourceObjectName,
                           const std::vector<std::string>& fieldNames,
                           json filters)
{
	json fields = json::array();

	for(const std::string& fieldName : fieldNames)
	{
		AddFormCatalogLoadField(fields, fieldName);
	}

	return {
		{"sourceId", sourceId},
		{"sourceName", sourceName},
		{"sourceType", settings.sourceType},
		{"databaseProvider", settings.databaseProvider},
		{"serverConnectionKey", settings.serverConnectionKey},
		{"sourceObjectName", sourceObjectName},
		{"fields", fields},
		{"filters", filters}
	};
}

void AddFormCatalogPrincipal(std::vector<std::pair<std::string, std::string>>& principals,
                             const std::string& principalType,
                             const std::string& principalId)
{
	const std::string trimmedType = TrimAscii(principalType);
	const std::string trimmedId = TrimAscii(principalId);

	if(trimmedType.empty() || trimmedId.empty())
	{
		return;
	}

	for(const auto& existing : principals)
	{
		if(EqualsIgnoreCase(existing.first, trimmedType) && existing.second == trimmedId)
		{
			return;
		}
	}

	principals.push_back({trimmedType, trimmedId});
}

std::vector<std::pair<std::string, std::string>> FormCatalogPrincipalsForSession(const Session& session)
{
	std::vector<std::pair<std::string, std::string>> principals;
	AddFormCatalogPrincipal(principals, "authenticated", "*");
	AddFormCatalogPrincipal(principals, "user", session.user_id);
	AddFormCatalogPrincipal(principals, "username", session.client_id);
	AddFormCatalogPrincipal(principals, "displayName", session.ruleset.sessionContext.userName);
	return principals;
}

json BuildFormCatalogLoadPlan(const FormCatalogSettings& settings, const Session& session)
{
	json tables = json::array();

	tables.push_back(BuildFormCatalogTable(
		settings,
		"form-catalog-forms",
		"Forms",
		settings.formsSourceObjectName,
		{"form_id", "form_key", "name", "description", "status"},
		json::array({
			BuildFormCatalogFilter("status", "active", "Text", "FormCatalogStatus")
		})));

	tables.push_back(BuildFormCatalogTable(
		settings,
		"form-catalog-form-versions",
		"Form versions",
		settings.formVersionsSourceObjectName,
		{"form_version_id", "form_id", "version_number", "version_label", "status", "session_model_hash", "session_model_schema_version", "source_name", "published_at"},
		json::array({
			BuildFormCatalogFilter("status", "published", "Text", "FormCatalogVersionStatus")
		})));

	const std::vector<std::pair<std::string, std::string>> principals = FormCatalogPrincipalsForSession(session);
	int principalIndex = 0;

	for(const auto& principal : principals)
	{
		tables.push_back(BuildFormCatalogTable(
			settings,
			"form-catalog-permissions-" + std::to_string(++principalIndex),
			"Form permissions",
			settings.formPermissionsSourceObjectName,
			{"form_id", "principal_type", "principal_id", "permission_level"},
			json::array({
				BuildFormCatalogFilter("principal_type", principal.first, "Text", "FormCatalogPrincipalType"),
				BuildFormCatalogFilter("principal_id", principal.second, "Text", "FormCatalogPrincipalId"),
				BuildFormCatalogFilter("permission_level", "start", "Text", "FormCatalogPermissionLevel")
			})));
	}

	return {
		{"tables", tables},
		{"diagnostics", json::array()}
	};
}

std::string FormCatalogRowString(const json& row, const std::string& sourceFieldName)
{
	return TrimAscii(LoadRowValueAsString(row, sourceFieldName));
}

std::optional<int> FormCatalogRowInt(const json& row, const std::string& sourceFieldName)
{
	const std::string value = FormCatalogRowString(row, sourceFieldName);

	if(value.empty())
	{
		return std::nullopt;
	}

	try
	{
		return std::stoi(value);
	}
	catch(const std::exception&)
	{
		return std::nullopt;
	}
}

struct FormCatalogVersionRow
{
	std::string formVersionId;
	std::string formId;
	int versionNumber{0};
	std::string versionLabel;
	std::string status;
	std::string sessionModelHash;
	std::string sessionModelSchemaVersion;
	std::string sourceName;
	std::string publishedAt;
};

std::optional<long long> ParseFormCatalogIntegerIdentifier(const std::string& value)
{
	const std::string trimmed = TrimAscii(value);

	if(trimmed.empty())
	{
		return std::nullopt;
	}

	try
	{
		size_t processed = 0;
		const long long parsed = std::stoll(trimmed, &processed);

		if(processed == trimmed.size())
		{
			return parsed;
		}
	}
	catch(const std::exception&)
	{
	}

	return std::nullopt;
}

bool IsLaterFormCatalogVersion(const std::string& candidateVersionId,
                               int candidateVersionNumber,
                               const std::string& currentVersionId,
                               int currentVersionNumber)
{
	if(candidateVersionNumber != currentVersionNumber)
	{
		return candidateVersionNumber > currentVersionNumber;
	}

	const std::optional<long long> candidateIntegerId = ParseFormCatalogIntegerIdentifier(candidateVersionId);
	const std::optional<long long> currentIntegerId = ParseFormCatalogIntegerIdentifier(currentVersionId);

	if(candidateIntegerId.has_value() && currentIntegerId.has_value())
	{
		return candidateIntegerId.value() > currentIntegerId.value();
	}

	return candidateVersionId > currentVersionId;
}

bool IsLaterFormCatalogVersion(const FormCatalogVersionRow& candidate, const FormCatalogVersionRow& current)
{
	return IsLaterFormCatalogVersion(
		candidate.formVersionId,
		candidate.versionNumber,
		current.formVersionId,
		current.versionNumber);
}

json BuildFormCatalogLoadSummary(const json& execution)
{
	return {
		{"configured", execution.value("configured", false)},
		{"attempted", execution.value("attempted", false)},
		{"httpStatus", execution.value("httpStatus", json(nullptr))},
		{"succeeded", execution.value("succeeded", false)},
		{"readExecuted", execution.value("readExecuted", false)},
		{"operationCount", execution.value("operationCount", 0)},
		{"rowCount", execution.value("rowCount", 0)},
		{"message", execution.value("message", std::string())}
	};
}

bool OperationUsesSourceObject(const json& operation, const std::string& sourceObjectName)
{
	return operation.is_object() &&
		EqualsIgnoreCase(operation.value("sourceObjectName", std::string()), sourceObjectName);
}

json BuildAvailableFormsFromCatalogExecution(const json& execution,
                                             const FormCatalogSettings& settings,
                                             const Session& session)
{
	json out = {
		{"ok", execution.value("succeeded", false)},
		{"message", execution.value("message", std::string())},
		{"userId", session.user_id},
		{"userName", session.ruleset.sessionContext.userName},
		{"principal", session.client_id},
		{"source", {
			{"serverConnectionKey", settings.serverConnectionKey},
			{"databaseProvider", settings.databaseProvider},
			{"formsSourceObjectName", settings.formsSourceObjectName},
			{"formVersionsSourceObjectName", settings.formVersionsSourceObjectName},
			{"formPermissionsSourceObjectName", settings.formPermissionsSourceObjectName}
		}},
		{"catalogLoad", BuildFormCatalogLoadSummary(execution)},
		{"forms", json::array()},
		{"formCount", 0},
		{"diagnostics", execution.value("diagnostics", json::array())}
	};

	if(!execution.value("succeeded", false))
	{
		return out;
	}

	std::unordered_map<std::string, json> formsById;
	std::vector<std::string> formOrder;
	std::unordered_map<std::string, FormCatalogVersionRow> latestVersionByFormId;
	std::unordered_map<std::string, json> permissionByFormId;
	const json operations = JsonArrayValue(execution, {"operations"});

	for(const json& operation : operations)
	{
		const json rows = JsonArrayValue(operation, {"rows"});

		if(OperationUsesSourceObject(operation, settings.formsSourceObjectName))
		{
			for(const json& row : rows)
			{
				const std::string formId = FormCatalogRowString(row, "form_id");
				const std::string formKey = FormCatalogRowString(row, "form_key");

				if(formId.empty() || formKey.empty())
				{
					continue;
				}

				if(formsById.find(formId) == formsById.end())
				{
					formOrder.push_back(formId);
				}

				formsById[formId] = {
					{"formId", formId},
					{"formKey", formKey},
					{"name", FormCatalogRowString(row, "name")},
					{"description", FormCatalogRowString(row, "description")},
					{"status", FormCatalogRowString(row, "status")}
				};
			}
		}
		else if(OperationUsesSourceObject(operation, settings.formVersionsSourceObjectName))
		{
			for(const json& row : rows)
			{
				FormCatalogVersionRow version;
				version.formVersionId = FormCatalogRowString(row, "form_version_id");
				version.formId = FormCatalogRowString(row, "form_id");
				version.versionNumber = FormCatalogRowInt(row, "version_number").value_or(0);
				version.versionLabel = FormCatalogRowString(row, "version_label");
				version.status = FormCatalogRowString(row, "status");
				version.sessionModelHash = FormCatalogRowString(row, "session_model_hash");
				version.sessionModelSchemaVersion = FormCatalogRowString(row, "session_model_schema_version");
				version.sourceName = FormCatalogRowString(row, "source_name");
				version.publishedAt = FormCatalogRowString(row, "published_at");

				if(version.formId.empty() || version.formVersionId.empty() || !EqualsIgnoreCase(version.status, "published"))
				{
					continue;
				}

				auto existing = latestVersionByFormId.find(version.formId);
				if(existing == latestVersionByFormId.end() || IsLaterFormCatalogVersion(version, existing->second))
				{
					latestVersionByFormId[version.formId] = version;
				}
			}
		}
		else if(OperationUsesSourceObject(operation, settings.formPermissionsSourceObjectName))
		{
			for(const json& row : rows)
			{
				const std::string formId = FormCatalogRowString(row, "form_id");
				const std::string permissionLevel = FormCatalogRowString(row, "permission_level");

				if(formId.empty() || !EqualsIgnoreCase(permissionLevel, "start"))
				{
					continue;
				}

				if(permissionByFormId.find(formId) == permissionByFormId.end())
				{
					permissionByFormId[formId] = {
						{"principalType", FormCatalogRowString(row, "principal_type")},
						{"principalId", FormCatalogRowString(row, "principal_id")},
						{"permissionLevel", permissionLevel}
					};
				}
			}
		}
	}

	for(const std::string& formId : formOrder)
	{
		auto form = formsById.find(formId);
		auto version = latestVersionByFormId.find(formId);
		auto permission = permissionByFormId.find(formId);

		if(form == formsById.end() || version == latestVersionByFormId.end() || permission == permissionByFormId.end())
		{
			continue;
		}

		json availableForm = form->second;
		availableForm["formVersionId"] = version->second.formVersionId;
		availableForm["versionNumber"] = version->second.versionNumber;
		availableForm["versionLabel"] = version->second.versionLabel;
		availableForm["versionStatus"] = version->second.status;
		availableForm["sessionModelHash"] = version->second.sessionModelHash;
		availableForm["sessionModelSchemaVersion"] = version->second.sessionModelSchemaVersion;
		availableForm["sourceName"] = version->second.sourceName;
		availableForm["publishedAt"] = version->second.publishedAt;
		availableForm["permission"] = permission->second;

		out["forms"].push_back(availableForm);
	}

	out["formCount"] = static_cast<int>(out["forms"].size());
	out["ok"] = true;
	return out;
}

struct FormLaunchSelection
{
	bool requested{false};
	std::string formKey;
	std::string formVersionId;
	std::optional<int> versionNumber;
};

FormLaunchSelection ParseFormLaunchSelection(const json& requestJson)
{
	FormLaunchSelection selection;
	selection.formKey = TrimAscii(first_json_scalar_string(requestJson, {"formKey", "form_key", "key"}));
	selection.formVersionId = TrimAscii(first_json_scalar_string(requestJson, {"formVersionId", "form_version_id", "versionId"}));
	selection.versionNumber = JsonIntValue(requestJson, {"versionNumber", "version_number"});
	selection.requested = !selection.formKey.empty() || !selection.formVersionId.empty() || selection.versionNumber.has_value();
	return selection;
}

json FormCatalogIdentifierFilterValue(const std::string& value)
{
	const std::string trimmed = TrimAscii(value);

	if(trimmed.empty())
	{
		return "";
	}

	try
	{
		size_t processed = 0;
		const long long parsed = std::stoll(trimmed, &processed);

		if(processed == trimmed.size())
		{
			return parsed;
		}
	}
	catch(const std::exception&)
	{
	}

	return trimmed;
}

json ExecuteFormCatalogTables(json tables, int maxRows)
{
	json loadPlan = {
		{"tables", std::move(tables)},
		{"diagnostics", json::array()}
	};

	return BuildDataConnectionLoadPlanExecutionResult(loadPlan, maxRows);
}

json ExecuteFormCatalogTable(const FormCatalogSettings& settings,
                             const std::string& sourceId,
                             const std::string& sourceName,
                             const std::string& sourceObjectName,
                             const std::vector<std::string>& fieldNames,
                             json filters,
                             int maxRows)
{
	json tables = json::array({
		BuildFormCatalogTable(settings, sourceId, sourceName, sourceObjectName, fieldNames, std::move(filters))
	});

	return ExecuteFormCatalogTables(std::move(tables), maxRows);
}

std::vector<json> FormCatalogRowsFromExecution(const json& execution, const std::string& sourceObjectName)
{
	std::vector<json> rows;
	const json operations = JsonArrayValue(execution, {"operations"});

	for(const json& operation : operations)
	{
		if(!OperationUsesSourceObject(operation, sourceObjectName))
		{
			continue;
		}

		for(const json& row : JsonArrayValue(operation, {"rows"}))
		{
			rows.push_back(row);
		}
	}

	return rows;
}

struct SelectedFormSessionModelLoadResult
{
	bool succeeded{false};
	int httpStatus{500};
	std::string code{"selected_form_load_failed"};
	std::string message;
	SelectedFormRuntime selectedForm;
	ScreenFile::ScreenData screenData;
	json diagnostics = json::array();
};

SelectedFormSessionModelLoadResult SelectedFormLoadFailure(int httpStatus,
                                                           const std::string& code,
                                                           const std::string& message,
                                                           const json& diagnostics = json::array())
{
	SelectedFormSessionModelLoadResult result;
	result.succeeded = false;
	result.httpStatus = httpStatus;
	result.code = code;
	result.message = message;
	result.diagnostics = diagnostics;
	return result;
}

json FormCatalogExecutionDiagnostics(const json& execution)
{
	return execution.value("diagnostics", json::array());
}

std::optional<json> FindLatestPublishedVersionRow(const std::vector<json>& rows)
{
	std::optional<json> latest;
	int latestVersionNumber = std::numeric_limits<int>::min();
	std::string latestVersionId;

	for(const json& row : rows)
	{
		const std::string versionId = FormCatalogRowString(row, "form_version_id");
		const std::string status = FormCatalogRowString(row, "status");
		const int versionNumber = FormCatalogRowInt(row, "version_number").value_or(0);

		if(versionId.empty() || !EqualsIgnoreCase(status, "published"))
		{
			continue;
		}

		if(!latest.has_value() || IsLaterFormCatalogVersion(versionId, versionNumber, latestVersionId, latestVersionNumber))
		{
			latest = row;
			latestVersionNumber = versionNumber;
			latestVersionId = versionId;
		}
	}

	return latest;
}

bool FormCatalogPermissionExecutionAllowsStart(const json& execution,
                                               const FormCatalogSettings& settings,
                                               const std::string& formId)
{
	const std::vector<json> permissionRows = FormCatalogRowsFromExecution(execution, settings.formPermissionsSourceObjectName);

	for(const json& row : permissionRows)
	{
		if(FormCatalogRowString(row, "form_id") == formId
			&& EqualsIgnoreCase(FormCatalogRowString(row, "permission_level"), "start"))
		{
			return true;
		}
	}

	return false;
}

json BuildSelectedFormPermissionCatalogTables(const FormCatalogSettings& settings,
                                              const Session& session,
                                              const std::string& formId)
{
	json permissionTables = json::array();
	const std::vector<std::pair<std::string, std::string>> principals = FormCatalogPrincipalsForSession(session);
	int principalIndex = 0;

	for(const auto& principal : principals)
	{
		permissionTables.push_back(BuildFormCatalogTable(
			settings,
			"selected-form-permission-" + std::to_string(++principalIndex),
			"Selected form permission",
			settings.formPermissionsSourceObjectName,
			{"form_id", "principal_type", "principal_id", "permission_level"},
			json::array({
				BuildFormCatalogFilter("form_id", FormCatalogIdentifierFilterValue(formId), "Integer", "SelectedFormPermissionFormId"),
				BuildFormCatalogFilter("principal_type", principal.first, "Text", "SelectedFormPermissionPrincipalType"),
				BuildFormCatalogFilter("principal_id", principal.second, "Text", "SelectedFormPermissionPrincipalId"),
				BuildFormCatalogFilter("permission_level", "start", "Text", "SelectedFormPermissionLevel")
			})));
	}

	return permissionTables;
}

SelectedFormSessionModelLoadResult LoadSelectedFormSessionModelFromCatalog(const FormCatalogSettings& settings,
                                                                           const Session& session,
                                                                           const FormLaunchSelection& selection)
{
	SelectedFormLoadMetricScope totalMetric("selected_form_load_total");

	if(!settings.enabled)
	{
		return SelectedFormLoadFailure(
			409,
			"form_catalog_disabled",
			"The form catalogue is disabled in Session Manager configuration.");
	}

	if(selection.formVersionId.empty() && selection.formKey.empty())
	{
		return SelectedFormLoadFailure(
			400,
			"form_selection_missing_key",
			"Selected form requests require formKey or formVersionId.");
	}

	const std::vector<std::string> formFields = {
		"form_id", "form_key", "name", "description", "status"
	};
	const std::vector<std::string> versionFields = {
		"form_version_id",
		"form_id",
		"version_number",
		"version_label",
		"status",
		"session_model_json",
		"session_model_hash",
		"session_model_schema_version",
		"source_name",
		"published_at"
	};

	json formRow;
	json versionRow;
	json permissionExecution;
	std::string formId;

	if(!selection.formVersionId.empty())
	{
		json versionFilters = json::array({
			BuildFormCatalogFilter("form_version_id", FormCatalogIdentifierFilterValue(selection.formVersionId), "Integer", "SelectedFormVersionId"),
			BuildFormCatalogFilter("status", "published", "Text", "SelectedFormVersionStatus")
		});
		json versionExecution;

		{
			SelectedFormLoadMetricScope metric("catalog_version_lookup");
			versionExecution = ExecuteFormCatalogTable(
				settings,
				"selected-form-version",
				"Selected form version",
				settings.formVersionsSourceObjectName,
				versionFields,
				std::move(versionFilters),
				2);
		}

		if(!versionExecution.value("succeeded", false))
		{
			return SelectedFormLoadFailure(
				versionExecution.value("attempted", false) ? 502 : 409,
				"selected_form_version_load_failed",
				"Could not load the selected form version from the form catalogue.",
				FormCatalogExecutionDiagnostics(versionExecution));
		}

		const std::vector<json> versionRows = FormCatalogRowsFromExecution(versionExecution, settings.formVersionsSourceObjectName);

		if(versionRows.empty())
		{
			return SelectedFormLoadFailure(
				404,
				"selected_form_version_not_found",
				"The selected form version was not found or is not published.");
		}

		versionRow = versionRows.front();
		formId = FormCatalogRowString(versionRow, "form_id");

		if(formId.empty())
		{
			return SelectedFormLoadFailure(
				409,
				"selected_form_version_missing_form_id",
				"The selected form version row does not include a form_id.");
		}

		json formFilters = json::array({
			BuildFormCatalogFilter("form_id", FormCatalogIdentifierFilterValue(formId), "Integer", "SelectedFormId"),
			BuildFormCatalogFilter("status", "active", "Text", "SelectedFormStatus")
		});
		json formPermissionTables = json::array();
		formPermissionTables.push_back(BuildFormCatalogTable(
			settings,
			"selected-form",
			"Selected form",
			settings.formsSourceObjectName,
			formFields,
			std::move(formFilters)));

		const json formPermissionChecks = BuildSelectedFormPermissionCatalogTables(settings, session, formId);

		for(const json& permissionTable : formPermissionChecks)
		{
			formPermissionTables.push_back(permissionTable);
		}

		json formPermissionExecution;

		{
			SelectedFormLoadMetricScope metric("catalog_form_permission_lookup");
			formPermissionExecution = ExecuteFormCatalogTables(std::move(formPermissionTables), 2);
		}

		if(!formPermissionExecution.value("succeeded", false))
		{
			return SelectedFormLoadFailure(
				formPermissionExecution.value("attempted", false) ? 502 : 409,
				"selected_form_load_failed",
				"Could not load the active form row or verify start permission for the selected form version.",
				FormCatalogExecutionDiagnostics(formPermissionExecution));
		}

		const std::vector<json> formRows = FormCatalogRowsFromExecution(formPermissionExecution, settings.formsSourceObjectName);

		if(formRows.empty())
		{
			return SelectedFormLoadFailure(
				404,
				"selected_form_not_found",
				"The selected form was not found or is not active.");
		}

		formRow = formRows.front();
		permissionExecution = std::move(formPermissionExecution);
	}
	else
	{
		json formFilters = json::array({
			BuildFormCatalogFilter("form_key", selection.formKey, "Text", "SelectedFormKey"),
			BuildFormCatalogFilter("status", "active", "Text", "SelectedFormStatus")
		});
		json formExecution;

		{
			SelectedFormLoadMetricScope metric("catalog_form_lookup");
			formExecution = ExecuteFormCatalogTable(
				settings,
				"selected-form",
				"Selected form",
				settings.formsSourceObjectName,
				formFields,
				std::move(formFilters),
				2);
		}

		if(!formExecution.value("succeeded", false))
		{
			return SelectedFormLoadFailure(
				formExecution.value("attempted", false) ? 502 : 409,
				"selected_form_load_failed",
				"Could not load the active form row from the form catalogue.",
				FormCatalogExecutionDiagnostics(formExecution));
		}

		const std::vector<json> formRows = FormCatalogRowsFromExecution(formExecution, settings.formsSourceObjectName);

		if(formRows.empty())
		{
			return SelectedFormLoadFailure(
				404,
				"selected_form_not_found",
				"The selected form was not found or is not active.");
		}

		formRow = formRows.front();
		formId = FormCatalogRowString(formRow, "form_id");

		if(formId.empty())
		{
			return SelectedFormLoadFailure(
				409,
				"selected_form_missing_form_id",
				"The selected form row does not include a form_id.");
		}

		json versionFilters = json::array({
			BuildFormCatalogFilter("form_id", FormCatalogIdentifierFilterValue(formId), "Integer", "SelectedFormId"),
			BuildFormCatalogFilter("status", "published", "Text", "SelectedFormVersionStatus")
		});

		if(selection.versionNumber.has_value())
		{
			versionFilters.push_back(BuildFormCatalogFilter("version_number", selection.versionNumber.value(), "Integer", "SelectedFormVersionNumber"));
		}

		json versionPermissionTables = json::array();
		versionPermissionTables.push_back(BuildFormCatalogTable(
			settings,
			"selected-form-version",
			"Selected form version",
			settings.formVersionsSourceObjectName,
			versionFields,
			std::move(versionFilters)));

		const json versionPermissionChecks = BuildSelectedFormPermissionCatalogTables(settings, session, formId);

		for(const json& permissionTable : versionPermissionChecks)
		{
			versionPermissionTables.push_back(permissionTable);
		}

		json versionPermissionExecution;

		{
			SelectedFormLoadMetricScope metric("catalog_version_permission_lookup");
			versionPermissionExecution = ExecuteFormCatalogTables(std::move(versionPermissionTables), settings.maxRows);
		}

		if(!versionPermissionExecution.value("succeeded", false))
		{
			return SelectedFormLoadFailure(
				versionPermissionExecution.value("attempted", false) ? 502 : 409,
				"selected_form_version_load_failed",
				"Could not load the published version row or verify start permission for the selected form.",
				FormCatalogExecutionDiagnostics(versionPermissionExecution));
		}

		const std::vector<json> versionRows = FormCatalogRowsFromExecution(versionPermissionExecution, settings.formVersionsSourceObjectName);
		const std::optional<json> latestVersion = FindLatestPublishedVersionRow(versionRows);

		if(!latestVersion.has_value())
		{
			return SelectedFormLoadFailure(
				404,
				"selected_form_version_not_found",
				"The selected form does not have a published version matching the request.");
		}

		versionRow = latestVersion.value();
		permissionExecution = std::move(versionPermissionExecution);
	}

	formId = FormCatalogRowString(formRow, "form_id");

	if(formId.empty())
	{
		return SelectedFormLoadFailure(
			409,
			"selected_form_missing_form_id",
			"The selected form row does not include a form_id.");
	}

	if(!FormCatalogPermissionExecutionAllowsStart(permissionExecution, settings, formId))
	{
		return SelectedFormLoadFailure(
			403,
			"selected_form_permission_denied",
			"The current user does not have permission to start the selected form.");
	}

	std::string sessionModelJson;

	{
		SelectedFormLoadMetricScope metric("session_model_json_extract");
		sessionModelJson = TrimAscii(LoadRowValueAsString(versionRow, "session_model_json"));
	}

	if(sessionModelJson.empty())
	{
		return SelectedFormLoadFailure(
			409,
			"selected_form_session_model_missing",
			"The selected form version does not include a Session Model JSON payload.");
	}

	RuleServerModelLoadResult modelLoad;

	try
	{
		SelectedFormLoadMetricScope metric("session_model_import_convert");
		modelLoad = LoadRuleServerModelFromSessionModelJsonText(sessionModelJson);
	}
	catch(const std::exception& e)
	{
		return SelectedFormLoadFailure(
			409,
			"selected_form_session_model_import_failed",
			std::string("The selected form version Session Model JSON could not be imported. ") + e.what());
	}

	SelectedFormSessionModelLoadResult result;
	result.succeeded = true;
	result.httpStatus = 200;
	result.code.clear();
	result.message = "Selected form Session Model JSON loaded.";
	result.screenData = modelLoad.screenFile.screenData;
	result.selectedForm.selected = true;
	result.selectedForm.formId = formId;
	result.selectedForm.formKey = FormCatalogRowString(formRow, "form_key");
	result.selectedForm.formName = FormCatalogRowString(formRow, "name");
	result.selectedForm.formDescription = FormCatalogRowString(formRow, "description");
	result.selectedForm.formVersionId = FormCatalogRowString(versionRow, "form_version_id");
	result.selectedForm.versionNumber = FormCatalogRowInt(versionRow, "version_number").value_or(0);
	result.selectedForm.versionLabel = FormCatalogRowString(versionRow, "version_label");
	result.selectedForm.sessionModelHash = FormCatalogRowString(versionRow, "session_model_hash");
	result.selectedForm.sessionModelSchemaVersion = FormCatalogRowString(versionRow, "session_model_schema_version");
	result.selectedForm.sourceName = FormCatalogRowString(versionRow, "source_name");
	result.selectedForm.publishedAt = FormCatalogRowString(versionRow, "published_at");
	result.selectedForm.runtimeTemplate = modelLoad.sessionModelRuntimeTemplate;
	result.selectedForm.externalSecurity = modelLoad.sessionModelRuntimeTemplate.externalSecurity;

	return result;
}

std::string now_iso8601_utc()
{
	const auto now = Clock::now();

	const std::time_t t = Clock::to_time_t(now);

	std::tm tm{};

#if defined(_WIN32)
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif

	std::ostringstream oss;

	oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");

	return oss.str();
}

std::string generate_token(std::size_t bytes = 32)
{
	std::random_device rd;

	std::mt19937_64 gen(rd());

	std::uniform_int_distribution<int> dist(0, 255);

	static const char* kHex = "0123456789abcdef";

	std::string out;

	out.reserve(bytes * 2);

	for(std::size_t i = 0; i < bytes; ++i)
	{
		const int v = dist(gen);

		out.push_back(kHex[(v >> 4) & 0xF]);

		out.push_back(kHex[v & 0xF]);
	}

	return out;
}

std::string build_user_id_from_client_id(const std::string& client_id)
{
	const std::string trimmed = TrimAscii(client_id);
	std::uint64_t hash = 1469598103934665603ULL;

	for(unsigned char ch : trimmed)
	{
		hash ^= static_cast<std::uint64_t>(ch);
		hash *= 1099511628211ULL;
	}

	std::ostringstream oss;
	oss << "user-" << std::hex << std::setw(16) << std::setfill('0') << hash;
	return oss.str();
}

std::string extract_token(const httplib::Request& req)
{
	auto auth = req.get_header_value("Authorization");

	if(!auth.empty())
	{
		const std::string prefix = "Bearer ";

		if(auth.rfind(prefix, 0) == 0 && auth.size() > prefix.size())
		{
			return auth.substr(prefix.size());
		}

		return auth;
	}

	auto x = req.get_header_value("X-Auth-Token");

	return x;
}

bool try_get_session(const std::string& token, Session& out)
{
	std::shared_ptr<std::recursive_mutex> stateMutex;

	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);

		auto it = g_sessions.find(token);

		if(it == g_sessions.end())
		{
			return false;
		}

		stateMutex = it->second.state_mu;
	}

	if(stateMutex == nullptr)
	{
		return false;
	}

	auto stateLock = std::make_shared<std::unique_lock<std::recursive_mutex>>(*stateMutex);

	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);

		auto it = g_sessions.find(token);

		if(it == g_sessions.end())
		{
			return false;
		}

		out = it->second;
	}

	out.state_lock = stateLock;

	return true;
}

bool touch_session(const std::string& token, Session* updated = nullptr)
{
	std::lock_guard<std::mutex> lock(g_sessions_mu);

	auto it = g_sessions.find(token);

	if(it == g_sessions.end())
	{
		return false;
	}

	it->second.last_seen = Clock::now();

	it->second.message_count++;

	if(updated)
	{
		*updated = it->second;
	}

	return true;
}

bool store_session_ruleset(const std::string& token,
                           const RuleSet& ruleset,
                           std::uint64_t messageCountBeforeIncrement,
                           Session* updated = nullptr)
{
	std::lock_guard<std::mutex> lock(g_sessions_mu);

	auto it = g_sessions.find(token);

	if(it == g_sessions.end())
	{
		return false;
	}

	it->second.ruleset = ruleset;
	it->second.last_seen = Clock::now();
	it->second.message_count = messageCountBeforeIncrement + 1;

	if(updated)
	{
		*updated = it->second;
	}

	return true;
}

bool store_session_context(const std::string& token, const RuleSet::SessionContext& context, Session* updated = nullptr)
{
	std::lock_guard<std::mutex> lock(g_sessions_mu);

	auto it = g_sessions.find(token);

	if(it == g_sessions.end())
	{
		return false;
	}

	it->second.ruleset.sessionContext = context;
	it->second.last_seen = Clock::now();
	it->second.message_count++;

	if(updated)
	{
		*updated = it->second;
	}

	return true;
}

void remove_expired_sessions(std::chrono::minutes ttl = std::chrono::minutes(30))
{
	const auto cutoff = Clock::now() - ttl;

	std::lock_guard<std::mutex> lock(g_sessions_mu);

	for(auto it = g_sessions.begin(); it != g_sessions.end();)
	{
		if(it->second.last_seen < cutoff)
		{
			it = g_sessions.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void remove_expired_debug_sessions(std::chrono::minutes ttl = std::chrono::minutes(30))
{
	const auto cutoff = Clock::now() - ttl;

	std::lock_guard<std::mutex> lock(g_debug_sessions_mu);

	for(auto it = g_debug_sessions.begin(); it != g_debug_sessions.end();)
	{
		if(it->second.last_seen < cutoff)
		{
			it = g_debug_sessions.erase(it);
		}
		else
		{
			++it;
		}
	}
}

json make_error(std::string code, std::string message)
{
	return json{
		{"ok", false},
		{"error",
		{
			{"code", std::move(code)},
		{"message", std::move(message)},
	}},
	};
}

std::optional<std::uint64_t> json_sequence_value(const json& value)
{
	if(value.is_number_unsigned())
	{
		return value.get<std::uint64_t>();
	}

	if(value.is_number_integer())
	{
		const long long parsed = value.get<long long>();

		if(parsed >= 0)
		{
			return static_cast<std::uint64_t>(parsed);
		}
	}

	if(value.is_string())
	{
		const std::string text = value.get<std::string>();

		if(!text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }))
		{
			try
			{
				return static_cast<std::uint64_t>(std::stoull(text));
			}
			catch(const std::exception&)
			{
				return std::nullopt;
			}
		}
	}

	return std::nullopt;
}

std::optional<std::uint64_t> first_json_sequence(const json& value, std::initializer_list<const char*> keys)
{
	if(!value.is_object())
	{
		return std::nullopt;
	}

	for(const char* key : keys)
	{
		if(value.contains(key) && !value.at(key).is_null())
		{
			std::optional<std::uint64_t> parsed = json_sequence_value(value.at(key));

			if(parsed.has_value())
			{
				return parsed;
			}
		}
	}

	return std::nullopt;
}

bool ScreenRequestIsStale(const Session& session,
                          const std::optional<std::uint64_t>& clientSequence,
                          const std::string& screenId,
                          const std::string& entityId,
                          const std::string& instanceId)
{
	if(!clientSequence.has_value())
	{
		return false;
	}

	if(clientSequence.value() < session.message_count)
	{
		return true;
	}

	const ScreenLocation latest = LatestScreenLocation(session.ruleset);

	if(latest.screenId.empty())
	{
		return false;
	}

	if(!screenId.empty() && screenId != latest.screenId)
	{
		return true;
	}

	if(!entityId.empty() && entityId != latest.entityId)
	{
		return true;
	}

	if(!instanceId.empty() && instanceId != latest.instanceId)
	{
		return true;
	}

	return false;
}

ScreenFile::ScreenData& EnsureSessionScreenDataLoaded(Session& session);

void WriteStaleScreenResponse(const std::string& token,
                              Session& session,
                              const std::optional<std::uint64_t>& clientSequence,
                              httplib::Response& res,
                              const EndpointResponseOptions& responseOptions)
{
	const std::uint64_t responseSequence = session.message_count + 1;
	const ScreenLocation latest = LatestScreenLocation(session.ruleset);

	bool hasBlockingValidationErrors = false;
	json out = make_error("stale_screen", "The request was for an older screen state. The current screen is returned.");
	out["conflict"] = {
		{"type", "stale_screen"},
		{"currentSequence", responseSequence}
	};

	if(clientSequence.has_value())
	{
		out["conflict"]["clientSequence"] = clientSequence.value();
		const auto command = session.emailCommandsBySequence.find(clientSequence.value() + 1);
		if(command != session.emailCommandsBySequence.end())
		{
			out["emailCommands"] = json::array({command->second});
		}
	}

	out["screenLocation"] = BuildScreenLocationJson(latest, responseSequence);
	out["actions"] = latest.screenId.empty() ? BuildCompleteActionsJson() : BuildDetachedScreenActionsJson();
	out["validationEvents"] = BuildActiveValidationEventsJson(session.ruleset, hasBlockingValidationErrors);

	if(!latest.screenId.empty())
	{
		DataManager dataManager;
		ScreenFile::ScreenData& screenData = EnsureSessionScreenDataLoaded(session);
		std::shared_ptr<ScreenFile::Screen> screen = screenData.GetScreenByID(latest.screenId);

		if(screen != nullptr)
		{
			screen = RefreshScreenForInstance(dataManager, session.ruleset, screen, latest.entityId, latest.instanceId, &screenData);

			if(screen != nullptr)
			{
				out["screen"] = BuildFrontendScreen(*screen);
				if(responseOptions.includeUiState)
				{
					out["uiState"] = BuildScreenUiState(*screen);
				}
				out["screenLocation"] = BuildScreenLocationJson(*screen, latest.entityId, latest.instanceId, responseSequence);
				out["actions"] = BuildScreenActionsJson(session.ruleset, *screen, screenData);
			}
		}
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	res.status = 409;
	WriteEndpointJsonResponse(res, out, responseOptions);
}

void MergeEvaluationStats(SessionModelRuleEvaluator::EvaluationStats& target, const SessionModelRuleEvaluator::EvaluationStats& source)
{
	target.queuedAttributes += source.queuedAttributes;
	target.queuedRelationships += source.queuedRelationships;
	target.candidateRules += source.candidateRules;
	target.evaluatedRules += source.evaluatedRules;
	target.appliedConclusions += source.appliedConclusions;
	target.skippedRules += source.skippedRules;
	target.unsupportedRules += source.unsupportedRules;
	target.maxDepthStops += source.maxDepthStops;
	target.inferredInstancesCreated += source.inferredInstancesCreated;
	target.inferredInstancesReused += source.inferredInstancesReused;
	target.inferredInstancesRemoved += source.inferredInstancesRemoved;
	target.inferredLinksCreated += source.inferredLinksCreated;
	target.inferredLinksReused += source.inferredLinksReused;
	target.inferredLinksRemoved += source.inferredLinksRemoved;
	target.validationEventsCreated += source.validationEventsCreated;
	target.validationEventsReused += source.validationEventsReused;
	target.validationEventsUpdated += source.validationEventsUpdated;
	target.validationEventsCleared += source.validationEventsCleared;
	target.validationErrors += source.validationErrors;
	target.validationWarnings += source.validationWarnings;
	target.createdInferredInstances.insert(target.createdInferredInstances.end(), source.createdInferredInstances.begin(), source.createdInferredInstances.end());
	target.reusedInferredInstances.insert(target.reusedInferredInstances.end(), source.reusedInferredInstances.begin(), source.reusedInferredInstances.end());
	target.removedInferredInstances.insert(target.removedInferredInstances.end(), source.removedInferredInstances.begin(), source.removedInferredInstances.end());
	target.createdInferredLinks.insert(target.createdInferredLinks.end(), source.createdInferredLinks.begin(), source.createdInferredLinks.end());
	target.reusedInferredLinks.insert(target.reusedInferredLinks.end(), source.reusedInferredLinks.begin(), source.reusedInferredLinks.end());
	target.removedInferredLinks.insert(target.removedInferredLinks.end(), source.removedInferredLinks.begin(), source.removedInferredLinks.end());
	target.createdValidationEvents.insert(target.createdValidationEvents.end(), source.createdValidationEvents.begin(), source.createdValidationEvents.end());
	target.reusedValidationEvents.insert(target.reusedValidationEvents.end(), source.reusedValidationEvents.begin(), source.reusedValidationEvents.end());
	target.updatedValidationEvents.insert(target.updatedValidationEvents.end(), source.updatedValidationEvents.begin(), source.updatedValidationEvents.end());
	target.clearedValidationEvents.insert(target.clearedValidationEvents.end(), source.clearedValidationEvents.begin(), source.clearedValidationEvents.end());
	target.diagnostics.insert(target.diagnostics.end(), source.diagnostics.begin(), source.diagnostics.end());
	target.trace.insert(target.trace.end(), source.trace.begin(), source.trace.end());
}

void AppendDataConnectionLoadHydrationDiagnostic(json& result,
                                                 const std::string& severity,
                                                 const std::string& code,
                                                 const std::string& message,
                                                 const std::string& context)
{
	const json diagnostic = MakeDataConnectionDryRunDiagnostic(severity, code, message, context);

	if(!result.contains("diagnostics") || !result.at("diagnostics").is_array())
	{
		result["diagnostics"] = json::array();
	}

	result["diagnostics"].push_back(diagnostic);

	if(!result.contains("hydrationDiagnostics") || !result.at("hydrationDiagnostics").is_array())
	{
		result["hydrationDiagnostics"] = json::array();
	}

	result["hydrationDiagnostics"].push_back(diagnostic);
}

std::shared_ptr<Entity> FindDataConnectionHydrationEntity(const RuleSet& ruleSet, const std::string& entityId)
{
	if(ruleSet.entities == nullptr)
	{
		return nullptr;
	}

	for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
	{
		if(entity != nullptr && entity->entityid == entityId)
		{
			return entity;
		}
	}

	return nullptr;
}

std::string DataConnectionHydrationValueToString(const json* value, bool isNull)
{
	if(isNull || value == nullptr || value->is_null())
	{
		return "";
	}

	if(value->is_string())
	{
		return value->get<std::string>();
	}

	if(value->is_boolean())
	{
		return value->get<bool>() ? "true" : "false";
	}

	if(value->is_number())
	{
		return value->dump();
	}

	return value->dump();
}

struct DataConnectionLoadHydrationCandidate
{
	std::shared_ptr<Attribute> attribute;
	std::string entityId;
	std::string instanceId;
	std::string attributeId;
	std::string sourceFieldName;
	std::string valueText;
};

struct DataConnectionPendingCollectionHydrationValue
{
	std::string entityId;
	std::string entityName;
	std::string attributeId;
	std::string attributeName;
	std::string attributeCoreName;
	std::string attributeDataType;
	std::string sourceFieldName;
	std::string valueType;
	json value;
	bool isNull = false;
	bool isRecordIdentity = false;
	std::string valueText;
};

struct DataConnectionPendingCollectionHydrationRow
{
	std::shared_ptr<Entity> entity;
	std::shared_ptr<Instance> parentInstance;
	std::string relationshipId;
	std::string sourceId;
	std::string sourceName;
	std::string serverConnectionKey;
	std::string sourceObjectName;
	std::vector<DataConnectionPendingCollectionHydrationValue> values;
	std::string operationContext;
};

struct DataConnectionLoadHydrationRelationshipCandidate
{
	std::string entityId;
	std::string instanceId;
	std::string relationshipId;
};

bool DataConnectionLoadOperationFieldIsPrimaryKey(const json& operation, const std::string& sourceFieldName)
{
	if(sourceFieldName.empty()
		|| !operation.is_object()
		|| !operation.contains("fields")
		|| !operation.at("fields").is_array())
	{
		return false;
	}

	for(const json& field : operation.at("fields"))
	{
		if(field.is_object()
			&& field.value("sourceFieldName", std::string()) == sourceFieldName
			&& field.value("isPrimaryKey", false))
		{
			return true;
		}
	}

	return false;
}

bool DataConnectionLoadValueIsRecordIdentity(const json& operation, const json& value)
{
	if(!value.is_object())
	{
		return false;
	}

	if(value.value("isPrimaryKey", false))
	{
		return true;
	}

	return DataConnectionLoadOperationFieldIsPrimaryKey(
		operation,
		value.value("sourceFieldName", std::string()));
}

bool IsSelectedRecordRelationshipFilter(const json& filter)
{
	if(!filter.is_object())
	{
		return false;
	}

	return filter.value("valueSource", std::string()) == "SelectedRecordRelationshipKey"
		|| filter.value("filterRole", std::string()) == "SelectedRecordRelationshipKey";
}

bool IsSelectedRecordRelationshipOperation(const json& operation)
{
	if(!operation.is_object()
		|| !operation.contains("filters")
		|| !operation.at("filters").is_array())
	{
		return false;
	}

	for(const json& filter : operation.at("filters"))
	{
		if(IsSelectedRecordRelationshipFilter(filter))
		{
			return true;
		}
	}

	return false;
}

const RuleSet::DataConnectionRelationshipFieldMapping* FindSelectedRecordRelationshipMapping(
	const RuleSet::DataConnectionEntityMapping& mapping,
	const json& operation)
{
	if(!operation.is_object()
		|| !operation.contains("filters")
		|| !operation.at("filters").is_array())
	{
		return nullptr;
	}

	for(const json& filter : operation.at("filters"))
	{
		if(!IsSelectedRecordRelationshipFilter(filter))
		{
			continue;
		}

		const std::string sourceFieldName = filter.value("sourceFieldName", std::string());

		for(const RuleSet::DataConnectionRelationshipFieldMapping& relationshipMapping : mapping.relationshipFieldMappings)
		{
			if(relationshipMapping.output
				&& !relationshipMapping.sourceFieldName.empty()
				&& relationshipMapping.sourceFieldName == sourceFieldName)
			{
				return &relationshipMapping;
			}
		}
	}

	return nullptr;
}

std::shared_ptr<Relationship> FindDataConnectionHydrationRelationship(const RuleSet& ruleSet, const std::string& relationshipId)
{
	if(relationshipId.empty() || ruleSet.entities == nullptr)
	{
		return nullptr;
	}

	for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
	{
		if(entity == nullptr)
		{
			continue;
		}

		for(const std::shared_ptr<Relationship>& relationship : entity->relationships)
		{
			if(relationship != nullptr && relationship->id == relationshipId)
			{
				return relationship;
			}
		}
	}

	return nullptr;
}

void AddDataConnectionHydrationRelationshipCandidate(
	RuleSet& ruleSet,
	std::vector<DataConnectionLoadHydrationRelationshipCandidate>& relationshipCandidates,
	std::unordered_set<std::string>& relationshipCandidateKeys,
	const std::string& relationshipId,
	const std::shared_ptr<Instance>& parentInstance,
	const std::shared_ptr<Instance>& childInstance)
{
	if(relationshipId.empty() || parentInstance == nullptr)
	{
		return;
	}

	std::string entityId = parentInstance->entityId;
	std::string instanceId = parentInstance->instanceId;
	const std::shared_ptr<Relationship> relationship = FindDataConnectionHydrationRelationship(ruleSet, relationshipId);

	if(relationship != nullptr && childInstance != nullptr && relationship->sourceEntity == childInstance->entityId)
	{
		entityId = childInstance->entityId;
		instanceId = childInstance->instanceId;
	}

	const std::string key = entityId + "\x1f" + instanceId + "\x1f" + relationshipId;

	if(relationshipCandidateKeys.insert(key).second)
	{
		relationshipCandidates.push_back(DataConnectionLoadHydrationRelationshipCandidate{
			entityId,
			instanceId,
			relationshipId
		});
	}
}

std::shared_ptr<Instance> FindSelectedRecordRelationshipParentInstance(
	RuleSet& ruleSet,
	const RuleSet::DataConnectionRelationshipFieldMapping& relationshipMapping,
	const json& operation)
{
	std::shared_ptr<Entity> parentEntity = FindDataConnectionHydrationEntity(ruleSet, relationshipMapping.relatedEntityId);

	if(parentEntity == nullptr)
	{
		return nullptr;
	}

	if(operation.is_object()
		&& operation.contains("filters")
		&& operation.at("filters").is_array())
	{
		for(const json& filter : operation.at("filters"))
		{
			if(!IsSelectedRecordRelationshipFilter(filter))
			{
				continue;
			}

			if(filter.value("entityId", std::string()) != relationshipMapping.relatedEntityId
				|| filter.value("attributeId", std::string()) != relationshipMapping.relatedAttributeId)
			{
				continue;
			}

			const std::string instanceId = filter.value("instanceId", std::string());

			if(!instanceId.empty())
			{
				return parentEntity->GetInstanceById(instanceId);
			}
		}
	}

	return parentEntity->instances.size() == 1 ? parentEntity->instances.front() : nullptr;
}

void ClearDataConnectionCollectionHydrationEntity(
	RuleSet& ruleSet,
	const std::shared_ptr<Entity>& entity,
	const std::shared_ptr<Instance>& parentInstance,
	const std::string& relationshipId)
{
	if(entity == nullptr || parentInstance == nullptr || relationshipId.empty())
	{
		return;
	}

	std::vector<std::shared_ptr<Instance>> ownedInstances;

	for(const std::shared_ptr<Instance>& instance : entity->instances)
	{
		if(instance == nullptr)
		{
			continue;
		}

		const bool ownedByParent = std::any_of(
			instance->relationshipLinks.begin(),
			instance->relationshipLinks.end(),
			[&](const InstanceRelationshipLink& link) {
				return link.relationshipId == relationshipId
					&& link.sourceInstanceId == parentInstance->instanceId
					&& link.targetInstanceId == instance->instanceId;
			});

		if(ownedByParent)
		{
			ownedInstances.push_back(instance);
		}
	}

	for(const std::shared_ptr<Instance>& instance : ownedInstances)
	{
		entity->CascadeDelete(instance, instance, ruleSet.entities, true);
	}
}

bool ValidateDataConnectionRelationshipCollectionOperation(
	RuleSet& ruleSet,
	json& executionResult,
	json& operation,
	const std::string& operationContext,
	std::vector<DataConnectionPendingCollectionHydrationRow>& pendingRows)
{
	const RuleSet::DataConnectionEntityMapping* mapping = FindDataConnectionEntityMappingForLoadPlanTable(ruleSet, operation);

	if(mapping == nullptr)
	{
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_collection_mapping_missing",
			"Data-connection load hydration could not resolve the relationship-scoped collection entity mapping.",
			operationContext);
		return false;
	}

	const RuleSet::DataConnectionRelationshipFieldMapping* relationshipMapping =
		FindSelectedRecordRelationshipMapping(*mapping, operation);

	if(relationshipMapping == nullptr)
	{
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_collection_relationship_missing",
			"Data-connection load hydration could not resolve the relationship mapping for a selected-record collection operation.",
			operationContext + ".filters");
		return false;
	}

	std::shared_ptr<Entity> entity = FindDataConnectionHydrationEntity(ruleSet, mapping->entityId);

	if(entity == nullptr)
	{
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_collection_entity_missing",
			"Data-connection load hydration could not resolve the collection target runtime entity.",
			operationContext);
		return false;
	}

	std::shared_ptr<Instance> parentInstance =
		FindSelectedRecordRelationshipParentInstance(ruleSet, *relationshipMapping, operation);

	if(parentInstance == nullptr)
	{
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_collection_parent_missing",
			"Data-connection load hydration could not resolve the selected parent instance for a relationship-scoped collection operation.",
			operationContext + ".filters");
		return false;
	}

	const std::shared_ptr<Relationship> relationship =
		FindDataConnectionHydrationRelationship(ruleSet, relationshipMapping->relationshipId);
	const std::string cardinality = relationship != nullptr ? ToLowerAscii(TrimAscii(relationship->type)) : std::string();
	const bool ownedDirection = relationship != nullptr
		&& relationship->sourceEntity == relationshipMapping->relatedEntityId
		&& relationship->targetEntity == mapping->entityId;
	const bool supportedCardinality = cardinality == "one-to-many"
		|| cardinality == "onetomany"
		|| cardinality == "1:n"
		|| cardinality == "1m"
		|| cardinality == "one-to-one"
		|| cardinality == "onetoone"
		|| cardinality == "1:1"
		|| cardinality == "11";

	if(!ownedDirection || !supportedCardinality)
	{
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_collection_not_owned",
			"Relationship-scoped hydration only permits owned parent-to-child one-to-one or one-to-many relationships.",
			operationContext + ".filters");
		return false;
	}

	json& rows = operation["rows"];

	if((cardinality == "one-to-one" || cardinality == "onetoone" || cardinality == "1:1" || cardinality == "11") && rows.size() > 1)
	{
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_one_to_one_ambiguous",
			"One-to-one relationship hydration returned more than one child row.",
			operationContext + ".rows");
		return false;
	}

	if(rows.empty())
	{
		pendingRows.push_back(DataConnectionPendingCollectionHydrationRow{
			entity,
			parentInstance,
			relationshipMapping->relationshipId,
			operation.value("sourceId", std::string()),
			operation.value("sourceName", std::string()),
			operation.value("serverConnectionKey", std::string()),
			operation.value("sourceObjectName", std::string()),
			{},
			operationContext
		});

		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"information",
			"data_connection_load_hydrate_collection_empty",
			"Data-connection load hydration accepted an empty relationship-scoped collection result.",
			operationContext + ".rows");
		return true;
	}

	bool valid = true;

	for(std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
	{
		json& row = rows[rowIndex];
		const std::string rowContext = operationContext + ".rows[" + std::to_string(rowIndex) + "]";

		if(!row.is_object() || !row.contains("values") || !row.at("values").is_array())
		{
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_collection_values_missing",
				"Data-connection load hydration requires each collection row to contain values.",
				rowContext + ".values");
			valid = false;
			continue;
		}

		std::shared_ptr<Instance> templateInstance = entity->GetCleanInstance();

		if(templateInstance == nullptr)
		{
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_collection_instance_template_missing",
				"Data-connection load hydration could not create a collection instance template for validation.",
				rowContext);
			valid = false;
			continue;
		}

		DataConnectionPendingCollectionHydrationRow pendingRow;
		pendingRow.entity = entity;
		pendingRow.parentInstance = parentInstance;
		pendingRow.relationshipId = relationshipMapping->relationshipId;
		pendingRow.sourceId = operation.value("sourceId", std::string());
		pendingRow.sourceName = operation.value("sourceName", std::string());
		pendingRow.serverConnectionKey = operation.value("serverConnectionKey", std::string());
		pendingRow.sourceObjectName = operation.value("sourceObjectName", std::string());
		pendingRow.operationContext = rowContext;

		std::unordered_set<std::string> rowTargets;

		for(std::size_t valueIndex = 0; valueIndex < row.at("values").size(); ++valueIndex)
		{
			const json& value = row.at("values")[valueIndex];
			const std::string valueContext = rowContext + ".values[" + std::to_string(valueIndex) + "]";

			if(!value.is_object())
			{
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_collection_value_invalid",
					"Data-connection load hydration found an invalid collection value entry.",
					valueContext);
				valid = false;
				continue;
			}

			const json* rawValue = FindObjectValue(value, {"value", "Value"});
			const bool isNull = JsonBoolValue(value, {"isNull", "IsNull"}).value_or(rawValue == nullptr || rawValue->is_null());

			if(rawValue == nullptr && !isNull)
			{
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_collection_value_missing",
					"Data-connection load hydration found a non-null collection value entry without a value.",
					valueContext + ".value");
				valid = false;
				continue;
			}

			if(!value.contains("targets") || !value.at("targets").is_array())
			{
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_collection_targets_missing",
					"Data-connection load hydration requires each collection value to include target attributes.",
					valueContext + ".targets");
				valid = false;
				continue;
			}

			const json& targets = value.at("targets");

			if(targets.empty())
			{
				if(IsMetadataOnlyDataConnectionLoadValue(value))
				{
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"information",
						"data_connection_load_hydrate_collection_metadata_value_skipped",
						"Data-connection collection hydration skipped a metadata-only returned value without a runtime target.",
						valueContext + ".targets");
					continue;
				}

				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_collection_no_targets",
					"Data-connection load hydration requires at least one runtime target per collection value.",
					valueContext + ".targets");
				valid = false;
				continue;
			}

			for(std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
			{
				const json& target = targets[targetIndex];
				const std::string targetContext = valueContext + ".targets[" + std::to_string(targetIndex) + "]";

				if(!target.is_object())
				{
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_collection_target_invalid",
						"Data-connection load hydration found an invalid collection target entry.",
						targetContext);
					valid = false;
					continue;
				}

				const std::string entityId = target.value("entityId", std::string());
				const std::string attributeId = target.value("attributeId", std::string());

				if(entityId != mapping->entityId || attributeId.empty())
				{
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_collection_target_missing_identity",
						"Data-connection load hydration requires collection targets to identify the mapped child entity and attribute.",
						targetContext);
					valid = false;
					continue;
				}

				std::shared_ptr<Attribute> attribute = templateInstance->GetAttributeByID(attributeId);

				if(attribute == nullptr)
				{
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_collection_attribute_missing",
						"Data-connection load hydration could not resolve the collection target runtime attribute.",
						targetContext);
					valid = false;
					continue;
				}

				const std::string valueText = DataConnectionHydrationValueToString(rawValue, isNull);

				if(!valueText.empty())
				{
					ValueVariant convertedValue;

					if(!Attribute::TryConvertValue(valueText, static_cast<Attribute::AttributeType>(attribute->GetType()), convertedValue))
					{
						AppendDataConnectionLoadHydrationDiagnostic(
							executionResult,
							"error",
							"data_connection_load_hydrate_collection_value_type_mismatch",
							"Data-connection load hydration could not convert the collection value to the target attribute type.",
							targetContext);
						valid = false;
						continue;
					}
				}

				const std::string targetKey = entityId + "|" + attributeId;

				if(!rowTargets.insert(targetKey).second)
				{
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_collection_duplicate_target",
						"Data-connection load hydration refused the collection row because more than one value targeted the same runtime attribute.",
						targetContext);
					valid = false;
					continue;
				}

				pendingRow.values.push_back(DataConnectionPendingCollectionHydrationValue{
					entityId,
					target.value("entityName", std::string()),
					attributeId,
					target.value("attributeName", std::string()),
					target.value("attributeCoreName", std::string()),
					target.value("attributeDataType", std::string()),
					value.value("sourceFieldName", std::string()),
					value.value("valueType", target.value("valueType", std::string())),
					rawValue != nullptr ? *rawValue : json(nullptr),
					isNull,
					DataConnectionLoadValueIsRecordIdentity(operation, value),
					valueText
				});
			}
		}

		pendingRows.push_back(pendingRow);
	}

	return valid;
}

bool SameDataConnectionRecordIdentitySnapshot(
	const RuleSet::DataConnectionRecordIdentitySnapshot& left,
	const RuleSet::DataConnectionRecordIdentitySnapshot& right)
{
	return left.sourceId == right.sourceId
		&& left.sourceObjectName == right.sourceObjectName
		&& left.snapshotRole == right.snapshotRole
		&& left.entityId == right.entityId
		&& left.instanceId == right.instanceId
		&& left.attributeId == right.attributeId
		&& left.sourceFieldName == right.sourceFieldName;
}

void UpsertDataConnectionRecordIdentitySnapshot(
	RuleSet& ruleSet,
	const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot)
{
	if(ruleSet.dataConnectionRecordIdentitySnapshots == nullptr)
	{
		ruleSet.dataConnectionRecordIdentitySnapshots =
			std::make_shared<std::vector<RuleSet::DataConnectionRecordIdentitySnapshot>>();
	}

	for(RuleSet::DataConnectionRecordIdentitySnapshot& existing : *ruleSet.dataConnectionRecordIdentitySnapshots)
	{
		if(SameDataConnectionRecordIdentitySnapshot(existing, snapshot))
		{
			existing = snapshot;
			return;
		}
	}

	ruleSet.dataConnectionRecordIdentitySnapshots->push_back(snapshot);
}

const RuleSet::DataConnectionAttributeMapping* FindDataConnectionAttributeMapping(
	const RuleSet& ruleSet,
	const std::string& sourceId,
	const std::string& sourceObjectName,
	const std::string& entityId,
	const std::string& attributeId,
	const std::string& sourceFieldName)
{
	if(ruleSet.dataConnectionEntityMappings == nullptr)
	{
		return nullptr;
	}

	for(const RuleSet::DataConnectionEntityMapping& entityMapping : *ruleSet.dataConnectionEntityMappings)
	{
		if(entityMapping.sourceId != sourceId
			|| entityMapping.sourceObjectName != sourceObjectName
			|| entityMapping.entityId != entityId)
		{
			continue;
		}

		for(const RuleSet::DataConnectionAttributeMapping& attributeMapping : entityMapping.attributeMappings)
		{
			if(attributeMapping.attributeId == attributeId
				&& attributeMapping.sourceFieldName == sourceFieldName)
			{
				return &attributeMapping;
			}
		}
	}

	return nullptr;
}

void CaptureDataConnectionRecordIdentitySnapshots(
	const json& operation,
	std::vector<RuleSet::DataConnectionRecordIdentitySnapshot>& snapshots,
	json& executionResult,
	const std::string& operationContext)
{
	if(!operation.contains("filters") || !operation.at("filters").is_array())
	{
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"warning",
			"data_connection_load_hydrate_identity_filters_missing",
			"Data-connection load hydration could not snapshot original record identity because the executed operation did not return filters.",
			operationContext + ".filters");
		return;
	}

	const json& filters = operation.at("filters");

	for(std::size_t filterIndex = 0; filterIndex < filters.size(); ++filterIndex)
	{
		const json& filter = filters[filterIndex];
		const std::string filterContext = operationContext + ".filters[" + std::to_string(filterIndex) + "]";

		if(!filter.is_object())
		{
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"warning",
				"data_connection_load_hydrate_identity_filter_invalid",
				"Data-connection load hydration skipped an invalid original record identity filter.",
				filterContext);
			continue;
		}

		const json* rawValue = FindObjectValue(filter, {"value", "Value"});
		const bool isKnown = JsonBoolValue(filter, {"isKnown", "IsKnown"}).value_or(rawValue != nullptr && !rawValue->is_null());
		const bool isCertain = JsonBoolValue(filter, {"isCertain", "IsCertain"}).value_or(isKnown);

		RuleSet::DataConnectionRecordIdentitySnapshot snapshot;

		snapshot.sourceId = operation.value("sourceId", std::string());
		snapshot.sourceName = operation.value("sourceName", std::string());
		snapshot.serverConnectionKey = operation.value("serverConnectionKey", std::string());
		snapshot.sourceObjectName = operation.value("sourceObjectName", std::string());
		snapshot.snapshotRole = "RecordIdentity";
		snapshot.entityId = filter.value("entityId", std::string());
		snapshot.entityName = filter.value("entityName", std::string());
		snapshot.instanceId = filter.value("instanceId", std::string());
		snapshot.attributeId = filter.value("attributeId", std::string());
		snapshot.attributeName = filter.value("attributeName", std::string());
		snapshot.attributeCoreName = filter.value("attributeCoreName", std::string());
		snapshot.attributeDataType = filter.value("attributeDataType", std::string());
		snapshot.sourceFieldName = filter.value("sourceFieldName", std::string());
		snapshot.operatorName = filter.value("operator", std::string("Equals"));
		snapshot.valueType = filter.value("valueType", std::string());
		snapshot.value = rawValue != nullptr ? *rawValue : json(nullptr);
		snapshot.isKnown = isKnown;
		snapshot.isCertain = isCertain;
		snapshot.isUncertain = JsonBoolValue(filter, {"isUncertain", "IsUncertain"}).value_or(false);
		snapshot.knowledgeState = filter.value("knowledgeState", std::string());

		if(snapshot.knowledgeState.empty())
		{
			snapshot.knowledgeState = snapshot.isKnown ? "Known" : "Unknown";
		}

		if(snapshot.sourceId.empty()
			|| snapshot.sourceObjectName.empty()
			|| snapshot.entityId.empty()
			|| snapshot.instanceId.empty()
			|| snapshot.attributeId.empty()
			|| snapshot.sourceFieldName.empty()
			|| !snapshot.isKnown
			|| !snapshot.isCertain
			|| snapshot.value.is_null())
		{
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"warning",
				"data_connection_load_hydrate_identity_filter_incomplete",
				"Data-connection load hydration skipped an incomplete original record identity filter.",
				filterContext);
			continue;
		}

		snapshots.push_back(snapshot);
	}
}

void ApplyDataConnectionLoadHydration(RuleSet& ruleSet, json& executionResult, bool evaluateChanges = true)
{
	executionResult["hydrateRequested"] = true;
	executionResult["hydrationAttempted"] = true;
	executionResult["hydrated"] = false;
	executionResult["hydrationRefused"] = false;
	executionResult["hydratedAttributeCount"] = 0;
	executionResult["hydrationDiagnostics"] = json::array();

	if(!executionResult.is_object() ||
	   !executionResult.value("succeeded", false) ||
	   executionResult.value("dryRun", true) ||
	   !executionResult.value("readExecuted", false))
	{
		executionResult["hydrationRefused"] = true;
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_execution_not_confirmed",
			"Data-connection load hydration requires a successful non-dry-run response with readExecuted=true.",
			"sessionModel.dataConnectionLoadExecution");
		return;
	}

	if(!executionResult.contains("operations") || !executionResult.at("operations").is_array())
	{
		executionResult["hydrationRefused"] = true;
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_load_hydrate_operations_missing",
			"Data-connection load hydration requires operation rows in the execution response.",
			"sessionModel.dataConnectionLoadExecution.operations");
		return;
	}

	bool canHydrate = true;
	std::vector<DataConnectionLoadHydrationCandidate> candidates;
	std::vector<DataConnectionPendingCollectionHydrationRow> pendingCollectionRows;
	std::vector<RuleSet::DataConnectionRecordIdentitySnapshot> identitySnapshots;
	std::vector<DataConnectionLoadHydrationRelationshipCandidate> relationshipCandidates;
	std::unordered_set<std::string> relationshipCandidateKeys;
	std::unordered_map<std::string, std::string> valuesByTarget;

	json& operations = executionResult["operations"];

	for(std::size_t operationIndex = 0; operationIndex < operations.size(); ++operationIndex)
	{
		json& operation = operations[operationIndex];
		const std::string operationContext = "sessionModel.dataConnectionLoadExecution.operations[" + std::to_string(operationIndex) + "]";

		if(!operation.is_object())
		{
			canHydrate = false;
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_operation_invalid",
				"Data-connection load hydration found an invalid operation entry.",
				operationContext);
			continue;
		}

		CaptureDataConnectionRecordIdentitySnapshots(operation, identitySnapshots, executionResult, operationContext);

		if(!operation.value("executed", false))
		{
			canHydrate = false;
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_operation_not_executed",
				"Data-connection load hydration refuses operation results that were not marked executed.",
				operationContext);
			continue;
		}

		if(!operation.contains("rows") || !operation.at("rows").is_array())
		{
			canHydrate = false;
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_rows_missing",
				"Data-connection load hydration requires each executed operation to return a rows array.",
				operationContext + ".rows");
			continue;
		}

		json& rows = operation["rows"];

		if(IsSelectedRecordRelationshipOperation(operation))
		{
			if(!ValidateDataConnectionRelationshipCollectionOperation(
				ruleSet,
				executionResult,
				operation,
				operationContext,
				pendingCollectionRows))
			{
				canHydrate = false;
			}

			continue;
		}

		if(rows.empty())
		{
			canHydrate = false;
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_no_rows",
				"Data-connection load hydration refused the result because the executed operation returned no rows.",
				operationContext + ".rows");
			continue;
		}

		if(rows.size() > 1)
		{
			canHydrate = false;
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_multiple_rows",
				"Data-connection load hydration refused the result because the executed operation returned multiple rows.",
				operationContext + ".rows");
			continue;
		}

		json& row = rows[0];

		if(!row.is_object() || !row.contains("values") || !row.at("values").is_array())
		{
			canHydrate = false;
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_values_missing",
				"Data-connection load hydration requires the single returned row to contain values.",
				operationContext + ".rows[0].values");
			continue;
		}

		json& values = row["values"];

		for(std::size_t valueIndex = 0; valueIndex < values.size(); ++valueIndex)
		{
			const json& value = values[valueIndex];
			const std::string valueContext = operationContext + ".rows[0].values[" + std::to_string(valueIndex) + "]";

			if(!value.is_object())
			{
				canHydrate = false;
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_value_invalid",
					"Data-connection load hydration found an invalid returned value entry.",
					valueContext);
				continue;
			}

			const json* rawValue = FindObjectValue(value, {"value", "Value"});
			const bool isNull = JsonBoolValue(value, {"isNull", "IsNull"}).value_or(rawValue == nullptr || rawValue->is_null());

			if(rawValue == nullptr && !isNull)
			{
				canHydrate = false;
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_value_missing",
					"Data-connection load hydration found a non-null value entry without a value.",
					valueContext + ".value");
				continue;
			}

			if(!value.contains("targets") || !value.at("targets").is_array())
			{
				canHydrate = false;
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_targets_missing",
					"Data-connection load hydration requires each returned value to include target attributes.",
					valueContext + ".targets");
				continue;
			}

			const std::string sourceFieldName = value.value("sourceFieldName", std::string());
			const std::string valueText = DataConnectionHydrationValueToString(rawValue, isNull);
			const json& targets = value.at("targets");

			if(targets.empty())
			{
				if(IsMetadataOnlyDataConnectionLoadValue(value))
				{
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"information",
						"data_connection_load_hydrate_metadata_value_skipped",
						"Data-connection load hydration skipped a metadata-only returned value without a runtime target.",
						valueContext + ".targets");
					continue;
				}

				canHydrate = false;
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_no_targets",
					"Data-connection load hydration requires at least one runtime target per returned value.",
					valueContext + ".targets");
				continue;
			}

			for(std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
			{
				const json& target = targets[targetIndex];
				const std::string targetContext = valueContext + ".targets[" + std::to_string(targetIndex) + "]";

				if(!target.is_object())
				{
					canHydrate = false;
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_target_invalid",
						"Data-connection load hydration found an invalid target entry.",
						targetContext);
					continue;
				}

				const std::string entityId = target.value("entityId", std::string());
				const std::string attributeId = target.value("attributeId", std::string());

				if(entityId.empty() || attributeId.empty())
				{
					canHydrate = false;
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_target_missing_identity",
						"Data-connection load hydration requires target entityId and attributeId.",
						targetContext);
					continue;
				}

				std::shared_ptr<Entity> entity = FindDataConnectionHydrationEntity(ruleSet, entityId);

				if(entity == nullptr)
				{
					canHydrate = false;
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_entity_missing",
						"Data-connection load hydration could not resolve the target runtime entity.",
						targetContext);
					continue;
				}

				if(entity->instances.size() != 1)
				{
					canHydrate = false;
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						entity->instances.empty() ? "data_connection_load_hydrate_no_instances" : "data_connection_load_hydrate_multiple_instances",
						"Data-connection load hydration requires exactly one runtime instance for the target entity.",
						targetContext);
					continue;
				}

				std::shared_ptr<Instance> instance = entity->instances[0];
				std::shared_ptr<Attribute> attribute = instance != nullptr ? instance->GetAttributeByID(attributeId) : nullptr;

				if(attribute == nullptr)
				{
					canHydrate = false;
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_attribute_missing",
						"Data-connection load hydration could not resolve the target runtime attribute.",
						targetContext);
					continue;
				}

				if(!valueText.empty())
				{
					ValueVariant convertedValue;

					if(!Attribute::TryConvertValue(valueText, static_cast<Attribute::AttributeType>(attribute->GetType()), convertedValue))
					{
						canHydrate = false;
						AppendDataConnectionLoadHydrationDiagnostic(
							executionResult,
							"error",
							"data_connection_load_hydrate_value_type_mismatch",
							"Data-connection load hydration could not convert the returned value to the target attribute type.",
							targetContext);
						continue;
					}
				}

				const std::string targetKey = entityId + "|" + instance->instanceId + "|" + attributeId;
				const auto existing = valuesByTarget.find(targetKey);

				if(existing != valuesByTarget.end())
				{
					canHydrate = false;
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_duplicate_target",
						"Data-connection load hydration refused the result because more than one returned value targeted the same runtime attribute.",
						targetContext);
					continue;
				}

				valuesByTarget[targetKey] = valueText;
				candidates.push_back(DataConnectionLoadHydrationCandidate{
					attribute,
					entityId,
					instance->instanceId,
					attributeId,
					sourceFieldName,
					valueText
				});

				const RuleSet::DataConnectionAttributeMapping* attributeMapping =
					FindDataConnectionAttributeMapping(
						ruleSet,
						operation.value("sourceId", std::string()),
						operation.value("sourceObjectName", std::string()),
						entityId,
						attributeId,
						sourceFieldName);

				if(attributeMapping != nullptr && attributeMapping->saveConflictCheck)
				{
					if(rawValue == nullptr || isNull)
					{
						AppendDataConnectionLoadHydrationDiagnostic(
							executionResult,
							"warning",
							"data_connection_load_hydrate_conflict_check_value_missing",
							"Data-connection load hydration could not snapshot an optimistic concurrency check because the returned value was null or missing.",
							targetContext);
					}
					else
					{
						RuleSet::DataConnectionRecordIdentitySnapshot snapshot;

						snapshot.sourceId = operation.value("sourceId", std::string());
						snapshot.sourceName = operation.value("sourceName", std::string());
						snapshot.serverConnectionKey = operation.value("serverConnectionKey", std::string());
						snapshot.sourceObjectName = operation.value("sourceObjectName", std::string());
						snapshot.snapshotRole = "OptimisticConcurrencyCheck";
						snapshot.entityId = entityId;
						snapshot.entityName = target.value("entityName", std::string());
						snapshot.instanceId = instance->instanceId;
						snapshot.attributeId = attributeId;
						snapshot.attributeName = target.value("attributeName", std::string());
						snapshot.attributeCoreName = target.value("attributeCoreName", std::string());
						snapshot.attributeDataType = target.value("attributeDataType", std::string());
						snapshot.sourceFieldName = sourceFieldName;
						snapshot.operatorName = "Equals";
						snapshot.valueType = value.value("valueType", target.value("valueType", std::string()));
						snapshot.value = *rawValue;
						snapshot.isKnown = true;
						snapshot.isCertain = true;
						snapshot.isUncertain = false;
						snapshot.knowledgeState = "Known";

						identitySnapshots.push_back(snapshot);
					}
				}
			}
		}
	}

	if(!canHydrate)
	{
		executionResult["hydrationRefused"] = true;
		executionResult["message"] = "Data-connection load execution completed, but hydration was refused because the result was ambiguous or invalid.";
		return;
	}

	if(canHydrate && !pendingCollectionRows.empty())
	{
		std::unordered_set<std::string> clearedCollections;

		for(const DataConnectionPendingCollectionHydrationRow& pendingRow : pendingCollectionRows)
		{
			if(pendingRow.entity == nullptr || pendingRow.parentInstance == nullptr)
			{
				executionResult["hydrationRefused"] = true;
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_collection_apply_scope_missing",
					"Data-connection load hydration could not apply a pre-validated collection row because its scope was missing.",
					pendingRow.operationContext);
				return;
			}

			const std::string collectionKey = pendingRow.entity->entityid + "|" + pendingRow.parentInstance->instanceId;

			if(clearedCollections.insert(collectionKey).second)
			{
				ClearDataConnectionCollectionHydrationEntity(
					ruleSet,
					pendingRow.entity,
					pendingRow.parentInstance,
					pendingRow.relationshipId);
				AddDataConnectionHydrationRelationshipCandidate(
					ruleSet,
					relationshipCandidates,
					relationshipCandidateKeys,
					pendingRow.relationshipId,
					pendingRow.parentInstance,
					nullptr);
			}
		}

		for(const DataConnectionPendingCollectionHydrationRow& pendingRow : pendingCollectionRows)
		{
			if(pendingRow.values.empty())
			{
				continue;
			}

			const std::string instanceId = pendingRow.entity->AddInstance(
				pendingRow.parentInstance,
				ruleSet.entities,
				pendingRow.relationshipId);
			std::shared_ptr<Instance> instance = pendingRow.entity->GetInstanceById(instanceId);

			if(instance == nullptr)
			{
				executionResult["hydrationRefused"] = true;
				AppendDataConnectionLoadHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_load_hydrate_collection_instance_create_failed",
					"Data-connection load hydration could not create a runtime instance for a pre-validated collection row.",
					pendingRow.operationContext);
				return;
			}

			AddDataConnectionHydrationRelationshipCandidate(
				ruleSet,
				relationshipCandidates,
				relationshipCandidateKeys,
				pendingRow.relationshipId,
				pendingRow.parentInstance,
				instance);

			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"information",
				"data_connection_load_hydrate_collection_instance_created",
				"Data-connection load hydration created a runtime instance for a relationship-scoped collection row.",
				pendingRow.entity->entityid + "/" + instanceId);

			for(const DataConnectionPendingCollectionHydrationValue& pendingValue : pendingRow.values)
			{
				std::shared_ptr<Attribute> attribute = instance->GetAttributeByID(pendingValue.attributeId);

				if(attribute == nullptr)
				{
					executionResult["hydrationRefused"] = true;
					AppendDataConnectionLoadHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_load_hydrate_collection_apply_attribute_missing",
						"Data-connection load hydration could not resolve a pre-validated collection attribute while applying values.",
						pendingRow.operationContext + "/" + pendingValue.attributeId);
					return;
				}

				candidates.push_back(DataConnectionLoadHydrationCandidate{
					attribute,
					pendingValue.entityId,
					instanceId,
					pendingValue.attributeId,
					pendingValue.sourceFieldName,
					pendingValue.valueText
				});

				if(pendingValue.isRecordIdentity
					&& !pendingValue.sourceFieldName.empty()
					&& !pendingValue.isNull
					&& !pendingValue.value.is_null())
				{
					RuleSet::DataConnectionRecordIdentitySnapshot snapshot;

					snapshot.sourceId = pendingRow.sourceId;
					snapshot.sourceName = pendingRow.sourceName;
					snapshot.serverConnectionKey = pendingRow.serverConnectionKey;
					snapshot.sourceObjectName = pendingRow.sourceObjectName;
					snapshot.snapshotRole = "RecordIdentity";
					snapshot.entityId = pendingValue.entityId;
					snapshot.entityName = pendingValue.entityName;
					snapshot.instanceId = instanceId;
					snapshot.attributeId = pendingValue.attributeId;
					snapshot.attributeName = pendingValue.attributeName;
					snapshot.attributeCoreName = pendingValue.attributeCoreName;
					snapshot.attributeDataType = pendingValue.attributeDataType;
					snapshot.sourceFieldName = pendingValue.sourceFieldName;
					snapshot.operatorName = "Equals";
					snapshot.valueType = pendingValue.valueType;
					snapshot.value = pendingValue.value;
					snapshot.isKnown = true;
					snapshot.isCertain = true;
					snapshot.isUncertain = false;
					snapshot.knowledgeState = "Known";

					identitySnapshots.push_back(snapshot);
				}
			}
		}
	}

	if(candidates.empty() && pendingCollectionRows.empty())
	{
		executionResult["hydrationRefused"] = true;
		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"warning",
			"data_connection_load_hydrate_no_candidates",
			"Data-connection load hydration found no target attributes to apply.",
			"sessionModel.dataConnectionLoadExecution.operations");
		return;
	}

	SessionModelRuleEvaluator::EvaluationStats evaluationStats;
	SessionModelRuleEvaluator evaluator;
	std::unordered_set<std::string> changedTargetKeys;
	int hydratedAttributeCount = 0;

	for(const DataConnectionLoadHydrationCandidate& candidate : candidates)
	{
		const std::string before = candidate.attribute->IsUnknown() ? std::string() : candidate.attribute->GetStringValue();
		const bool wasUnknown = candidate.attribute->IsUnknown();
		const bool applied = candidate.attribute->SetValueFromString(candidate.valueText, false);

		if(!applied)
		{
			executionResult["hydrationRefused"] = true;
			executionResult["hydrated"] = false;
			AppendDataConnectionLoadHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_load_hydrate_apply_failed",
				"Data-connection load hydration failed while applying a pre-validated value.",
				candidate.entityId + "/" + candidate.instanceId + "/" + candidate.attributeId);
			return;
		}

		const std::string after = candidate.attribute->IsUnknown() ? std::string() : candidate.attribute->GetStringValue();
		const bool isUnknown = candidate.attribute->IsUnknown();
		const std::string targetKey = candidate.entityId + "|" + candidate.instanceId + "|" + candidate.attributeId;

		++hydratedAttributeCount;

		if(evaluateChanges
			&& changedTargetKeys.insert(targetKey).second
			&& (wasUnknown != isUnknown || before != after))
		{
			MergeEvaluationStats(
				evaluationStats,
				evaluator.EvaluateChangedAttribute(ruleSet, candidate.entityId, candidate.instanceId, candidate.attributeId));
		}

		AppendDataConnectionLoadHydrationDiagnostic(
			executionResult,
			"information",
			"data_connection_load_hydrate_attribute_applied",
			"Data-connection load hydration applied a returned field to a runtime attribute.",
			candidate.entityId + "/" + candidate.instanceId + "/" + candidate.attributeId);
	}

	if(evaluateChanges)
	{
		for(const DataConnectionLoadHydrationRelationshipCandidate& relationshipCandidate : relationshipCandidates)
		{
			MergeEvaluationStats(
				evaluationStats,
				evaluator.EvaluateChangedRelationship(ruleSet, relationshipCandidate.entityId, relationshipCandidate.instanceId, relationshipCandidate.relationshipId));
		}
	}

	for(const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot : identitySnapshots)
	{
		UpsertDataConnectionRecordIdentitySnapshot(ruleSet, snapshot);
	}

	executionResult["hydrated"] = true;
	executionResult["hydrationRefused"] = false;
	executionResult["hydratedAttributeCount"] = hydratedAttributeCount;
	executionResult["hydratedRelationshipEvaluationCount"] = relationshipCandidates.size();
	executionResult["recordIdentitySnapshotCount"] = identitySnapshots.size();
	executionResult["hydrationEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats);
	executionResult["message"] = "Data-connection load execution completed and hydrated " + std::to_string(hydratedAttributeCount) + " runtime attribute(s).";
}

void AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(json& result,
                                                               const std::string& severity,
                                                               const std::string& code,
                                                               const std::string& message,
                                                               const std::string& context)
{
	const json diagnostic = MakeDataConnectionDryRunDiagnostic(severity, code, message, context);

	if(!result.contains("diagnostics") || !result.at("diagnostics").is_array())
	{
		result["diagnostics"] = json::array();
	}

	result["diagnostics"].push_back(diagnostic);

	if(!result.contains("generatedValueHydrationDiagnostics") || !result.at("generatedValueHydrationDiagnostics").is_array())
	{
		result["generatedValueHydrationDiagnostics"] = json::array();
	}

	result["generatedValueHydrationDiagnostics"].push_back(diagnostic);
}

void ApplyDataConnectionSaveGeneratedValueHydration(RuleSet& ruleSet, json& executionResult)
{
	executionResult["generatedValueHydrationAttempted"] = true;
	executionResult["generatedValueHydrated"] = false;
	executionResult["generatedValueHydrationRefused"] = false;
	executionResult["generatedValueHydratedAttributeCount"] = 0;
	executionResult["generatedValueRecordIdentitySnapshotCount"] = 0;
	executionResult["generatedValueHydrationDiagnostics"] = json::array();

	if(!executionResult.is_object()
	   || !executionResult.value("succeeded", false)
	   || executionResult.value("dryRun", true)
	   || !executionResult.value("committed", false))
	{
		executionResult["generatedValueHydrationRefused"] = true;
		AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_save_generated_hydrate_execution_not_confirmed",
			"Generated-value hydration requires a successful non-dry-run committed save response.",
			"sessionModel.dataConnectionSaveExecution");
		return;
	}

	if(!executionResult.contains("operations") || !executionResult.at("operations").is_array())
	{
		executionResult["generatedValueHydrationRefused"] = true;
		AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
			executionResult,
			"error",
			"data_connection_save_generated_hydrate_operations_missing",
			"Generated-value hydration requires operation results in the committed save response.",
			"sessionModel.dataConnectionSaveExecution.operations");
		return;
	}

	bool canHydrate = true;
	bool sawCreatedOperation = false;
	std::vector<DataConnectionLoadHydrationCandidate> candidates;
	std::vector<RuleSet::DataConnectionRecordIdentitySnapshot> identitySnapshots;
	std::unordered_map<std::string, std::string> valuesByTarget;

	json& operations = executionResult["operations"];

	for(std::size_t operationIndex = 0; operationIndex < operations.size(); ++operationIndex)
	{
		json& operation = operations[operationIndex];
		const std::string operationContext = "sessionModel.dataConnectionSaveExecution.operations[" + std::to_string(operationIndex) + "]";

		if(!operation.is_object())
		{
			canHydrate = false;
			AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_save_generated_hydrate_operation_invalid",
				"Generated-value hydration found an invalid operation entry.",
				operationContext);
			continue;
		}

		if(operation.value("saveOutcome", std::string()) != "Created")
		{
			continue;
		}

		sawCreatedOperation = true;

		if(!operation.value("executed", false))
		{
			canHydrate = false;
			AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_save_generated_hydrate_operation_not_executed",
				"Generated-value hydration refuses created operation results that were not marked executed.",
				operationContext);
			continue;
		}

		if(!operation.contains("generatedValues") || !operation.at("generatedValues").is_array())
		{
			AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
				executionResult,
				"information",
				"data_connection_save_generated_hydrate_values_absent",
				"The committed create operation returned no generated values to hydrate.",
				operationContext + ".generatedValues");
			continue;
		}

		json& generatedValues = operation["generatedValues"];

		for(std::size_t valueIndex = 0; valueIndex < generatedValues.size(); ++valueIndex)
		{
			const json& value = generatedValues[valueIndex];
			const std::string valueContext = operationContext + ".generatedValues[" + std::to_string(valueIndex) + "]";

			if(!value.is_object())
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_value_invalid",
					"Generated-value hydration found an invalid generated value entry.",
					valueContext);
				continue;
			}

			const json* rawValue = FindObjectValue(value, {"value", "Value"});
			const bool isNull = JsonBoolValue(value, {"isNull", "IsNull"}).value_or(rawValue == nullptr || rawValue->is_null());

			if(rawValue == nullptr && !isNull)
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_value_missing",
					"Generated-value hydration found a non-null generated value entry without a value.",
					valueContext + ".value");
				continue;
			}

			if(isNull)
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_value_null",
					"Generated-value hydration cannot hydrate a null database-generated key value.",
					valueContext + ".value");
				continue;
			}

			const std::string entityId = operation.value("entityId", std::string());
			const std::string instanceId = operation.value("instanceId", std::string());
			const std::string attributeId = value.value("attributeId", std::string());
			const std::string sourceFieldName = value.value("sourceFieldName", std::string());

			if(entityId.empty() || instanceId.empty() || attributeId.empty() || sourceFieldName.empty())
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_target_missing_identity",
					"Generated-value hydration requires operation entityId, instanceId, generated attributeId, and sourceFieldName.",
					valueContext);
				continue;
			}

			std::shared_ptr<Entity> entity = FindDataConnectionHydrationEntity(ruleSet, entityId);

			if(entity == nullptr)
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_entity_missing",
					"Generated-value hydration could not resolve the target runtime entity.",
					valueContext);
				continue;
			}

			std::shared_ptr<Instance> instance = entity->GetInstanceById(instanceId);
			std::shared_ptr<Attribute> attribute = instance != nullptr ? instance->GetAttributeByID(attributeId) : nullptr;

			if(instance == nullptr)
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_instance_missing",
					"Generated-value hydration could not resolve the target runtime instance.",
					valueContext);
				continue;
			}

			if(attribute == nullptr)
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_attribute_missing",
					"Generated-value hydration could not resolve the target runtime attribute.",
					valueContext);
				continue;
			}

			const std::string valueText = DataConnectionHydrationValueToString(rawValue, isNull);

			if(!valueText.empty())
			{
				ValueVariant convertedValue;

				if(!Attribute::TryConvertValue(valueText, static_cast<Attribute::AttributeType>(attribute->GetType()), convertedValue))
				{
					canHydrate = false;
					AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
						executionResult,
						"error",
						"data_connection_save_generated_hydrate_value_type_mismatch",
						"Generated-value hydration could not convert the returned generated value to the target attribute type.",
						valueContext);
					continue;
				}
			}

			const std::string targetKey = entityId + "|" + instanceId + "|" + attributeId;
			const auto existing = valuesByTarget.find(targetKey);

			if(existing != valuesByTarget.end())
			{
				canHydrate = false;
				AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
					executionResult,
					"error",
					"data_connection_save_generated_hydrate_duplicate_target",
					"Generated-value hydration refused the result because more than one generated value targeted the same runtime attribute.",
					valueContext);
				continue;
			}

			valuesByTarget[targetKey] = valueText;
			candidates.push_back(DataConnectionLoadHydrationCandidate{
				attribute,
				entityId,
				instanceId,
				attributeId,
				sourceFieldName,
				valueText
			});

			RuleSet::DataConnectionRecordIdentitySnapshot snapshot;

			snapshot.sourceId = operation.value("sourceId", std::string());
			snapshot.sourceName = operation.value("sourceName", std::string());
			snapshot.serverConnectionKey = operation.value("serverConnectionKey", std::string());
			snapshot.sourceObjectName = operation.value("sourceObjectName", std::string());
			snapshot.snapshotRole = "RecordIdentity";
			snapshot.entityId = entityId;
			snapshot.entityName = operation.value("entityName", std::string());
			snapshot.instanceId = instanceId;
			snapshot.attributeId = attributeId;
			snapshot.attributeName = value.value("attributeName", std::string());
			snapshot.attributeCoreName = value.value("attributeCoreName", std::string());
			snapshot.attributeDataType = value.value("attributeDataType", std::string());
			snapshot.sourceFieldName = sourceFieldName;
			snapshot.operatorName = "Equals";
			snapshot.valueType = value.value("valueType", std::string());
			snapshot.value = *rawValue;
			snapshot.isKnown = true;
			snapshot.isCertain = true;
			snapshot.isUncertain = false;
			snapshot.knowledgeState = "Known";

			identitySnapshots.push_back(snapshot);
		}
	}

	if(!canHydrate)
	{
		executionResult["generatedValueHydrationRefused"] = true;
		executionResult["message"] = executionResult.value("message", std::string()) + " Generated-value hydration was refused because the returned generated values were ambiguous or invalid.";
		return;
	}

	if(candidates.empty())
	{
		if(sawCreatedOperation)
		{
			AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
				executionResult,
				"information",
				"data_connection_save_generated_hydrate_no_candidates",
				"Committed create execution returned no generated values to hydrate.",
				"sessionModel.dataConnectionSaveExecution.operations");
		}

		return;
	}

	SessionModelRuleEvaluator::EvaluationStats evaluationStats;
	SessionModelRuleEvaluator evaluator;
	std::unordered_set<std::string> changedTargetKeys;
	int hydratedAttributeCount = 0;

	for(const DataConnectionLoadHydrationCandidate& candidate : candidates)
	{
		const std::string before = candidate.attribute->IsUnknown() ? std::string() : candidate.attribute->GetStringValue();
		const bool wasUnknown = candidate.attribute->IsUnknown();
		const bool applied = candidate.attribute->SetValueFromString(candidate.valueText, false);

		if(!applied)
		{
			executionResult["generatedValueHydrationRefused"] = true;
			executionResult["generatedValueHydrated"] = false;
			AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
				executionResult,
				"error",
				"data_connection_save_generated_hydrate_apply_failed",
				"Generated-value hydration failed while applying a pre-validated value.",
				candidate.entityId + "/" + candidate.instanceId + "/" + candidate.attributeId);
			return;
		}

		const std::string after = candidate.attribute->IsUnknown() ? std::string() : candidate.attribute->GetStringValue();
		const bool isUnknown = candidate.attribute->IsUnknown();
		const std::string targetKey = candidate.entityId + "|" + candidate.instanceId + "|" + candidate.attributeId;

		++hydratedAttributeCount;

		if(changedTargetKeys.insert(targetKey).second && (wasUnknown != isUnknown || before != after))
		{
			MergeEvaluationStats(
				evaluationStats,
				evaluator.EvaluateChangedAttribute(ruleSet, candidate.entityId, candidate.instanceId, candidate.attributeId));
		}

		AppendDataConnectionSaveGeneratedValueHydrationDiagnostic(
			executionResult,
			"information",
			"data_connection_save_generated_hydrate_attribute_applied",
			"Generated-value hydration applied a returned database-generated value to a runtime attribute.",
			candidate.entityId + "/" + candidate.instanceId + "/" + candidate.attributeId);
	}

	for(const RuleSet::DataConnectionRecordIdentitySnapshot& snapshot : identitySnapshots)
	{
		UpsertDataConnectionRecordIdentitySnapshot(ruleSet, snapshot);
	}

	executionResult["generatedValueHydrated"] = true;
	executionResult["generatedValueHydrationRefused"] = false;
	executionResult["generatedValueHydratedAttributeCount"] = hydratedAttributeCount;
	executionResult["generatedValueRecordIdentitySnapshotCount"] = identitySnapshots.size();
	executionResult["generatedValueHydrationEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats);
	executionResult["message"] = executionResult.value("message", std::string()) + " Generated-value hydration applied " + std::to_string(hydratedAttributeCount) + " runtime attribute(s).";
}

struct RelationshipMutation
{
	string entityId;
	string instanceId;
	string relationshipId;
};

string RelationshipMutationKey(const RelationshipMutation& mutation)
{
	return mutation.entityId + "\x1f" + mutation.instanceId + "\x1f" + mutation.relationshipId;
}

shared_ptr<Relationship> FindRuntimeRelationshipById(const RuleSet& ruleSet, const string& relationshipId)
{
	if(relationshipId.empty() || ruleSet.entities == nullptr)
	{
		return nullptr;
	}

	for(const shared_ptr<Entity>& entity : *ruleSet.entities)
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

shared_ptr<Relationship> FindRuntimeRelationshipBetweenEntities(const RuleSet& ruleSet, const string& leftEntityId, const string& rightEntityId)
{
	if(leftEntityId.empty() || rightEntityId.empty() || ruleSet.entities == nullptr)
	{
		return nullptr;
	}

	for(const shared_ptr<Entity>& entity : *ruleSet.entities)
	{
		if(entity == nullptr)
		{
			continue;
		}

		for(const shared_ptr<Relationship>& relationship : entity->relationships)
		{
			if(relationship == nullptr || relationship->id.empty())
			{
				continue;
			}

			const bool matchesForward = relationship->sourceEntity == leftEntityId && relationship->targetEntity == rightEntityId;
			const bool matchesReverse = relationship->sourceEntity == rightEntityId && relationship->targetEntity == leftEntityId;

			if(matchesForward || matchesReverse)
			{
				return relationship;
			}
		}
	}

	return nullptr;
}

void AddRelationshipMutation(vector<RelationshipMutation>& mutations,
                             unordered_set<string>& mutationKeys,
                             const string& entityId,
                             const string& instanceId,
                             const string& relationshipId)
{
	if(entityId.empty() || instanceId.empty() || relationshipId.empty())
	{
		return;
	}

	RelationshipMutation mutation{entityId, instanceId, relationshipId};

	if(mutationKeys.insert(RelationshipMutationKey(mutation)).second)
	{
		mutations.push_back(mutation);
	}
}

void AddRelationshipMutationFromLink(vector<RelationshipMutation>& mutations,
                                     unordered_set<string>& mutationKeys,
                                     const InstanceRelationshipLink& link,
                                     const string& removedInstanceId = "")
{
	if(link.relationshipId.empty())
	{
		return;
	}

	if(link.sourceInstanceId != removedInstanceId)
	{
		AddRelationshipMutation(mutations, mutationKeys, link.sourceEntityId, link.sourceInstanceId, link.relationshipId);

		return;
	}

	if(link.targetInstanceId != removedInstanceId)
	{
		AddRelationshipMutation(mutations, mutationKeys, link.targetEntityId, link.targetInstanceId, link.relationshipId);
	}
}

void AddCollectRelationshipMutation(vector<RelationshipMutation>& mutations,
                                    unordered_set<string>& mutationKeys,
                                    const RuleSet& ruleSet,
                                    const string& relationshipId,
                                    const shared_ptr<Instance>& parentInstance,
                                    const shared_ptr<Instance>& childInstance)
{
	if(relationshipId.empty() || parentInstance == nullptr)
	{
		return;
	}

	shared_ptr<Relationship> relationship = FindRuntimeRelationshipById(ruleSet, relationshipId);

	if(relationship != nullptr)
	{
		if(childInstance != nullptr && relationship->sourceEntity == childInstance->entityId)
		{
			AddRelationshipMutation(mutations, mutationKeys, childInstance->entityId, childInstance->instanceId, relationshipId);

			return;
		}

		if(relationship->sourceEntity == parentInstance->entityId || relationship->targetEntity == parentInstance->entityId)
		{
			AddRelationshipMutation(mutations, mutationKeys, parentInstance->entityId, parentInstance->instanceId, relationshipId);

			return;
		}
	}

	AddRelationshipMutation(mutations, mutationKeys, parentInstance->entityId, parentInstance->instanceId, relationshipId);
}

vector<RelationshipMutation> CollectRelationshipMutationsForInstance(RuleSet& ruleSet, const string& entityId, const string& instanceId)
{
	vector<RelationshipMutation> mutations;
	unordered_set<string> mutationKeys;
	DataManager dm;
	shared_ptr<Instance> instance = dm.GetEntityInstanceById(ruleSet.entities, entityId, instanceId);

	if(instance == nullptr)
	{
		return mutations;
	}

	for(const InstanceRelationshipLink& link : instance->relationshipLinks)
	{
		AddRelationshipMutationFromLink(mutations, mutationKeys, link, instanceId);
	}

	return mutations;
}

SessionModelRuleEvaluator::EvaluationStats EvaluateRelationshipMutations(RuleSet& ruleSet, const vector<RelationshipMutation>& mutations)
{
	SessionModelRuleEvaluator::EvaluationStats stats;

	if(mutations.empty() || ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	SessionModelRuleEvaluator evaluator;

	for(const RelationshipMutation& mutation : mutations)
	{
		MergeEvaluationStats(stats, evaluator.EvaluateChangedRelationship(ruleSet, mutation.entityId, mutation.instanceId, mutation.relationshipId));
	}

	return stats;
}

enum class ForgetVisibilityState
{
	Visible,
	Hidden,
	Unknown
};

struct ForgetAttributeAction
{
	string entityId;
	string instanceId;
	string attributeId;
	string policyMode;
	shared_ptr<Attribute> attribute;
};

struct ForgetRelationshipAction
{
	string relationshipId;
	string policyMode;
	shared_ptr<Instance> parentInstance;
	shared_ptr<Instance> childInstance;
};

struct ForgetRemovedInstance
{
	string entityId;
	string instanceId;
};

struct ForgetPolicyPlan
{
	vector<ForgetAttributeAction> attributes;
	vector<ForgetRelationshipAction> relationships;
	int unknownVisibilityCount{0};
	int unresolvedBindingCount{0};
};

struct ForgetAttributeSnapshot
{
	shared_ptr<Attribute> target;
	Attribute value;
};

struct ForgetEntityInstancesSnapshot
{
	shared_ptr<Entity> entity;
	vector<shared_ptr<Instance>> instances;
};

struct ForgetInstanceLinksSnapshot
{
	shared_ptr<Instance> instance;
	vector<shared_ptr<Instance>> linkedInstances;
	vector<InstanceRelationshipLink> relationshipLinks;
};

struct ForgetPolicyRollbackState
{
	RuleSet* ruleSet{nullptr};
	bool finalized{false};
	unordered_map<string, json> retainedBefore;
	vector<ForgetAttributeSnapshot> attributeSnapshots;
	vector<ForgetEntityInstancesSnapshot> entitySnapshots;
	vector<ForgetInstanceLinksSnapshot> linkSnapshots;
	vector<ScreenLocation> screenLocationsBefore;
	unordered_map<string, string> inferredInstanceIdsBefore;
	unordered_map<string, RuleSet::ValidationEvent> validationEventsBefore;
	vector<RuleSet::DataConnectionRecordIdentitySnapshot> recordIdentitySnapshotsBefore;

	void Commit()
	{
		finalized = true;
	}

	void Rollback()
	{
		if(finalized || ruleSet == nullptr)
		{
			return;
		}

		finalized = true;
		for(ForgetAttributeSnapshot& snapshot : attributeSnapshots)
		{
			if(snapshot.target != nullptr)
			{
				*snapshot.target = std::move(snapshot.value);
			}
		}

		for(ForgetInstanceLinksSnapshot& snapshot : linkSnapshots)
		{
			if(snapshot.instance != nullptr)
			{
				snapshot.instance->linkedInstances = std::move(snapshot.linkedInstances);
				snapshot.instance->relationshipLinks = std::move(snapshot.relationshipLinks);
				snapshot.instance->InvalidateRelationshipLinkIndex();
			}
		}

		for(ForgetEntityInstancesSnapshot& snapshot : entitySnapshots)
		{
			if(snapshot.entity != nullptr)
			{
				snapshot.entity->instances = std::move(snapshot.instances);
				snapshot.entity->instanceIndexSize = static_cast<size_t>(-1);
			}
		}

		if(ruleSet->retainedForgetDataByKey != nullptr)
		{
			ruleSet->retainedForgetDataByKey->swap(retainedBefore);
		}
		if(ruleSet->screenLocations != nullptr)
		{
			*ruleSet->screenLocations = std::move(screenLocationsBefore);
		}
		if(ruleSet->inferredInstanceIdsByKey != nullptr)
		{
			*ruleSet->inferredInstanceIdsByKey = std::move(inferredInstanceIdsBefore);
		}
		if(ruleSet->activeValidationEventsByKey != nullptr)
		{
			*ruleSet->activeValidationEventsByKey = std::move(validationEventsBefore);
		}
		if(ruleSet->dataConnectionRecordIdentitySnapshots != nullptr)
		{
			*ruleSet->dataConnectionRecordIdentitySnapshots = std::move(recordIdentitySnapshotsBefore);
		}
	}

	~ForgetPolicyRollbackState()
	{
		try
		{
			Rollback();
		}
		catch(...)
		{
			// Destructors must not throw. All rollback storage is preallocated before mutation.
		}
	}
};

struct ForgetPolicyApplyResult
{
	bool succeeded{true};
	bool rolledBack{false};
	string error;
	shared_ptr<ForgetPolicyRollbackState> transaction;
	int forgottenAttributeCount{0};
	int forgottenInstanceCount{0};
	int retainedAttributeCount{0};
	int retainedSubtreeCount{0};
	vector<ForgetAttributeAction> changedAttributes;
	vector<RelationshipMutation> changedRelationships;
	vector<ForgetRemovedInstance> removedInstances;
};

bool IsForgetPolicyInputControl(const ScreenFile::Element& element)
{
	static const unordered_set<string> inputTypes = {
		"question",
		"textbox",
		"textarea",
		"character-count",
		"richtext",
		"checkbox",
		"radio",
		"dropdown",
		"date",
		"datetime",
		"time",
		"number",
		"currency",
		"email",
		"password",
		"file"
	};

	return inputTypes.find(lowercase_copy(element.controlType)) != inputTypes.end();
}

ForgetVisibilityState ElementForgetVisibility(const ScreenFile::Element& element, ForgetVisibilityState ancestorState)
{
	if(ancestorState != ForgetVisibilityState::Visible)
	{
		return ancestorState;
	}

	if(element.displayKnown.has_value())
	{
		return element.displayKnown.value() && element.display.value_or(true)
			? ForgetVisibilityState::Visible
			: (element.displayKnown.value() ? ForgetVisibilityState::Hidden : ForgetVisibilityState::Unknown);
	}

	if(element.visibility.has_value() || element.displayRuleId.has_value())
	{
		return ForgetVisibilityState::Unknown;
	}

	return element.display.value_or(true) ? ForgetVisibilityState::Visible : ForgetVisibilityState::Hidden;
}

shared_ptr<Instance> ResolveUniqueForgetBindingInstance(const RuleSet& ruleSet,
	                                                    const shared_ptr<Instance>& screenInstance,
	                                                    const string& targetEntityId)
{
	if(screenInstance == nullptr)
	{
		return nullptr;
	}

	if(targetEntityId.empty() || targetEntityId == screenInstance->entityId)
	{
		return screenInstance;
	}

	vector<shared_ptr<Instance>> queue{screenInstance};
	unordered_set<string> visited;
	vector<shared_ptr<Instance>> matches;

	for(size_t index = 0; index < queue.size(); ++index)
	{
		const shared_ptr<Instance>& current = queue[index];
		if(current == nullptr)
		{
			continue;
		}

		const string key = current->entityId + "\x1f" + current->instanceId;
		if(!visited.insert(key).second)
		{
			continue;
		}

		if(current->entityId == targetEntityId)
		{
			matches.push_back(current);
			continue;
		}

		for(const InstanceRelationshipLink& link : current->relationshipLinks)
		{
			shared_ptr<Instance> linked = link.linkedInstance.lock();
			if(linked == nullptr)
			{
				const string linkedEntityId = link.sourceInstanceId == current->instanceId ? link.targetEntityId : link.sourceEntityId;
				const string linkedInstanceId = link.sourceInstanceId == current->instanceId ? link.targetInstanceId : link.sourceInstanceId;
				DataManager dataManager;
				linked = dataManager.GetEntityInstanceById(ruleSet.entities, linkedEntityId, linkedInstanceId);
			}

			if(linked != nullptr)
			{
				queue.push_back(linked);
			}
		}
	}

	if(matches.size() == 1)
	{
		return matches.front();
	}

	DataManager dataManager;
	shared_ptr<Entity> targetEntity = dataManager.GetEntityByID(ruleSet.entities, targetEntityId);
	if(matches.empty() && targetEntity != nullptr && targetEntity->root.value_or(false) && targetEntity->instances.size() == 1)
	{
		return targetEntity->instances.front();
	}

	return nullptr;
}

struct ForgetBindingAggregate
{
	string entityId;
	string instanceId;
	string memberId;
	shared_ptr<Instance> instance;
	bool visible{false};
	bool hidden{false};
	bool unknown{false};
};

ForgetPolicyPlan BuildForgetPolicyPlan(const RuleSet& ruleSet,
	                                   const ScreenFile::Screen& screen,
	                                   const string& entityId,
	                                   const string& instanceId)
{
	ForgetPolicyPlan plan;
	DataManager dataManager;
	shared_ptr<Instance> screenInstance = dataManager.GetEntityInstanceById(ruleSet.entities, entityId, instanceId);
	if(screenInstance == nullptr)
	{
		plan.unresolvedBindingCount++;
		return plan;
	}

	unordered_map<string, ForgetBindingAggregate> attributeBindings;
	unordered_map<string, ForgetBindingAggregate> relationshipBindings;
	unordered_map<string, shared_ptr<Instance>> bindingInstanceByEntityId;
	const auto resolveBindingInstance = [&](const string& targetEntityId) {
		const string cacheKey = targetEntityId.empty() ? entityId : targetEntityId;
		auto cached = bindingInstanceByEntityId.find(cacheKey);
		if(cached != bindingInstanceByEntityId.end())
		{
			return cached->second;
		}

		shared_ptr<Instance> resolved = ResolveUniqueForgetBindingInstance(ruleSet, screenInstance, cacheKey);
		bindingInstanceByEntityId.emplace(cacheKey, resolved);
		return resolved;
	};

	const auto addVisibility = [](ForgetBindingAggregate& binding, ForgetVisibilityState visibility) {
		binding.visible = binding.visible || visibility == ForgetVisibilityState::Visible;
		binding.hidden = binding.hidden || visibility == ForgetVisibilityState::Hidden;
		binding.unknown = binding.unknown || visibility == ForgetVisibilityState::Unknown;
	};

	const auto inspectElement = [&](const auto& self, const ScreenFile::Element& element, ForgetVisibilityState ancestorState) -> void {
		const ForgetVisibilityState visibility = ElementForgetVisibility(element, ancestorState);

		if(IsForgetPolicyInputControl(element))
		{
			const string attributeId = element.boundAttributeId.value_or(element.id.value_or(""));
			const string bindingEntityId = element.boundEntityId.value_or(entityId);
			if(!attributeId.empty())
			{
				shared_ptr<Instance> bindingInstance = resolveBindingInstance(bindingEntityId);
				if(bindingInstance == nullptr)
				{
					plan.unresolvedBindingCount++;
				}
				else
				{
					const string key = RuleSet::AttributeDependencyKey(bindingInstance->entityId, attributeId) + "\x1f" + bindingInstance->instanceId;
					ForgetBindingAggregate& binding = attributeBindings[key];
					binding.entityId = bindingInstance->entityId;
					binding.instanceId = bindingInstance->instanceId;
					binding.memberId = attributeId;
					binding.instance = bindingInstance;
					addVisibility(binding, visibility);
				}
			}
		}

		if(element.collectRelationshipId.has_value() && !element.collectRelationshipId->empty())
		{
			shared_ptr<Relationship> relationship = FindRuntimeRelationshipById(ruleSet, element.collectRelationshipId.value());
			shared_ptr<Instance> parentInstance = relationship != nullptr
				? resolveBindingInstance(relationship->sourceEntity)
				: nullptr;

			if(parentInstance == nullptr)
			{
				plan.unresolvedBindingCount++;
			}
			else
			{
				const string key = element.collectRelationshipId.value() + "\x1f" + parentInstance->instanceId;
				ForgetBindingAggregate& binding = relationshipBindings[key];
				binding.entityId = parentInstance->entityId;
				binding.instanceId = parentInstance->instanceId;
				binding.memberId = element.collectRelationshipId.value();
				binding.instance = parentInstance;
				addVisibility(binding, visibility);
			}
		}

		for(const ScreenFile::Element& child : element.children)
		{
			self(self, child, visibility);
		}
	};

	for(const ScreenFile::Element& element : screen.elements)
	{
		inspectElement(inspectElement, element, ForgetVisibilityState::Visible);
	}

	for(const auto& entry : attributeBindings)
	{
		const ForgetBindingAggregate& binding = entry.second;
		if(binding.visible || binding.unknown || !binding.hidden)
		{
			if(binding.unknown)
			{
				plan.unknownVisibilityCount++;
			}
			continue;
		}

		shared_ptr<Attribute> attribute = binding.instance != nullptr ? binding.instance->GetAttributeByID(binding.memberId) : nullptr;
		if(attribute == nullptr || attribute->IsUnknown() || attribute->IsInferred())
		{
			continue;
		}

		plan.attributes.push_back(ForgetAttributeAction{
			binding.entityId,
			binding.instanceId,
			binding.memberId,
			ruleSet.GetAttributeForgetPolicyMode(binding.entityId, binding.memberId),
			attribute
		});
	}

	unordered_set<string> relationshipActionKeys;
	for(const auto& entry : relationshipBindings)
	{
		const ForgetBindingAggregate& binding = entry.second;
		if(binding.visible || binding.unknown || !binding.hidden)
		{
			if(binding.unknown)
			{
				plan.unknownVisibilityCount++;
			}
			continue;
		}

		for(const InstanceRelationshipLink& link : binding.instance->relationshipLinks)
		{
			if(!link.outgoing || link.relationshipId != binding.memberId)
			{
				continue;
			}

			shared_ptr<Instance> childInstance = link.linkedInstance.lock();
			if(childInstance == nullptr)
			{
				childInstance = dataManager.GetEntityInstanceById(ruleSet.entities, link.targetEntityId, link.targetInstanceId);
			}

			const string actionKey = binding.memberId + "\x1f" + link.targetInstanceId;
			if(childInstance != nullptr && relationshipActionKeys.insert(actionKey).second)
			{
				plan.relationships.push_back(ForgetRelationshipAction{
					binding.memberId,
					ruleSet.GetRelationshipForgetPolicyMode(binding.memberId),
					binding.instance,
					childInstance
				});
			}
		}
	}

	return plan;
}

json BuildRetainedRelationshipSnapshot(const ForgetRelationshipAction& action)
{
	json instances = json::array();
	vector<shared_ptr<Instance>> queue{action.childInstance};
	unordered_set<string> visited;

	for(size_t index = 0; index < queue.size(); ++index)
	{
		const shared_ptr<Instance>& instance = queue[index];
		if(instance == nullptr)
		{
			continue;
		}

		const string key = instance->entityId + "\x1f" + instance->instanceId;
		if(!visited.insert(key).second)
		{
			continue;
		}

		instances.push_back(instance->CreateJSON());
		for(const InstanceRelationshipLink& link : instance->relationshipLinks)
		{
			if(link.outgoing)
			{
				queue.push_back(link.linkedInstance.lock());
			}
		}
	}

	return json{
		{"kind", "relationshipSubtree"},
		{"relationshipId", action.relationshipId},
		{"sourceEntityId", action.parentInstance != nullptr ? action.parentInstance->entityId : string()},
		{"sourceInstanceId", action.parentInstance != nullptr ? action.parentInstance->instanceId : string()},
		{"targetEntityId", action.childInstance != nullptr ? action.childInstance->entityId : string()},
		{"targetInstanceId", action.childInstance != nullptr ? action.childInstance->instanceId : string()},
		{"instances", instances}
	};
}

ForgetPolicyApplyResult ApplyForgetPolicyPlan(RuleSet& ruleSet, const ForgetPolicyPlan& plan)
{
	ForgetPolicyApplyResult result;
	result.changedAttributes.reserve(plan.attributes.size());
	result.changedRelationships.reserve(plan.relationships.size() * 2);
	if(plan.attributes.empty() && plan.relationships.empty())
	{
		return result;
	}

	if(ruleSet.retainedForgetDataByKey == nullptr)
	{
		ruleSet.retainedForgetDataByKey = make_shared<unordered_map<string, json>>();
	}

	shared_ptr<ForgetPolicyRollbackState> transaction = make_shared<ForgetPolicyRollbackState>();
	transaction->ruleSet = &ruleSet;
	transaction->retainedBefore = *ruleSet.retainedForgetDataByKey;
	if(ruleSet.screenLocations != nullptr)
	{
		transaction->screenLocationsBefore = *ruleSet.screenLocations;
	}
	if(ruleSet.inferredInstanceIdsByKey != nullptr)
	{
		transaction->inferredInstanceIdsBefore = *ruleSet.inferredInstanceIdsByKey;
	}
	if(ruleSet.activeValidationEventsByKey != nullptr)
	{
		transaction->validationEventsBefore = *ruleSet.activeValidationEventsByKey;
	}
	if(ruleSet.dataConnectionRecordIdentitySnapshots != nullptr)
	{
		transaction->recordIdentitySnapshotsBefore = *ruleSet.dataConnectionRecordIdentitySnapshots;
	}

	if(ruleSet.entities != nullptr)
	{
		for(const shared_ptr<Entity>& entity : *ruleSet.entities)
		{
			if(entity == nullptr)
			{
				continue;
			}

			transaction->entitySnapshots.push_back(ForgetEntityInstancesSnapshot{entity, entity->instances});
			for(const shared_ptr<Instance>& instance : entity->instances)
			{
				if(instance == nullptr)
				{
					continue;
				}

				transaction->linkSnapshots.push_back(ForgetInstanceLinksSnapshot{instance, instance->linkedInstances, instance->relationshipLinks});
				for(const shared_ptr<Attribute>& attribute : instance->attributes)
				{
					if(attribute != nullptr)
					{
						transaction->attributeSnapshots.push_back(ForgetAttributeSnapshot{attribute, *attribute});
					}
				}
			}
		}
	}

	unordered_map<string, json> retainedAfter = transaction->retainedBefore;

	// Stage all retained copies before touching the active graph.
	for(const ForgetAttributeAction& action : plan.attributes)
	{
		if(action.policyMode == "Retain" && action.attribute != nullptr)
		{
			const string key = "attribute\x1f" + action.entityId + "\x1f" + action.instanceId + "\x1f" + action.attributeId;
			retainedAfter[key] = json{
				{"kind", "attribute"},
				{"entityId", action.entityId},
				{"instanceId", action.instanceId},
				{"attributeId", action.attributeId},
				{"attribute", action.attribute->CreateJSON()}
			};
			result.retainedAttributeCount++;
		}
	}

	for(const ForgetRelationshipAction& action : plan.relationships)
	{
		if(action.policyMode == "Retain" && action.childInstance != nullptr)
		{
			const string key = "relationship\x1f" + action.relationshipId + "\x1f"
				+ (action.parentInstance != nullptr ? action.parentInstance->instanceId : string()) + "\x1f"
				+ action.childInstance->instanceId;
			retainedAfter[key] = BuildRetainedRelationshipSnapshot(action);
			result.retainedSubtreeCount++;
		}
	}

	DataManager dataManager;
	unordered_map<string, shared_ptr<Instance>> deletedInstances;
	vector<shared_ptr<Instance>> deletionQueue;
	for(const ForgetRelationshipAction& action : plan.relationships)
	{
		if(action.childInstance != nullptr)
		{
			deletionQueue.push_back(action.childInstance);
		}
	}

	for(size_t index = 0; index < deletionQueue.size(); ++index)
	{
		const shared_ptr<Instance>& instance = deletionQueue[index];
		if(instance == nullptr)
		{
			continue;
		}

		const string instanceKey = instance->entityId + "\x1f" + instance->instanceId;
		if(!deletedInstances.emplace(instanceKey, instance).second)
		{
			continue;
		}

		for(const InstanceRelationshipLink& link : instance->relationshipLinks)
		{
			if(!link.outgoing)
			{
				continue;
			}

			shared_ptr<Instance> child = link.linkedInstance.lock();
			if(child == nullptr)
			{
				child = dataManager.GetEntityInstanceById(ruleSet.entities, link.targetEntityId, link.targetInstanceId);
			}

			if(child != nullptr)
			{
				deletionQueue.push_back(child);
			}
		}
	}

	unordered_set<string> relationshipMutationKeys;
	for(const ForgetRelationshipAction& action : plan.relationships)
	{
		if(action.childInstance == nullptr)
		{
			continue;
		}

		vector<RelationshipMutation> instanceMutations = CollectRelationshipMutationsForInstance(
			ruleSet,
			action.childInstance->entityId,
			action.childInstance->instanceId);
		for(const RelationshipMutation& mutation : instanceMutations)
		{
			AddRelationshipMutation(result.changedRelationships, relationshipMutationKeys, mutation.entityId, mutation.instanceId, mutation.relationshipId);
		}
		AddCollectRelationshipMutation(result.changedRelationships,
		                              relationshipMutationKeys,
		                              ruleSet,
		                              action.relationshipId,
		                              action.parentInstance,
		                              action.childInstance);

	}

	try
	{
		ruleSet.retainedForgetDataByKey->swap(retainedAfter);

		for(const ForgetAttributeAction& action : plan.attributes)
		{
			if(action.attribute == nullptr
				|| deletedInstances.find(action.entityId + "\x1f" + action.instanceId) != deletedInstances.end())
			{
				continue;
			}

			action.attribute->MakeUnknown(false);
			result.changedAttributes.push_back(action);
			result.forgottenAttributeCount++;
		}

		for(ForgetInstanceLinksSnapshot& snapshot : transaction->linkSnapshots)
		{
			shared_ptr<Instance>& instance = snapshot.instance;
			if(instance == nullptr)
			{
				continue;
			}

			instance->linkedInstances.erase(
				remove_if(instance->linkedInstances.begin(), instance->linkedInstances.end(), [&](const shared_ptr<Instance>& linked) {
					return linked != nullptr && deletedInstances.find(linked->entityId + "\x1f" + linked->instanceId) != deletedInstances.end();
				}),
				instance->linkedInstances.end());
			instance->relationshipLinks.erase(
				remove_if(instance->relationshipLinks.begin(), instance->relationshipLinks.end(), [&](const InstanceRelationshipLink& link) {
					return deletedInstances.find(link.sourceEntityId + "\x1f" + link.sourceInstanceId) != deletedInstances.end()
						|| deletedInstances.find(link.targetEntityId + "\x1f" + link.targetInstanceId) != deletedInstances.end();
				}),
				instance->relationshipLinks.end());
			instance->InvalidateRelationshipLinkIndex();
		}

		for(ForgetEntityInstancesSnapshot& snapshot : transaction->entitySnapshots)
		{
			shared_ptr<Entity>& entity = snapshot.entity;
			entity->instances.erase(
				remove_if(entity->instances.begin(), entity->instances.end(), [&](const shared_ptr<Instance>& instance) {
					return instance != nullptr && deletedInstances.find(instance->entityId + "\x1f" + instance->instanceId) != deletedInstances.end();
				}),
				entity->instances.end());
			entity->instanceIndexSize = static_cast<size_t>(-1);
		}

		result.forgottenInstanceCount = static_cast<int>(deletedInstances.size());
		vector<string> removedInstanceKeys;
		removedInstanceKeys.reserve(deletedInstances.size());
		for(const auto& entry : deletedInstances)
		{
			removedInstanceKeys.push_back(entry.first);
		}
		sort(removedInstanceKeys.begin(), removedInstanceKeys.end());
		for(const string& key : removedInstanceKeys)
		{
			const shared_ptr<Instance>& instance = deletedInstances.at(key);
			result.removedInstances.push_back(ForgetRemovedInstance{instance->entityId, instance->instanceId});
		}
		result.transaction = transaction;
	}
	catch(const std::exception& ex)
	{
		transaction->Rollback();
		result = ForgetPolicyApplyResult();
		result.succeeded = false;
		result.error = ex.what();
	}

	return result;
}

json ForgetPolicyResultToJson(const ForgetPolicyPlan& plan, const ForgetPolicyApplyResult& result, bool applied, const string& mode)
{
	json removedAttributes = json::array();
	for(const ForgetAttributeAction& action : result.changedAttributes)
	{
		removedAttributes.push_back({
			{"entityId", action.entityId},
			{"instanceId", action.instanceId},
			{"attributeId", action.attributeId}
		});
	}

	json removedInstances = json::array();
	for(const ForgetRemovedInstance& instance : result.removedInstances)
	{
		removedInstances.push_back({
			{"entityId", instance.entityId},
			{"instanceId", instance.instanceId}
		});
	}

	return json{
		{"succeeded", result.succeeded},
		{"applied", applied},
		{"rolledBack", result.rolledBack},
		{"mode", mode},
		{"forgottenAttributeCount", result.forgottenAttributeCount},
		{"forgottenInstanceCount", result.forgottenInstanceCount},
		{"retainedAttributeCount", result.retainedAttributeCount},
		{"retainedSubtreeCount", result.retainedSubtreeCount},
		{"unknownVisibilityCount", plan.unknownVisibilityCount},
		{"unresolvedBindingCount", plan.unresolvedBindingCount},
		{"removedAttributes", removedAttributes},
		{"removedInstances", removedInstances}
	};
}

SessionModelRuleEvaluator::EvaluationStats EvaluateInitialRelationshipRules(RuleSet& ruleSet)
{
	SessionModelRuleEvaluator::EvaluationStats stats;

	if(ruleSet.entities == nullptr || ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	SessionModelRuleEvaluator evaluator;

	for(const shared_ptr<Entity>& entity : *ruleSet.entities)
	{
		if(entity == nullptr)
		{
			continue;
		}

		for(const shared_ptr<Relationship>& relationship : entity->relationships)
		{
			if(relationship == nullptr || relationship->id.empty())
			{
				continue;
			}

			for(const shared_ptr<Instance>& instance : entity->instances)
			{
				if(instance != nullptr)
				{
					MergeEvaluationStats(stats, evaluator.EvaluateChangedRelationship(ruleSet, entity->entityid, instance->instanceId, relationship->id));
				}
			}
		}
	}

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats EvaluateSessionContextRules(RuleSet& ruleSet)
{
	SessionModelRuleEvaluator::EvaluationStats stats;

	if(ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	SessionModelRuleEvaluator evaluator;
	MergeEvaluationStats(stats, evaluator.EvaluateChangedSessionContext(ruleSet, {
		"CurrentDate",
		"CurrentDateTime",
		"CurrentTime",
		"CurrentLocale",
		"DeploymentName",
		"DeploymentVersion"
	}));

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats EvaluateScreenLoadRules(RuleSet& ruleSet)
{
	SessionModelRuleEvaluator::EvaluationStats stats;

	if(ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	MergeEvaluationStats(stats, EvaluateSessionContextRules(ruleSet));

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats EvaluateFullSessionInitializationRules(RuleSet& ruleSet, bool captureTrace = false)
{
	SessionModelRuleEvaluator::EvaluationStats stats;

	if(ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	SessionModelRuleEvaluator evaluator;
	MergeEvaluationStats(stats, evaluator.EvaluateAllRules(ruleSet, captureTrace));
	MergeEvaluationStats(stats, EvaluateSessionContextRules(ruleSet));

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats EvaluateInitialSessionModelRules(RuleSet& ruleSet, bool captureTrace = false)
{
	SessionModelRuleEvaluator::EvaluationStats stats;

	if(ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	MergeEvaluationStats(stats, EvaluateFullSessionInitializationRules(ruleSet, captureTrace));
	MergeEvaluationStats(stats, EvaluateInitialRelationshipRules(ruleSet));

	return stats;
}

json DebugEngineJson()
{
	return {
		{"name", "SessionManagerRuntime"},
		{"rulesEngine", "SessionModelRuleEvaluator"},
		{"buildConfiguration", build_configuration_name()},
		{"debugProtocolVersion", 1}
	};
}

int DebugSessionTtlSeconds()
{
	return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::minutes(30)).count());
}

void WriteDebugError(httplib::Response& res, int status, const std::string& code, const std::string& message)
{
	res.status = status;
	res.set_content(make_error(code, message).dump(2), "application/json");
}

bool TryGetDebugSessionModelJson(const json& requestJson, json& sessionModelJson, std::string& errorCode, std::string& errorMessage)
{
	const json* modelValue = FindObjectValue(requestJson, {"sessionModel", "model", "sessionModelJson", "modelJson"});

	if(modelValue == nullptr || modelValue->is_null())
	{
		errorCode = "missing_debug_session_model";
		errorMessage = "Debug session start requests require sessionModel or sessionModelJson.";
		return false;
	}

	if(modelValue->is_object())
	{
		sessionModelJson = *modelValue;
		return true;
	}

	if(modelValue->is_string())
	{
		try
		{
			sessionModelJson = json::parse(modelValue->get<std::string>());
			return true;
		}
		catch(const std::exception& e)
		{
			errorCode = "bad_debug_session_model_json";
			errorMessage = std::string("Debug session model JSON could not be parsed: ") + e.what();
			return false;
		}
	}

	errorCode = "invalid_debug_session_model";
	errorMessage = "Debug session model must be a JSON object or a JSON string.";
	return false;
}

const json* FindDebugArrayValue(const json& requestJson, std::initializer_list<const char*> keys, std::string& errorCode, std::string& errorMessage)
{
	const json* value = FindObjectValue(requestJson, keys);

	if(value == nullptr || value->is_null())
	{
		return nullptr;
	}

	if(!value->is_array())
	{
		errorCode = "invalid_debug_array";
		errorMessage = "Debug request array fields must be JSON arrays.";
		return nullptr;
	}

	return value;
}

json DebugRuntimeCountsJson(const RuleSet& ruleSet)
{
	std::size_t entityCount = 0;
	std::size_t attributeCount = 0;
	std::size_t instanceCount = 0;
	std::size_t relationshipCount = 0;

	if(ruleSet.entities != nullptr)
	{
		for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
		{
			if(entity == nullptr)
			{
				continue;
			}

			++entityCount;
			attributeCount += entity->attributes.size();
			instanceCount += entity->instances.size();
			relationshipCount += entity->relationships.size();
		}
	}

	return {
		{"entityCount", entityCount},
		{"attributeCount", attributeCount},
		{"instanceCount", instanceCount},
		{"relationshipCount", relationshipCount},
		{"ruleCount", ruleSet.sessionModelRules != nullptr ? ruleSet.sessionModelRules->size() : 0}
	};
}

json DebugEntityInstancesJson(const RuleSet& ruleSet)
{
	json result = json::array();

	if(ruleSet.entities == nullptr)
	{
		return result;
	}

	for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
	{
		if(entity == nullptr)
		{
			continue;
		}

		for(const std::shared_ptr<Instance>& instance : entity->instances)
		{
			if(instance == nullptr)
			{
				continue;
			}

			result.push_back({
				{"entityId", entity->entityid},
				{"instanceId", instance->instanceId},
				{"isRoot", instance->isRoot},
				{"isInferred", instance->isInferred}
			});
		}
	}

	return result;
}

std::string DebugSingleInstanceId(const RuleSet& ruleSet,
                                  const std::string& entityId,
                                  bool& ambiguous)
{
	ambiguous = false;
	std::shared_ptr<Entity> entity = find_ruleset_entity(ruleSet, entityId);

	if(entity == nullptr)
	{
		return "";
	}

	std::string instanceId;

	for(const std::shared_ptr<Instance>& instance : entity->instances)
	{
		if(instance != nullptr && !instance->instanceId.empty())
		{
			if(!instanceId.empty())
			{
				ambiguous = true;
				return "";
			}

			instanceId = instance->instanceId;
		}
	}

	return instanceId;
}

std::shared_ptr<Instance> DebugFindInstance(RuleSet& ruleSet,
                                            const std::string& entityId,
                                            const std::string& requestedInstanceId,
                                            std::string& resolvedInstanceId,
                                            bool& ambiguous)
{
	ambiguous = false;
	resolvedInstanceId = requestedInstanceId.empty()
		? DebugSingleInstanceId(ruleSet, entityId, ambiguous)
		: requestedInstanceId;

	if(ambiguous || resolvedInstanceId.empty())
	{
		return nullptr;
	}

	DataManager dataManager;
	return dataManager.GetEntityInstanceById(ruleSet.entities, entityId, resolvedInstanceId);
}

std::shared_ptr<Relationship> DebugFindRelationshipBetween(const RuleSet& ruleSet,
                                                           const std::string& sourceEntityId,
                                                           const std::string& targetEntityId,
                                                           const std::string& requestedRelationshipId,
                                                           bool& ambiguous)
{
	ambiguous = false;

	if(!requestedRelationshipId.empty())
	{
		std::shared_ptr<Relationship> relationship = find_ruleset_relationship(ruleSet, requestedRelationshipId);
		return relationship != nullptr &&
			((relationship->sourceEntity == sourceEntityId && relationship->targetEntity == targetEntityId) ||
			 (relationship->sourceEntity == targetEntityId && relationship->targetEntity == sourceEntityId))
			? relationship
			: nullptr;
	}

	std::shared_ptr<Entity> sourceEntity = find_ruleset_entity(ruleSet, sourceEntityId);

	if(sourceEntity == nullptr)
	{
		return nullptr;
	}

	std::shared_ptr<Relationship> result;

	for(const std::shared_ptr<Relationship>& relationship : sourceEntity->relationships)
	{
		if(relationship != nullptr && relationship->sourceEntity == sourceEntityId && relationship->targetEntity == targetEntityId)
		{
			if(result != nullptr)
			{
				ambiguous = true;
				return nullptr;
			}

			result = relationship;
		}
	}

	return result;
}

json DebugAttributeStateJson(const std::string& entityId,
                             const std::string& instanceId,
                             const std::shared_ptr<Attribute>& attribute)
{
	if(attribute == nullptr)
	{
		return nullptr;
	}

	json state = {
		{"entityId", entityId},
		{"instanceId", instanceId},
		{"attributeId", attribute->GetID()},
		{"knowledgeState", Attribute::KnowledgeStateToString(attribute->GetKnowledgeState())},
		{"known", attribute->IsKnown()},
		{"certain", attribute->IsCertain()},
		{"uncertain", attribute->IsUncertain()},
		{"inferred", attribute->IsInferred()}
	};

	if(attribute->IsUnknown())
	{
		state["value"] = nullptr;
	}
	else
	{
		state["value"] = attribute->GetStringValue();
	}

	return state;
}

bool ApplyDebugEntityInstanceDefinitions(RuleSet& ruleSet,
                                         const json& entityInstances,
                                         int& createdCount,
                                         std::string& errorCode,
                                         std::string& errorMessage)
{
	createdCount = 0;

	if(!entityInstances.is_array())
	{
		return true;
	}

	DataManager dataManager;

	for(const json& item : entityInstances)
	{
		if(!item.is_object())
		{
			errorCode = "invalid_debug_entity_instance";
			errorMessage = "Debug entity instance definitions must be JSON objects.";
			return false;
		}

		const std::string entityId = first_json_scalar_string(item, {"entityId", "entityid"});
		std::string instanceId = first_json_scalar_string(item, {"instanceId", "instanceid"});

		if(entityId.empty())
		{
			errorCode = "missing_debug_entity_id";
			errorMessage = "Debug entity instance definitions require entityId.";
			return false;
		}

		std::shared_ptr<Entity> entity = dataManager.GetEntityByID(ruleSet.entities, entityId);

		if(entity == nullptr)
		{
			errorCode = "debug_entity_not_found";
			errorMessage = "Debug entity instance definition references an unknown entity: " + entityId;
			return false;
		}

		if(!instanceId.empty() && entity->GetInstanceById(instanceId) != nullptr)
		{
			continue;
		}

		const std::string parentEntityId = first_json_scalar_string(item, {"parentEntityId", "parententityid"});
		const std::string parentInstanceId = first_json_scalar_string(item, {"parentInstanceId", "parentinstanceid"});
		std::shared_ptr<Instance> created;

		if(!parentEntityId.empty() || !parentInstanceId.empty())
		{
			if(parentEntityId.empty())
			{
				errorCode = "missing_debug_parent_entity_id";
				errorMessage = "Debug entity instance definitions with parentInstanceId also require parentEntityId.";
				return false;
			}

			std::string resolvedParentInstanceId;
			bool parentInstanceAmbiguous = false;
			std::shared_ptr<Instance> parentInstance = DebugFindInstance(
				ruleSet,
				parentEntityId,
				parentInstanceId,
				resolvedParentInstanceId,
				parentInstanceAmbiguous);

			if(parentInstance == nullptr)
			{
				errorCode = parentInstanceAmbiguous
					? "debug_parent_instance_ambiguous"
					: "debug_parent_instance_not_found";
				errorMessage = parentInstanceAmbiguous
					? "Debug entity instance definition must provide parentInstanceId when the parent entity has multiple instances."
					: "Debug entity instance definition references an unknown parent instance.";
				return false;
			}

			const std::string relationshipId = first_json_scalar_string(item, {"relationshipId", "relationshipid"});
			bool relationshipAmbiguous = false;
			std::shared_ptr<Relationship> relationship = DebugFindRelationshipBetween(
				ruleSet,
				parentInstance->entityId,
				entity->entityid,
				relationshipId,
				relationshipAmbiguous);

			if(relationship == nullptr)
			{
				errorCode = relationshipAmbiguous
					? "debug_relationship_ambiguous"
					: "debug_relationship_not_found";
				errorMessage = relationshipAmbiguous
					? "Debug entity instance definition must provide relationshipId when multiple relationships connect the parent and child entities."
					: "Debug entity instance definition references an unknown or incompatible relationship.";
				return false;
			}

			created = entity->GetCleanInstance();

			if(created != nullptr)
			{
				created->entityId = entity->entityid;

				if(!instanceId.empty())
				{
					created->SetInstanceId(instanceId);
				}

				entity->instances.push_back(created);

				parentInstance->AddLinkedInstance(created);
				created->AddLinkedInstance(parentInstance);

				if(relationship != nullptr)
				{
					if(relationship->sourceEntity == parentInstance->entityId && relationship->targetEntity == created->entityId)
					{
						parentInstance->AddRelationshipLink(created, relationship, true);
						created->AddRelationshipLink(parentInstance, relationship, false);
					}
					else if(relationship->sourceEntity == created->entityId && relationship->targetEntity == parentInstance->entityId)
					{
						created->AddRelationshipLink(parentInstance, relationship, true);
						parentInstance->AddRelationshipLink(created, relationship, false);
					}
				}

				created->EvaluateFunctions();
				parentInstance->EvaluateFunctions();
			}
		}
		else
		{
			created = entity->GetCleanInstance();

			if(created != nullptr)
			{
				created->entityId = entity->entityid;
				created->isRoot = entity->root.value_or(false);
				entity->instances.push_back(created);
				created->EvaluateFunctions();
			}
		}

		if(created == nullptr)
		{
			errorCode = "debug_instance_create_failed";
			errorMessage = "Debug entity instance could not be created for entity: " + entityId;
			return false;
		}

		if(instanceId.empty())
		{
			instanceId = created->instanceId;
		}
		else
		{
			created->SetInstanceId(instanceId);
		}

		entity->instanceIndexSize = static_cast<size_t>(-1);
		++createdCount;
	}

	return true;
}

bool ApplyDebugFacts(RuleSet& ruleSet,
                     const json& facts,
                     int& changedFactCount,
                     json& changedFacts,
                     SessionModelRuleEvaluator::EvaluationStats& evaluationStats,
                     std::string& errorCode,
                     std::string& errorMessage)
{
	changedFactCount = 0;
	changedFacts = json::array();

	if(!facts.is_array())
	{
		return true;
	}

	SessionModelRuleEvaluator evaluator;
	struct AppliedDebugFact
	{
		std::string entityId;
		std::string instanceId;
		std::shared_ptr<Attribute> attribute;
		std::string before;
	};
	std::vector<AppliedDebugFact> appliedFacts;
	std::vector<SessionModelRuleEvaluator::ChangedAttribute> changedAttributes;

	for(const json& fact : facts)
	{
		if(!fact.is_object())
		{
			errorCode = "invalid_debug_fact";
			errorMessage = "Debug facts must be JSON objects.";
			return false;
		}

		const std::string entityId = first_json_scalar_string(fact, {"entityId", "entityid"});
		const std::string requestedInstanceId = first_json_scalar_string(fact, {"instanceId", "instanceid"});
		const std::string attributeId = first_json_scalar_string(fact, {"attributeId", "attributeid", "id"});

		if(entityId.empty() || attributeId.empty())
		{
			errorCode = "missing_debug_fact_context";
			errorMessage = "Debug facts require entityId and attributeId.";
			return false;
		}

		std::string instanceId;
		bool instanceAmbiguous = false;
		std::shared_ptr<Instance> instance = DebugFindInstance(
			ruleSet,
			entityId,
			requestedInstanceId,
			instanceId,
			instanceAmbiguous);

		if(instance == nullptr)
		{
			errorCode = instanceAmbiguous
				? "debug_fact_instance_ambiguous"
				: "debug_instance_not_found";
			errorMessage = instanceAmbiguous
				? "Debug facts must provide instanceId when the target entity has multiple instances."
				: "Debug fact target instance not found.";
			return false;
		}

		std::shared_ptr<Attribute> attribute = find_instance_attribute(instance, attributeId);

		if(attribute == nullptr)
		{
			errorCode = "debug_attribute_not_found";
			errorMessage = "Debug fact target attribute not found: " + attributeId;
			return false;
		}

		const json* rawValue = FindObjectValue(fact, {"value", "Value"});
		const std::string knowledgeStateText = first_json_scalar_string(fact, {"knowledgeState", "knownness"});
		const Attribute::KnowledgeState knowledgeState = knowledgeStateText.empty()
			? Attribute::KnowledgeState::Certain
			: parse_history_knownness(knowledgeStateText);

		if(rawValue == nullptr && knowledgeState != Attribute::KnowledgeState::Unknown)
		{
			errorCode = "missing_debug_fact_value";
			errorMessage = "Debug facts require value unless knowledgeState is Unknown.";
			return false;
		}

		const std::string before = AttributeStateSignature(attribute);
		bool applied = true;

		if(rawValue == nullptr || rawValue->is_null() || knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			attribute->MakeUnknown(false);
		}
		else
		{
			applied = attribute->SetValueFromString(SubmitValueToString(*rawValue), false);

			if(applied && knowledgeState != Attribute::KnowledgeState::Certain)
			{
				attribute->SetKnowledgeState(knowledgeState, false);
			}
		}

		if(!applied)
		{
			errorCode = "invalid_debug_fact_value";
			errorMessage = "Debug fact value could not be converted for attribute: " + attributeId;
			return false;
		}

		appliedFacts.push_back(AppliedDebugFact{entityId, instanceId, attribute, before});
		changedAttributes.push_back(SessionModelRuleEvaluator::ChangedAttribute{entityId, instanceId, attributeId});
		++changedFactCount;
	}

	if(ruleSet.sessionModelRules != nullptr && !ruleSet.sessionModelRules->empty() && !changedAttributes.empty())
	{
		MergeEvaluationStats(
			evaluationStats,
			evaluator.EvaluateChangedAttributes(ruleSet, changedAttributes, true));
	}

	for(const AppliedDebugFact& fact : appliedFacts)
	{
		const std::string after = AttributeStateSignature(fact.attribute);
		json factResult = DebugAttributeStateJson(fact.entityId, fact.instanceId, fact.attribute);
		factResult["changed"] = fact.before != after;
		changedFacts.push_back(factResult);
	}

	return true;
}

json DebugTargetStateJson(RuleSet& ruleSet, const json& target, std::string& errorCode, std::string& errorMessage)
{
	if(!target.is_object())
	{
		return nullptr;
	}

	const std::string entityId = first_json_scalar_string(target, {"entityId", "entityid"});
	const std::string requestedInstanceId = first_json_scalar_string(target, {"instanceId", "instanceid"});
	const std::string attributeId = first_json_scalar_string(target, {"attributeId", "attributeid", "id"});

	if(entityId.empty() || attributeId.empty())
	{
		errorCode = "missing_debug_target_context";
		errorMessage = "Debug targets require entityId and attributeId.";
		return nullptr;
	}

	std::string instanceId;
	bool instanceAmbiguous = false;
	std::shared_ptr<Instance> instance = DebugFindInstance(
		ruleSet,
		entityId,
		requestedInstanceId,
		instanceId,
		instanceAmbiguous);

	if(instance == nullptr)
	{
		errorCode = instanceAmbiguous
		? "debug_target_instance_ambiguous"
		: "debug_target_instance_not_found";
		errorMessage = instanceAmbiguous
		? "Debug targets must provide instanceId when the target entity has multiple instances."
		: "Debug target instance not found.";
		return nullptr;
	}

	std::shared_ptr<Attribute> attribute = find_instance_attribute(instance, attributeId);

	if(attribute == nullptr)
	{
		errorCode = "debug_target_attribute_not_found";
		errorMessage = "Debug target attribute not found: " + attributeId;
		return nullptr;
	}

	return DebugAttributeStateJson(entityId, instanceId, attribute);
}

bool BuildDebugObservedFactsJson(RuleSet& ruleSet,
                                 const json& observations,
                                 json& observedFacts,
                                 std::string& errorCode,
                                 std::string& errorMessage)
{
	constexpr size_t maximumObservationCount = 2048;
	observedFacts = json::array();

	if(!observations.is_array())
	{
		return true;
	}

	if(observations.size() > maximumObservationCount)
	{
		errorCode = "too_many_debug_observations";
		errorMessage = "Debug evaluation accepts at most 2048 inferred observations.";
		return false;
	}

	std::unordered_set<std::string> observedKeys;
	for(const json& observation : observations)
	{
		if(!observation.is_object())
		{
			errorCode = "invalid_debug_observation";
			errorMessage = "Debug observations must be JSON objects.";
			return false;
		}

		if(FindObjectValue(observation, {"value", "Value"}) != nullptr)
		{
			errorCode = "debug_observation_cannot_assert_value";
			errorMessage = "Debug observations identify inferred values and cannot assert a value.";
			return false;
		}

		const std::string entityId = first_json_scalar_string(observation, {"entityId", "entityid"});
		const std::string requestedInstanceId = first_json_scalar_string(observation, {"instanceId", "instanceid"});
		const std::string attributeId = first_json_scalar_string(observation, {"attributeId", "attributeid", "id"});
		if(entityId.empty() || attributeId.empty())
		{
			errorCode = "missing_debug_observation_context";
			errorMessage = "Debug observations require entityId and attributeId.";
			return false;
		}

		const std::string observationKey = entityId + "\n" + requestedInstanceId + "\n" + attributeId;
		if(!observedKeys.insert(observationKey).second)
		{
			continue;
		}

		std::string observationErrorCode;
		std::string observationErrorMessage;
		json observed = DebugTargetStateJson(
			ruleSet,
			observation,
			observationErrorCode,
			observationErrorMessage);
		if(!observationErrorCode.empty() || observed.is_null())
		{
			observed = {
				{"entityId", entityId},
				{"instanceId", requestedInstanceId},
				{"attributeId", attributeId},
				{"knowledgeState", "Unknown"},
				{"known", false},
				{"certain", false},
				{"value", nullptr},
				{"errorCode", observationErrorCode.empty() ? "debug_observation_not_available" : observationErrorCode},
				{"errorMessage", observationErrorMessage.empty() ? "Debug observation value is not available." : observationErrorMessage}
			};
		}

		observedFacts.push_back(observed);
	}

	return true;
}

json DebugSessionMetadataJson(const std::string& debugSessionId, const DebugSession& session)
{
	return {
		{"debugSessionId", debugSessionId},
		{"developerId", session.developerId},
		{"projectId", session.projectId},
		{"formKey", session.formKey},
		{"formVersionId", session.formVersionId},
		{"expiresInSeconds", DebugSessionTtlSeconds()}
	};
}

void RuleServer::HandleDebugCapabilitiesRequest(const httplib::Request& req, httplib::Response& res)
{
	(void)req;

	json out = {
		{"ok", true},
		{"engine", DebugEngineJson()},
		{"supportsScreenPayloads", false},
		{"supportsLeanEvaluation", true},
		{"supportsExternalSessionModel", true},
		{"supportsSessionModelValidation", true},
		{"supportsObservedFacts", true},
		{"sessionTtlSeconds", DebugSessionTtlSeconds()},
		{"endpoints", json::array({
			"/debug/capabilities",
			"/debug/session-model/validate",
			"/debug/session/start",
			"/debug/session/evaluate",
			"/debug/session/end"
		})}
	};

	res.set_content(out.dump(2), "application/json");
}

void RuleServer::HandleDebugSessionModelValidateRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/debug/session-model/validate", req, res);

	try
	{
		json requestJson = req.body.empty() ? json::object() : json::parse(req.body);

		if(!requestJson.is_object())
		{
			WriteDebugError(res, 400, "invalid_debug_validation_request", "Debug Session Model validation requests must be JSON objects.");
			return;
		}

		json sessionModelJson;
		std::string errorCode;
		std::string errorMessage;

		if(!TryGetDebugSessionModelJson(requestJson, sessionModelJson, errorCode, errorMessage))
		{
			WriteDebugError(res, errorCode == "bad_debug_session_model_json" ? 400 : 422, errorCode, errorMessage);
			return;
		}

		SessionModelImporter importer;
		SessionModelImporter::ImportResult importResult = importer.LoadJson(sessionModelJson);
		std::vector<SessionModelImporter::Diagnostic> diagnostics = importResult.diagnostics;
		bool runtimeConverted = false;
		json runtime = nullptr;

		if(!importResult.HasErrors())
		{
			SessionModelImporter::RuntimeConversionResult runtimeResult = importer.ConvertToRuntimeRuleSet(importResult.model);
			AppendDiagnostics(diagnostics, runtimeResult.diagnostics);
			if(!runtimeResult.HasErrors())
			{
				runtimeConverted = true;
				runtime = DebugRuntimeCountsJson(runtimeResult.ruleSet);
			}
		}

		const bool valid = std::none_of(
			diagnostics.begin(),
			diagnostics.end(),
			[](const SessionModelImporter::Diagnostic& diagnostic) {
				return diagnostic.severity == SessionModelImporter::DiagnosticSeverity::Error;
			});
		const int blockingDiagnosticCount = DebugBlockingSessionModelDiagnosticCount(diagnostics);
		json out = {
			{"ok", true},
			{"valid", valid},
			{"debugReady", valid && runtimeConverted && blockingDiagnosticCount == 0},
			{"runtimeConverted", runtimeConverted},
			{"blockingDiagnosticCount", blockingDiagnosticCount},
			{"engine", DebugEngineJson()},
			{"runtime", runtime},
			{"diagnostics", SessionModelDiagnosticsToJson(diagnostics)}
		};

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		WriteDebugError(res, 400, "bad_json", std::string("Invalid JSON: ") + e.what());
	}
}

void RuleServer::HandleDebugSessionStartRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/debug/session/start", req, res);

	try
	{
		json requestJson = req.body.empty() ? json::object() : json::parse(req.body);

		if(!requestJson.is_object())
		{
			WriteDebugError(res, 400, "invalid_debug_start_request", "Debug session start requests must be JSON objects.");
			return;
		}

		const std::string developerId = TrimAscii(first_json_scalar_string(requestJson, {"developerId", "developerid", "developer"}));

		if(developerId.empty())
		{
			WriteDebugError(res, 400, "missing_debug_developer_id", "Debug session start requests require developerId.");
			return;
		}

		json sessionModelJson;
		std::string errorCode;
		std::string errorMessage;

		if(!TryGetDebugSessionModelJson(requestJson, sessionModelJson, errorCode, errorMessage))
		{
			WriteDebugError(res, errorCode == "bad_debug_session_model_json" ? 400 : 422, errorCode, errorMessage);
			return;
		}

		SessionModelImporter importer;
		SessionModelImporter::ImportResult importResult = importer.LoadJson(sessionModelJson);
		std::vector<SessionModelImporter::Diagnostic> diagnostics = importResult.diagnostics;

		if(importResult.HasErrors())
		{
			json out = make_error("debug_session_model_invalid", "Debug session model import failed.");
			out["diagnostics"] = SessionModelDiagnosticsToJson(diagnostics);
			res.status = 422;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		SessionModelImporter::RuntimeConversionResult runtimeResult = importer.ConvertToRuntimeRuleSet(importResult.model);
		AppendDiagnostics(diagnostics, runtimeResult.diagnostics);

		if(runtimeResult.HasErrors())
		{
			json out = make_error("debug_session_model_invalid", "Debug session model runtime conversion failed.");
			out["diagnostics"] = SessionModelDiagnosticsToJson(diagnostics);
			res.status = 422;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		RuleSet debugRuleSet = CloneSessionModelRuleSetTemplate(runtimeResult.ruleSet);

		int createdInstanceCount = 0;
		const json* entityInstances = FindDebugArrayValue(requestJson, {"entityInstances", "instances"}, errorCode, errorMessage);

		if(!errorCode.empty())
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		if(entityInstances != nullptr && !ApplyDebugEntityInstanceDefinitions(debugRuleSet, *entityInstances, createdInstanceCount, errorCode, errorMessage))
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		json changedFacts = json::array();
		int changedFactCount = 0;
		const json* facts = FindDebugArrayValue(requestJson, {"facts", "initialFacts"}, errorCode, errorMessage);

		if(!errorCode.empty())
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		SessionModelRuleEvaluator::EvaluationStats evaluationStats = EvaluateInitialSessionModelRules(
			debugRuleSet,
			facts == nullptr || facts->empty());

		if(facts != nullptr && !ApplyDebugFacts(debugRuleSet, *facts, changedFactCount, changedFacts, evaluationStats, errorCode, errorMessage))
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		DebugSession session;
		session.developerId = developerId;
		session.projectId = TrimAscii(first_json_scalar_string(requestJson, {"projectId", "projectid"}));
		session.formKey = TrimAscii(first_json_scalar_string(requestJson, {"formKey", "form_key", "formkey"}));
		session.formVersionId = TrimAscii(first_json_scalar_string(requestJson, {"formVersionId", "form_version_id", "versionId"}));
		session.created_at = Clock::now();
		session.last_seen = session.created_at;
		session.templateRuleset = CloneSessionModelRuleSetTemplate(runtimeResult.ruleSet);
		session.ruleset = debugRuleSet;
		session.diagnostics = diagnostics;

		const std::string debugSessionId = generate_token(16);

		{
			std::lock_guard<std::mutex> lock(g_debug_sessions_mu);
			g_debug_sessions[debugSessionId] = session;
		}

		bool hasBlockingValidationErrors = false;
		json out = {
			{"ok", true},
			{"engine", DebugEngineJson()},
			{"debugSession", DebugSessionMetadataJson(debugSessionId, session)},
			{"runtime", DebugRuntimeCountsJson(debugRuleSet)},
			{"entityInstances", DebugEntityInstancesJson(debugRuleSet)},
			{"createdInstanceCount", createdInstanceCount},
			{"changedFactCount", changedFactCount},
			{"changedFacts", changedFacts},
			{"validationEvents", BuildActiveValidationEventsJson(debugRuleSet, hasBlockingValidationErrors)},
			{"blocked", hasBlockingValidationErrors},
			{"sessionModelEvaluation", RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats)},
			{"diagnostics", SessionModelDiagnosticsToJson(diagnostics)}
		};

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		WriteDebugError(res, 400, "bad_json", std::string("Invalid JSON: ") + e.what());
	}
}

void RuleServer::HandleDebugSessionEvaluateRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/debug/session/evaluate", req, res);

	try
	{
		json requestJson = req.body.empty() ? json::object() : json::parse(req.body);

		if(!requestJson.is_object())
		{
			WriteDebugError(res, 400, "invalid_debug_evaluate_request", "Debug evaluate requests must be JSON objects.");
			return;
		}

		const std::string debugSessionId = TrimAscii(first_json_scalar_string(requestJson, {"debugSessionId", "sessionId", "sessionid"}));

		if(debugSessionId.empty())
		{
			WriteDebugError(res, 400, "missing_debug_session_id", "Debug evaluate requests require debugSessionId.");
			return;
		}

		remove_expired_debug_sessions();

		std::lock_guard<std::mutex> lock(g_debug_sessions_mu);
		auto sessionIt = g_debug_sessions.find(debugSessionId);

		if(sessionIt == g_debug_sessions.end())
		{
			WriteDebugError(res, 404, "debug_session_not_found", "Debug session was not found or has expired.");
			return;
		}

		DebugSession& session = sessionIt->second;
		session.last_seen = Clock::now();
		RuleSet debugRuleSet = CloneSessionModelRuleSetTemplate(session.templateRuleset);

		std::string errorCode;
		std::string errorMessage;
		int createdInstanceCount = 0;
		const json* entityInstances = FindDebugArrayValue(requestJson, {"entityInstances", "instances"}, errorCode, errorMessage);

		if(!errorCode.empty())
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		if(entityInstances != nullptr && !ApplyDebugEntityInstanceDefinitions(debugRuleSet, *entityInstances, createdInstanceCount, errorCode, errorMessage))
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		json changedFacts = json::array();
		int changedFactCount = 0;
		const json* facts = FindDebugArrayValue(requestJson, {"facts", "changes", "values"}, errorCode, errorMessage);

		if(!errorCode.empty())
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		SessionModelRuleEvaluator::EvaluationStats evaluationStats = EvaluateInitialSessionModelRules(
			debugRuleSet,
			facts == nullptr || facts->empty());

		if(facts != nullptr && !ApplyDebugFacts(debugRuleSet, *facts, changedFactCount, changedFacts, evaluationStats, errorCode, errorMessage))
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		json observedFacts = json::array();
		const json* observations = FindDebugArrayValue(requestJson, {"observations", "observedFacts"}, errorCode, errorMessage);

		if(!errorCode.empty())
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		if(observations != nullptr && !BuildDebugObservedFactsJson(
			debugRuleSet,
			*observations,
			observedFacts,
			errorCode,
			errorMessage))
		{
			WriteDebugError(res, 400, errorCode, errorMessage);
			return;
		}

		json target = nullptr;
		const json* targetValue = FindObjectValue(requestJson, {"target", "debugTarget"});

		if(targetValue != nullptr && !targetValue->is_null())
		{
			target = DebugTargetStateJson(debugRuleSet, *targetValue, errorCode, errorMessage);

			if(!errorCode.empty())
			{
				WriteDebugError(res, 400, errorCode, errorMessage);
				return;
			}
		}

		session.ruleset = debugRuleSet;

		bool hasBlockingValidationErrors = false;
		json out = {
			{"ok", true},
			{"engine", DebugEngineJson()},
			{"debugSession", DebugSessionMetadataJson(debugSessionId, session)},
			{"runtime", DebugRuntimeCountsJson(debugRuleSet)},
			{"entityInstances", DebugEntityInstancesJson(debugRuleSet)},
			{"createdInstanceCount", createdInstanceCount},
			{"changedFactCount", changedFactCount},
			{"changedFacts", changedFacts},
			{"observedFacts", observedFacts},
			{"target", target},
			{"validationEvents", BuildActiveValidationEventsJson(debugRuleSet, hasBlockingValidationErrors)},
			{"blocked", hasBlockingValidationErrors},
			{"sessionModelEvaluation", RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats)},
			{"diagnostics", SessionModelDiagnosticsToJson(session.diagnostics)}
		};

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		WriteDebugError(res, 400, "bad_json", std::string("Invalid JSON: ") + e.what());
	}
}

void RuleServer::HandleDebugSessionEndRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/debug/session/end", req, res);

	try
	{
		json requestJson = req.body.empty() ? json::object() : json::parse(req.body);

		if(!requestJson.is_object())
		{
			WriteDebugError(res, 400, "invalid_debug_end_request", "Debug session end requests must be JSON objects.");
			return;
		}

		const std::string debugSessionId = TrimAscii(first_json_scalar_string(requestJson, {"debugSessionId", "sessionId", "sessionid"}));
		const std::string developerId = TrimAscii(first_json_scalar_string(requestJson, {"developerId", "developerid", "developer"}));
		const std::string projectId = TrimAscii(first_json_scalar_string(requestJson, {"projectId", "projectid"}));
		const std::string formKey = TrimAscii(first_json_scalar_string(requestJson, {"formKey", "form_key", "formkey"}));
		int terminatedCount = 0;

		if(debugSessionId.empty() && developerId.empty())
		{
			WriteDebugError(res, 400, "missing_debug_end_scope", "Debug session end requests require debugSessionId or developerId.");
			return;
		}

		{
			std::lock_guard<std::mutex> lock(g_debug_sessions_mu);

			if(!debugSessionId.empty())
			{
				terminatedCount = static_cast<int>(g_debug_sessions.erase(debugSessionId));
			}
			else
			{
				for(auto it = g_debug_sessions.begin(); it != g_debug_sessions.end();)
				{
					const DebugSession& session = it->second;
					const bool matchesDeveloper = session.developerId == developerId;
					const bool matchesProject = projectId.empty() || session.projectId == projectId;
					const bool matchesForm = formKey.empty() || session.formKey == formKey;

					if(matchesDeveloper && matchesProject && matchesForm)
					{
						it = g_debug_sessions.erase(it);
						++terminatedCount;
					}
					else
					{
						++it;
					}
				}
			}
		}

		json out = {
			{"ok", true},
			{"terminatedCount", terminatedCount}
		};

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		WriteDebugError(res, 400, "bad_json", std::string("Invalid JSON: ") + e.what());
	}
}

void ApplyLaunchSessionContext(RuleSet& ruleSet, const RuleSet::SessionContext& previousContext)
{
	ruleSet.sessionContext = previousContext;
}

bool RuleServer::IsAllowedCorsOrigin(const std::string& origin)
{
	if(origin.empty())
	{
		return false;
	}

	const std::string lowerOrigin = ToLowerAscii(origin);
	const auto isLoopbackOriginWithPort = [](const std::string& value, const std::string& prefix)
	{
		if(value.rfind(prefix, 0) != 0)
		{
			return false;
		}

		const std::string portText = value.substr(prefix.size());
		if(portText.empty() || portText.size() > 5)
		{
			return false;
		}

		int port = 0;
		for(unsigned char ch : portText)
		{
			if(!std::isdigit(ch))
			{
				return false;
			}

			port = (port * 10) + (ch - '0');
		}

		return port > 0 && port <= 65535;
	};

	if(isLoopbackOriginWithPort(lowerOrigin, "http://localhost:")
		|| isLoopbackOriginWithPort(lowerOrigin, "https://localhost:")
		|| isLoopbackOriginWithPort(lowerOrigin, "http://127.0.0.1:")
		|| isLoopbackOriginWithPort(lowerOrigin, "https://127.0.0.1:")
		|| isLoopbackOriginWithPort(lowerOrigin, "http://[::1]:")
		|| isLoopbackOriginWithPort(lowerOrigin, "https://[::1]:"))
	{
		return true;
	}

	const auto normalizeConfiguredOrigin = [](std::string value) -> std::optional<std::string>
	{
		value = TrimAscii(value);
		if(!value.empty() && value.back() == '/')
		{
			value.pop_back();
		}

		const std::string normalized = ToLowerAscii(value);
		const size_t schemeLength = normalized.rfind("https://", 0) == 0
			? 8
			: (normalized.rfind("http://", 0) == 0 ? 7 : 0);
		if(schemeLength == 0 || normalized.size() == schemeLength)
		{
			return std::nullopt;
		}

		const std::string authority = normalized.substr(schemeLength);
		if(authority.find_first_of("/?#@,; \t\r\n") != std::string::npos)
		{
			return std::nullopt;
		}

		return normalized;
	};

	const std::optional<std::string> normalizedRequestOrigin = normalizeConfiguredOrigin(origin);
	if(!normalizedRequestOrigin.has_value())
	{
		return false;
	}

	const std::string configuredOrigins = get_env_or_default("CCP_CORS_ALLOWED_ORIGINS", "");
	size_t start = 0;
	while(start <= configuredOrigins.size())
	{
		const size_t separator = configuredOrigins.find(',', start);
		const size_t length = separator == std::string::npos
			? configuredOrigins.size() - start
			: separator - start;
		const std::optional<std::string> configuredOrigin = normalizeConfiguredOrigin(
			configuredOrigins.substr(start, length));

		if(configuredOrigin.has_value() && configuredOrigin.value() == normalizedRequestOrigin.value())
		{
			return true;
		}

		if(separator == std::string::npos)
		{
			break;
		}

		start = separator + 1;
	}

	return false;
}

void RuleServer::ApplyCorsHeaders(const httplib::Request& req, httplib::Response& res)
{
	const std::string requestOrigin = req.get_header_value("Origin");
	const bool originAllowed = IsAllowedCorsOrigin(requestOrigin);

	auto setSingleHeader = [&res](const char* key, const std::string& value) {
		res.headers.erase(key);
		res.set_header(key, value);
	};

	res.headers.erase("Access-Control-Allow-Origin");
	res.headers.erase("Access-Control-Allow-Credentials");
	if(originAllowed)
	{
		setSingleHeader("Access-Control-Allow-Origin", requestOrigin);
		setSingleHeader("Access-Control-Allow-Credentials", "true");
	}

	setSingleHeader("Vary", "Origin");
	setSingleHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	setSingleHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Auth-Token");
	setSingleHeader("Access-Control-Max-Age", "3600");
}

RuleSet BuildCleanRuleSetForSession(
	const Session& session,
	const RuleSet::SessionContext& previousContext,
	bool evaluateInitialRules = true)
{
	RuleSet cleanRuleSet;

	if(session.selectedForm.selected)
	{
		SelectedFormLoadMetricScope metric("selected_form_template_clone");
		cleanRuleSet = CloneSessionModelRuleSetTemplate(session.selectedForm.runtimeTemplate);
	}
	else
	{
		cleanRuleSet = RuleServer::GetCleanRuleSet();
	}

	ApplyLaunchSessionContext(cleanRuleSet, previousContext);

	if(evaluateInitialRules && session.selectedForm.selected)
	{
		SelectedFormLoadMetricScope metric("selected_form_initial_rules");
		EvaluateInitialSessionModelRules(cleanRuleSet);
	}
	else if(evaluateInitialRules)
	{
		EvaluateInitialSessionModelRules(cleanRuleSet);
	}

	return cleanRuleSet;
}

ScreenFile::ScreenData& EnsureSessionScreenDataLoaded(Session& session)
{
	if(session.screenData.screens.empty())
	{
		session.screenData = CurrentDefaultScreenDataCopy();
	}

	return session.screenData;
}

json SelectedFormRuntimeToJson(const SelectedFormRuntime& form)
{
	json out = {
		{"selected", form.selected}
	};

	if(!form.selected)
	{
		return out;
	}

	out["formId"] = form.formId;
	out["formKey"] = form.formKey;
	out["name"] = form.formName;
	out["description"] = form.formDescription;
	out["formVersionId"] = form.formVersionId;
	out["versionNumber"] = form.versionNumber;
	out["versionLabel"] = form.versionLabel;
	out["sessionModelHash"] = form.sessionModelHash;
	out["sessionModelSchemaVersion"] = form.sessionModelSchemaVersion;
	out["sourceName"] = form.sourceName;
	out["publishedAt"] = form.publishedAt;
	out["sessionModelSource"] = "database";
	if(form.externalSecurity.enabled)
	{
		out["externalSecurity"] = {
			{"enabled", true},
			{"provider", form.externalSecurity.provider},
			{"approvedProfileKey", form.externalSecurity.approvedProfileKey},
			{"issuerUrl", form.externalSecurity.issuerUrl},
			{"discoveryUrl", form.externalSecurity.discoveryUrl},
			{"authorizationUrl", form.externalSecurity.authorizationUrl},
			{"logoutUrl", form.externalSecurity.logoutUrl},
			{"tokenUrl", form.externalSecurity.tokenUrl},
			{"jwksUrl", form.externalSecurity.jwksUrl},
			{"introspectionUrl", form.externalSecurity.introspectionUrl},
			{"tokenExchangeUrl", form.externalSecurity.tokenExchangeUrl},
			{"userInfoUrl", form.externalSecurity.userInfoUrl},
			{"audience", form.externalSecurity.audience},
			{"scopes", form.externalSecurity.scopes},
			{"returnRouteIdentifier", form.externalSecurity.returnRouteIdentifier}
		};
	}

	return out;
}

struct SessionAuthenticationModeValidation
{
	bool succeeded{false};
	int httpStatus{403};
	std::string code;
	std::string message;
};

SessionAuthenticationModeValidation ValidateSessionAuthenticationModeForForm(
	const Session& session,
	const SelectedFormRuntime& form,
	const RuleSet::ExternalSecurityPolicy& policy)
{
	if(!session.externalIdentity.enabled)
	{
		if(policy.enabled)
		{
			return {false, 403, "external_authentication_required", "This form requires an externally authenticated FastForm session."};
		}

		return {true, 200, std::string(), std::string()};
	}

	if(!form.selected)
	{
		return {false, 403, "external_form_required", "An externally authenticated session must remain bound to its selected form."};
	}

	if(!policy.enabled || !EqualsIgnoreCase(policy.provider, "FastForm"))
	{
		return {false, 403, "external_form_mode_mismatch", "The selected form is not enabled for FastForm authentication."};
	}

	if(policy.approvedProfileKey != session.externalIdentity.approvedProfileKey
		|| policy.issuerUrl != session.externalIdentity.issuer
		|| form.formKey != session.externalIdentity.formKey
		|| (!session.externalIdentity.formVersionId.empty()
			&& form.formVersionId != session.externalIdentity.formVersionId))
	{
		return {false, 403, "external_identity_binding_mismatch", "The selected form does not match the external session binding."};
	}

	return {true, 200, std::string(), std::string()};
}

SessionAuthenticationModeValidation ValidateSessionAuthenticationModeForCurrentRuntime(const Session& session)
{
	const RuleSet::ExternalSecurityPolicy& policy = session.selectedForm.selected
		? session.selectedForm.externalSecurity
		: session.ruleset.externalSecurity;
	return ValidateSessionAuthenticationModeForForm(session, session.selectedForm, policy);
}

bool WriteSessionAuthenticationModeFailure(
	httplib::Response& response,
	const SessionAuthenticationModeValidation& validation)
{
	if(validation.succeeded)
	{
		return false;
	}

	response.status = validation.httpStatus;
	response.set_header("Cache-Control", "no-store");
	response.set_content(make_error(validation.code, validation.message).dump(2), "application/json");
	return true;
}

SelectedFormSessionModelLoadResult ApplySelectedFormSelectionToSession(Session& session, const FormLaunchSelection& selection)
{
	FormCatalogSettings settings = LoadFormCatalogSettings();
	SelectedFormSessionModelLoadResult selectedFormLoad = LoadSelectedFormSessionModelFromCatalog(settings, session, selection);

	if(!selectedFormLoad.succeeded)
	{
		return selectedFormLoad;
	}

	const RuleSet::SessionContext previousContext = session.ruleset.sessionContext;
	session.selectedForm = selectedFormLoad.selectedForm;
	session.screenData = selectedFormLoad.screenData;
	session.ruleset = BuildCleanRuleSetForSession(session, previousContext);
	session.message_count = 0;

	return selectedFormLoad;
}

bool WriteSelectedFormLoadFailure(httplib::Response& res, const SelectedFormSessionModelLoadResult& selectedFormLoad)
{
	if(selectedFormLoad.succeeded)
	{
		return false;
	}

	res.status = selectedFormLoad.httpStatus;
	json error = make_error(selectedFormLoad.code, selectedFormLoad.message);

	if(!selectedFormLoad.diagnostics.empty())
	{
		error["diagnostics"] = selectedFormLoad.diagnostics;
	}

	res.set_content(error.dump(2), "application/json");
	return true;
}

bool store_session_runtime(const std::string& token,
                           const Session& session,
                           std::uint64_t messageCountBeforeIncrement,
                           Session* updated = nullptr)
{
	std::lock_guard<std::mutex> lock(g_sessions_mu);

	auto it = g_sessions.find(token);

	if(it == g_sessions.end())
	{
		return false;
	}

	it->second.ruleset = session.ruleset;
	it->second.screenData = session.screenData;
	it->second.selectedForm = session.selectedForm;
	it->second.emailCommandsBySequence = session.emailCommandsBySequence;
	it->second.last_seen = Clock::now();
	it->second.message_count = messageCountBeforeIncrement + 1;

	if(updated)
	{
		*updated = it->second;
	}

	return true;
}

ScreenData::DataRoot RuleServer::ParseScreenDataJson(const std::string jsonString)
{
	ScreenData::DataRoot dataRoot;

	try
	{
		auto j = json::parse(jsonString);

		const string id = j.value("screenid", std::string());
		const string entityId = j.value("entityId", std::string());
		const string instanceId = j.value("instanceId", std::string());

		const string nextScreenId = j.value("nextScreenId", std::string());
		const string nextEntityId = j.value("nextEntityId", std::string());
		const string nextInstanceId = j.value("nextInstanceId", std::string());

		const json* arr = &j["data"];
				
		dataRoot.screenid = id;
		dataRoot.entityId = entityId;
		dataRoot.instanceId = instanceId;
		dataRoot.nextScreenId = nextScreenId;
		dataRoot.nextEntityId = nextEntityId;
		dataRoot.nextInstanceId = nextInstanceId;

		if(arr)
		{
			for(const auto& item : *arr)
			{
				ScreenData::Data d;

				if(item.contains("value"))
				{
					auto value = item["value"];

					if(SubmitValueIsSupported(value))
					{
						d.value = SubmitValueToString(value);
					}
					else
					{
						throw json::type_error::create(302, "value must be a scalar value", &item);
					}
				}

				d.attributeId = first_json_scalar_string(item, {"attributeId", "attributeid", "id", "elementId", "elementid"});

				dataRoot.data.push_back(std::move(d));
			}
		}
	}
	catch(const std::exception& e)
	{
		std::cerr << "Failed to parse screen data JSON: " << e.what() << std::endl;
	}

	return dataRoot;
}

RuleSet RuleServer::GetCleanRuleSet()
{
	if(!loaded)
	{
		Load();
	}

	if(loadedFromSessionModel)
	{
		return CloneSessionModelRuleSetTemplate(sessionModelRuntimeTemplate);
	}

	RuleSet rs = RuleSet();

	for(const JSONFile::Entity& ent : RuleServer::model.entities)
	{
		shared_ptr<Entity> entity = make_shared<Entity>(ent);

		rs.entities->push_back(entity);

		for(const JSONFile::JSONAttribute& ja : ent.attributes)
		{
			shared_ptr<Attribute> att = make_shared<Attribute>(ja);

			entity->attributes.push_back(att);
		}

		for(const JSONFile::JSONRule& jr : ent.rules)
		{
			shared_ptr<Rule> rule = Rule::Create(jr, make_shared<vector<shared_ptr<Attribute>>>(entity->attributes));

			entity->rules.push_back(rule);
		}

		if(entity->root == true)
		{
			entity->AddInstance();
		}

		for(shared_ptr<Rule>& rule : entity->rules)
		{
			for(shared_ptr<Condition>& cond : rule->conditions)
			{
				shared_ptr<Attribute> att = cond->GetAttribute();

				if(att->IsInferred())
				{
					for(shared_ptr<Rule>& rule2 : entity->rules)
					{
						if(rule2->GetAttribute()->GetID() == att->GetID())
						{
							rule2->AddRule(rule);

							break;
						}
					}
				}
			}
		}

		for(const JSONFile::Relationship& rel : ent.relationships)
		{
			shared_ptr<Relationship> relationship = make_shared<Relationship>();

			relationship->id = rel.id;
			relationship->description = rel.description;
			relationship->sourceEntity = entity->entityid;
			relationship->targetEntity = rel.targetentity;
			relationship->type = rel.type;

			entity->relationships.push_back(relationship);
		}
	}

	ApplySessionContextEnvironment(rs, true);

	return rs;
}

shared_ptr<Instance> RuleServer::GetCleanInstance(string entityId)
{
	for(const JSONFile::Entity& ent : RuleServer::model.entities)
	{
		if(ent.entityid == entityId)
		{
			shared_ptr<Instance> instance = make_shared<Instance>();

			instance->entityId = entityId;

			for(const JSONFile::JSONAttribute& ja : ent.attributes)
			{
				shared_ptr<Attribute> att = make_shared<Attribute>(ja);

				att->instance = instance;

				instance->attributes.push_back(att);
			}

			for(const JSONFile::JSONRule& jr : ent.rules)
			{
				RuleType rt = Rule::StringToRuleType(jr.ruletype);

				shared_ptr<Rule> rule = Rule::Create(jr, make_shared<vector<shared_ptr<Attribute>>>(instance->attributes));

				instance->rules.push_back(rule);
			}

			for(shared_ptr<Rule>& rule : instance->rules)
			{
				for(shared_ptr<Condition>& cond : rule->conditions)
				{
					shared_ptr<Attribute> att = cond->GetAttribute();

					if(att->IsInferred())
					{
						for(shared_ptr<Rule>& rule2 : instance->rules)
						{
							if(rule2->GetAttribute()->GetID() == att->GetID())
							{
								rule2->AddRule(rule);

								break;
							}
						}
					}
				}
			}

			return instance;

			break;
		}
	}

	return nullptr;
}

void RuleServer::HandleLoginRequest(const httplib::Request& req, httplib::Response& res)
{
	try
	{
		const auto body = json::parse(req.body);

		if(!body.is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_login_request", "Login requests must be JSON objects.").dump(2), "application/json");
			return;
		}

		const auto allSuppliedFieldsAreStrings = [&](std::initializer_list<const char*> keys) {
			for(const char* key : keys)
			{
				if(body.contains(key) && !body.at(key).is_string())
				{
					return false;
				}
			}

			return true;
		};

		if(!allSuppliedFieldsAreStrings({"client_id", "clientId", "username", "userName"})
			|| !allSuppliedFieldsAreStrings({"client_secret", "clientSecret", "password"}))
		{
			res.status = 400;
			res.set_content(make_error("invalid_login_request", "Login credential fields must be strings when supplied.").dump(2), "application/json");
			return;
		}

		const std::string client_id = JsonStringValue(body, {"client_id", "clientId"});
		const std::string client_secret = JsonStringValue(body, {"client_secret", "clientSecret"});

		LoginIdentity identity;

		if(valid_credentials(client_id, client_secret))
		{
			identity.authenticated = true;
			identity.principal = client_id;
			identity.userId = build_user_id_from_client_id(client_id);
			identity.userName = client_id;
		}
		else
		{
			const std::string username = JsonStringValue(body, {"username", "userName", "client_id", "clientId"});
			const std::string password = JsonStringValue(body, {"password", "client_secret", "clientSecret"});
			identity = TryAuthenticateUserFromTable(username, password);
		}

		if(!identity.authenticated)
		{
			res.status = identity.failureCode == "auth_unavailable" ? 503 : 401;

			const std::string errorCode = identity.failureCode.empty() ? "invalid_credentials" : identity.failureCode;
			const std::string errorMessage = identity.failureMessage.empty()
				? "Username or password is incorrect."
				: identity.failureMessage;
			res.set_content(make_error(errorCode, errorMessage).dump(2), "application/json");

			return;
		}

		const std::string token = generate_token();

		const auto now = Clock::now();

		RuleSet rs = GetCleanRuleSet();
		rs.sessionContext.userId = identity.userId;
		rs.sessionContext.userName = identity.displayName.empty() ? identity.userName : identity.displayName;
		EvaluateInitialSessionModelRules(rs);

		std::string rootEntityId;
		std::string rootInstanceId;

		if(rs.entities != nullptr)
		{
			for(const std::shared_ptr<Entity>& entity : *rs.entities)
			{
				if(entity != nullptr && entity->root.value_or(false) && !entity->instances.empty())
				{
					rootEntityId = entity->entityid;

					if(entity->instances[0] != nullptr)
					{
						rootInstanceId = entity->instances[0]->instanceId;
					}

					break;
				}
			}
		}

		{
			std::lock_guard<std::mutex> lock(g_sessions_mu);

			g_sessions[token] = Session(identity.principal, identity.userId, now, now, 0, rs, CurrentDefaultScreenDataCopy());
		}

		json out = {
			{"ok", true},
			{"token", token},
			{"token_type", "Bearer"},
			{"expires_in_seconds", 1800},
			{"userId", identity.userId},
			{"userName", identity.userName},
		};

		if(!identity.displayName.empty())
		{
			out["displayName"] = identity.displayName;
		}

		if(!rootEntityId.empty() && !rootInstanceId.empty())
		{
			out["rootEntityId"] = rootEntityId;
			out["rootInstanceId"] = rootInstanceId;
		}

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleExternalLoginRequest(const httplib::Request& req, httplib::Response& res)
{
	res.set_header("Cache-Control", "no-store");

	try
	{
		const json body = json::parse(req.body);
		if(!body.is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_external_login_request", "External login requests must be JSON objects.").dump(2), "application/json");
			return;
		}

		for(const char* forbiddenKey : {"accessToken", "refreshToken", "idToken", "externalToken"})
		{
			if(body.contains(forbiddenKey))
			{
				res.status = 400;
				res.set_content(make_error("external_token_forbidden", "External provider tokens must terminate at GDS Render and must not be sent to the Session Manager.").dump(2), "application/json");
				return;
			}
		}

		const auto requireString = [&](const char* key, std::size_t maximumLength, bool required = true) -> std::optional<std::string> {
			if(!body.contains(key))
			{
				return required ? std::nullopt : std::optional<std::string>(std::string());
			}

			if(!body.at(key).is_string())
			{
				return std::nullopt;
			}

			const std::string value = body.at(key).get<std::string>();
			if(value.size() > maximumLength || TrimAscii(value) != value || (required && value.empty()))
			{
				return std::nullopt;
			}

			for(unsigned char character : value)
			{
				if(character < 0x20 || character == 0x7f)
				{
					return std::nullopt;
				}
			}

			return value;
		};

		const std::optional<std::string> workloadClientId = requireString("workloadClientId", 256);
		const std::optional<std::string> workloadClientSecret = requireString("workloadClientSecret", 4096);
		const std::optional<std::string> issuer = requireString("issuer", 2048);
		const std::optional<std::string> subject = requireString("subject", 256);
		const std::optional<std::string> tenantId = requireString("tenantId", 256);
		const std::optional<std::string> providerSessionId = requireString("providerSessionId", 256);
		const std::optional<std::string> launchId = requireString("launchId", 256);
		const std::optional<std::string> approvedProfileKey = requireString("approvedProfileKey", 128);
		const std::optional<std::string> formKey = requireString("formKey", 128);
		const std::optional<std::string> formVersionId = requireString("formVersionId", 128, false);

		if(!workloadClientId.has_value()
			|| !workloadClientSecret.has_value()
			|| !issuer.has_value()
			|| !subject.has_value()
			|| !tenantId.has_value()
			|| !providerSessionId.has_value()
			|| !launchId.has_value()
			|| !approvedProfileKey.has_value()
			|| !formKey.has_value()
			|| !formVersionId.has_value())
		{
			res.status = 400;
			res.set_content(make_error("external_identity_invalid", "The trusted external identity binding is incomplete or invalid.").dump(2), "application/json");
			return;
		}

		if(!valid_credentials(workloadClientId.value(), workloadClientSecret.value()))
		{
			res.status = 401;
			res.set_content(make_error("invalid_workload_credentials", "The GDS workload credentials are invalid.").dump(2), "application/json");
			return;
		}

		RuleSet ruleset = GetCleanRuleSet();
		ruleset.sessionContext.userId = subject.value();
		ruleset.sessionContext.userName = subject.value();
		const auto now = Clock::now();
		Session candidate(workloadClientId.value(), subject.value(), now, now, 0, ruleset, CurrentDefaultScreenDataCopy());
		candidate.externalIdentity.enabled = true;
		candidate.externalIdentity.issuer = issuer.value();
		candidate.externalIdentity.subject = subject.value();
		candidate.externalIdentity.tenantId = tenantId.value();
		candidate.externalIdentity.providerSessionId = providerSessionId.value();
		candidate.externalIdentity.launchId = launchId.value();
		candidate.externalIdentity.approvedProfileKey = approvedProfileKey.value();
		candidate.externalIdentity.formKey = formKey.value();
		candidate.externalIdentity.formVersionId = formVersionId.value();

		json selectionJson = {{"formKey", formKey.value()}};
		if(!formVersionId->empty())
		{
			selectionJson["formVersionId"] = formVersionId.value();
		}

		const FormLaunchSelection selection = ParseFormLaunchSelection(selectionJson);
		FormCatalogSettings settings = LoadFormCatalogSettings();
		SelectedFormSessionModelLoadResult selectedFormLoad = LoadSelectedFormSessionModelFromCatalog(settings, candidate, selection);
		if(WriteSelectedFormLoadFailure(res, selectedFormLoad))
		{
			return;
		}

		const SessionAuthenticationModeValidation modeValidation = ValidateSessionAuthenticationModeForForm(
			candidate,
			selectedFormLoad.selectedForm,
			selectedFormLoad.selectedForm.externalSecurity);
		if(WriteSessionAuthenticationModeFailure(res, modeValidation))
		{
			return;
		}

		const RuleSet::SessionContext previousContext = candidate.ruleset.sessionContext;
		candidate.selectedForm = selectedFormLoad.selectedForm;
		candidate.screenData = selectedFormLoad.screenData;
		candidate.ruleset = BuildCleanRuleSetForSession(candidate, previousContext, false);
		EvaluateInitialSessionModelRules(candidate.ruleset);

		const std::string token = generate_token();
		{
			std::lock_guard<std::mutex> lock(g_sessions_mu);
			g_sessions[token] = candidate;
		}

		json startInputs = json::array();
		if(candidate.ruleset.startInputs != nullptr)
		{
			for(const RuleSet::StartInputDefinition& input : *candidate.ruleset.startInputs)
			{
				startInputs.push_back({
					{"entityId", input.entityId},
					{"attributeId", input.attributeId},
					{"attributeName", input.attributeName},
					{"coreName", input.coreName},
					{"attributeType", input.attributeType},
					{"required", input.required}
				});
			}
		}

		json out = {
			{"ok", true},
			{"token", token},
			{"token_type", "Bearer"},
			{"expires_in_seconds", 1800},
			{"authenticationMode", "FastForm"},
			{"userId", subject.value()},
			{"userName", subject.value()},
			{"startInputCount", static_cast<int>(startInputs.size())},
			{"startInputs", std::move(startInputs)},
			{"form", SelectedFormRuntimeToJson(candidate.selectedForm)}
		};

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;
		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleMeRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);

		auto it = g_sessions.find(token);

		if(it != g_sessions.end())
		{
			it->second.last_seen = Clock::now();
		}
	}

	json out = {
		{"ok", true},
		{"client_id", s.client_id},
		{"userId", s.user_id},
		{"authenticationMode", s.externalIdentity.enabled ? "FastForm" : "Legacy"},
		{"created_at_utc", [&] {
			const std::time_t t = Clock::to_time_t(s.created_at);
			std::tm tm{};
#if defined(_WIN32)
			gmtime_s(&tm, &t);
#else
			gmtime_r(&tm, &t);
#endif
			std::ostringstream oss;

			oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");

			return oss.str();
		}()},
	};

	if(s.externalIdentity.enabled)
	{
		out["externalIdentity"] = {
			{"issuer", s.externalIdentity.issuer},
			{"subject", s.externalIdentity.subject},
			{"tenantId", s.externalIdentity.tenantId},
			{"providerSessionId", s.externalIdentity.providerSessionId},
			{"launchId", s.externalIdentity.launchId},
			{"approvedProfileKey", s.externalIdentity.approvedProfileKey},
			{"formKey", s.externalIdentity.formKey},
			{"formVersionId", s.externalIdentity.formVersionId}
		};
	}

	res.set_content(out.dump(2), "application/json");
}

void RuleServer::HandleLogoutRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	bool removed = false;
	{
		std::lock_guard<std::mutex> lock(g_sessions_mu);

		removed = (g_sessions.erase(token) > 0);
	}

	if(!removed)
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or already logged out.").dump(2), "application/json");

		return;
	}

	res.set_content(json{{"ok", true}}.dump(2), "application/json");
}

void RuleServer::HandleFormsRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;
		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");
		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
		return;
	}

	try
	{
		json requestJson = json::object();

		if(!TrimAscii(req.body).empty())
		{
			requestJson = json::parse(req.body);
		}

		if(!requestJson.is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_request_shape", "Forms requests must be JSON objects when a body is supplied.").dump(2), "application/json");
			return;
		}

		FormCatalogSettings settings = LoadFormCatalogSettings();

		if(!settings.enabled)
		{
			res.status = 409;
			res.set_content(make_error("form_catalog_disabled", "The form catalogue is disabled in Session Manager configuration.").dump(2), "application/json");
			return;
		}

		const std::optional<int> requestedMaxRows = JsonIntValue(requestJson, {"maxRows"});
		if(requestedMaxRows.has_value())
		{
			settings.maxRows = std::max(1, std::min(1000, requestedMaxRows.value()));
		}

		const json loadPlan = BuildFormCatalogLoadPlan(settings, s);
		const json execution = BuildDataConnectionLoadPlanExecutionResult(loadPlan, settings.maxRows);
		json out = BuildAvailableFormsFromCatalogExecution(execution, settings, s);

		if(!out.value("ok", false))
		{
			res.status = execution.value("attempted", false) ? 502 : 409;
		}

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;
		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleStartInputsRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;
		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");
		return;
	}

	Session candidate;

	if(!try_get_session(token, candidate))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
		return;
	}

	try
	{
		json requestJson = json::object();

		if(!TrimAscii(req.body).empty())
		{
			requestJson = json::parse(req.body);
		}

		if(!requestJson.is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_request_shape", "Start-input requests must be JSON objects when a body is supplied.").dump(2), "application/json");
			return;
		}

		const FormLaunchSelection selection = ParseFormLaunchSelection(requestJson);

		if(selection.requested)
		{
			SelectedFormSessionModelLoadResult selectedFormLoad = ApplySelectedFormSelectionToSession(candidate, selection);

			if(WriteSelectedFormLoadFailure(res, selectedFormLoad))
			{
				return;
			}
		}

		if(selection.requested || candidate.externalIdentity.enabled || candidate.ruleset.externalSecurity.enabled)
		{
			const SessionAuthenticationModeValidation modeValidation = ValidateSessionAuthenticationModeForCurrentRuntime(candidate);
			if(WriteSessionAuthenticationModeFailure(res, modeValidation))
			{
				return;
			}
		}

		json startInputs = json::array();

		if(candidate.ruleset.startInputs != nullptr)
		{
			for(const RuleSet::StartInputDefinition& input : *candidate.ruleset.startInputs)
			{
				startInputs.push_back({
					{"entityId", input.entityId},
					{"attributeId", input.attributeId},
					{"attributeName", input.attributeName},
					{"coreName", input.coreName},
					{"attributeType", input.attributeType},
					{"required", input.required}
				});
			}
		}

		json out = {
			{"ok", true},
			{"startInputCount", static_cast<int>(startInputs.size())},
			{"startInputs", std::move(startInputs)}
		};

		if(candidate.selectedForm.selected)
		{
			out["form"] = SelectedFormRuntimeToJson(candidate.selectedForm);
		}

		res.set_header("Cache-Control", "no-store");
		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;
		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleReloadSessionModelRequest(const httplib::Request&, httplib::Response& res)
{
	try
	{
		RuleServer::Load();

		const std::vector<SessionModelImporter::Diagnostic>& diagnostics = RuleServer::GetSessionModelDiagnostics();
		json out = {
			{"ok", true},
			{"loaded", RuleServer::loaded},
			{"sessionModel", RuleServer::IsSessionModelLoaded()},
			{"diagnosticCount", diagnostics.size()},
			{"message", RuleServer::IsSessionModelLoaded()
				? "Session model JSON reloaded."
				: "Model JSON reloaded."}
		};

		if(!diagnostics.empty())
		{
			out["diagnostics"] = SessionModelDiagnosticsToJson(diagnostics);
		}

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& exception)
	{
		res.status = 500;
		res.set_content(make_error("session_model_reload_failed", std::string("Could not reload the Session Model JSON file. ") + exception.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleSessionContextRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	SessionModelRuleEvaluator::EvaluationStats evaluationStats;
	bool hasSessionModelEvaluation = false;
	bool historySnapshotChanged = false;
	std::vector<std::string> changedContextFunctions;

	auto addChangedFunction = [&](const std::string& functionName) {
		if(!functionName.empty() && std::find(changedContextFunctions.begin(), changedContextFunctions.end(), functionName) == changedContextFunctions.end())
		{
			changedContextFunctions.push_back(functionName);
		}
	};

	try
	{
		const std::string oldCurrentDate = s.ruleset.GetSessionCurrentDate();
		const std::string oldCurrentDateTime = s.ruleset.GetSessionCurrentDateTime();
		const std::string oldCurrentTime = s.ruleset.GetSessionCurrentTime();
		const RuleSet::SessionContext oldContext = s.ruleset.sessionContext;
		auto j = json::parse(req.body);

		if(j.contains("sessionStartDateTime"))
		{
			s.ruleset.SetSessionStartDateTime(j.value("sessionStartDateTime", std::string()));
		}

		const bool hasSessionMetadata = j.contains("currentLocale") || j.contains("deploymentName") || j.contains("deploymentVersion");

		if(hasSessionMetadata)
		{
			const std::string currentLocale = j.contains("currentLocale")
				? j.value("currentLocale", std::string())
				: s.ruleset.sessionContext.currentLocale;
			const std::string deploymentName = j.contains("deploymentName")
				? j.value("deploymentName", std::string())
				: s.ruleset.sessionContext.deploymentName;
			const std::string deploymentVersion = j.contains("deploymentVersion")
				? j.value("deploymentVersion", std::string())
				: s.ruleset.sessionContext.deploymentVersion;

			s.ruleset.SetSessionMetadata(currentLocale, deploymentName, deploymentVersion);
		}

		if(j.contains("asOfTransactionTime") || j.contains("historySnapshotTime"))
		{
			const std::string historySnapshotTime = first_json_scalar_string(j, {"asOfTransactionTime", "historySnapshotTime"});

			if(!RuleSet::IsValidTransactionTime(historySnapshotTime))
			{
				res.status = 400;
				res.set_content(make_error("history_snapshot_time_invalid", "History snapshot time must be an ISO date or date-time value.").dump(2), "application/json");

				return;
			}

			s.ruleset.SetHistorySnapshotTime(historySnapshotTime);
		}

		if(j.contains("sessionStartDateTime"))
		{
			if(s.ruleset.GetSessionCurrentDate() != oldCurrentDate)
			{
				addChangedFunction("CurrentDate");
			}

			if(s.ruleset.GetSessionCurrentDateTime() != oldCurrentDateTime)
			{
				addChangedFunction("CurrentDateTime");
			}

			if(s.ruleset.GetSessionCurrentTime() != oldCurrentTime)
			{
				addChangedFunction("CurrentTime");
			}
		}

		if(s.ruleset.sessionContext.currentLocale != oldContext.currentLocale)
		{
			addChangedFunction("CurrentLocale");
		}

		if(s.ruleset.sessionContext.deploymentName != oldContext.deploymentName)
		{
			addChangedFunction("DeploymentName");
		}

		if(s.ruleset.sessionContext.deploymentVersion != oldContext.deploymentVersion)
		{
			addChangedFunction("DeploymentVersion");
		}

		historySnapshotChanged = s.ruleset.sessionContext.asOfTransactionTime != oldContext.asOfTransactionTime;
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	if(s.ruleset.sessionModelRules != nullptr && !s.ruleset.sessionModelRules->empty() && (!changedContextFunctions.empty() || historySnapshotChanged))
	{
		SessionModelRuleEvaluator evaluator;

		if(!changedContextFunctions.empty())
		{
			MergeEvaluationStats(evaluationStats, evaluator.EvaluateChangedSessionContext(s.ruleset, changedContextFunctions));
		}

		if(historySnapshotChanged)
		{
			MergeEvaluationStats(evaluationStats, evaluator.EvaluateChangedHistorySnapshot(s.ruleset));
		}

		hasSessionModelEvaluation = true;
	}

	Session updated;

	if(!store_session_context(token, s.ruleset.sessionContext, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	json out = {
		{"ok", true},
		{"sessionContext", SessionContextToJSON(updated.ruleset.sessionContext)}
	};

	if(hasSessionModelEvaluation)
	{
		out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats);
	}

	res.set_content(out.dump(4), "application/json");
}

void RuleServer::HandleSessionHistoryRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	struct ChangedHistoryAttribute
	{
		std::string entityId;
		std::string instanceId;
		std::string attributeId;
		int segmentsAdded = 0;
		std::vector<std::pair<std::string, std::string>> effectiveIntervals;
	};

	struct ChangedEntityExistenceHistory
	{
		std::string entityId;
		std::string instanceId;
		int segmentsAdded = 0;
		std::vector<std::pair<std::string, std::string>> effectiveIntervals;
	};

	struct ChangedRelationshipLinkHistory
	{
		std::string relationshipId;
		std::string sourceEntityId;
		std::string sourceInstanceId;
		std::string targetEntityId;
		std::string targetInstanceId;
		int segmentsAdded = 0;
		std::vector<std::pair<std::string, std::string>> effectiveIntervals;
	};

	struct ChangedRelationshipCoverageHistory
	{
		std::string relationshipId;
		std::string sourceEntityId;
		std::string sourceInstanceId;
		int segmentsAdded = 0;
		std::vector<std::pair<std::string, std::string>> effectiveIntervals;
	};

	std::vector<ChangedHistoryAttribute> changedAttributes;
	std::vector<ChangedEntityExistenceHistory> changedEntityExistence;
	std::vector<ChangedRelationshipLinkHistory> changedRelationshipLinks;
	std::vector<ChangedRelationshipCoverageHistory> changedRelationshipCoverage;
	std::unordered_map<std::string, size_t> changedAttributeIndexByKey;
	std::unordered_map<std::string, size_t> changedEntityExistenceIndexByKey;
	std::unordered_map<std::string, size_t> changedRelationshipLinkIndexByKey;
	std::unordered_map<std::string, size_t> changedRelationshipCoverageIndexByKey;
	SessionModelRuleEvaluator::EvaluationStats evaluationStats;
	bool hasSessionModelEvaluation = false;
	int segmentsAdded = 0;
	int attributeSegmentsAdded = 0;
	int entityExistenceSegmentsAdded = 0;
	int relationshipLinkSegmentsAdded = 0;
	int relationshipCoverageSegmentsAdded = 0;
	const size_t initialHistorySegmentCount = s.ruleset.attributeHistorySegments != nullptr ? s.ruleset.attributeHistorySegments->size() : 0;
	const size_t initialEntityExistenceHistorySegmentCount = s.ruleset.entityExistenceHistorySegments != nullptr ? s.ruleset.entityExistenceHistorySegments->size() : 0;
	const size_t initialRelationshipLinkHistorySegmentCount = s.ruleset.relationshipLinkHistorySegments != nullptr ? s.ruleset.relationshipLinkHistorySegments->size() : 0;
	const size_t initialRelationshipCoverageHistorySegmentCount = s.ruleset.relationshipCoverageHistorySegments != nullptr ? s.ruleset.relationshipCoverageHistorySegments->size() : 0;

	auto rollbackHistoryMutation = [&]() {
		if(s.ruleset.attributeHistorySegments != nullptr && s.ruleset.attributeHistorySegments->size() > initialHistorySegmentCount)
		{
			s.ruleset.attributeHistorySegments->resize(initialHistorySegmentCount);
			s.ruleset.RebuildAttributeHistoryIndex();
		}

		if(s.ruleset.entityExistenceHistorySegments != nullptr && s.ruleset.entityExistenceHistorySegments->size() > initialEntityExistenceHistorySegmentCount)
		{
			s.ruleset.entityExistenceHistorySegments->resize(initialEntityExistenceHistorySegmentCount);
			s.ruleset.RebuildEntityExistenceHistoryIndex();
		}

		if(s.ruleset.relationshipLinkHistorySegments != nullptr && s.ruleset.relationshipLinkHistorySegments->size() > initialRelationshipLinkHistorySegmentCount)
		{
			s.ruleset.relationshipLinkHistorySegments->resize(initialRelationshipLinkHistorySegmentCount);
			s.ruleset.RebuildRelationshipLinkHistoryIndex();
		}

		if(s.ruleset.relationshipCoverageHistorySegments != nullptr && s.ruleset.relationshipCoverageHistorySegments->size() > initialRelationshipCoverageHistorySegmentCount)
		{
			s.ruleset.relationshipCoverageHistorySegments->resize(initialRelationshipCoverageHistorySegmentCount);
			s.ruleset.RebuildRelationshipCoverageHistoryIndex();
		}
	};

	try
	{
		auto j = json::parse(req.body);

		const std::string rootTransactionStart = first_json_scalar_string(j, {"asOfTransactionTime", "transactionStart"});
		const std::string rootTransactionEnd = get_json_scalar_string(j, "transactionEnd");
		const std::string rootAssertedAt = get_json_scalar_string(j, "assertedAt");
		const std::string rootEvaluationTime = get_json_scalar_string(j, "evaluationTime");
		const std::string rootModelTimeZone = get_json_scalar_string(j, "modelTimeZone");
		const std::string rootSource = get_json_scalar_string(j, "source");

		std::string errorCode;
		std::string errorMessage;
		int errorStatus = 400;

		auto recordChangedAttribute = [&](const std::string& entityId,
		                                  const std::string& instanceId,
		                                  const std::string& attributeId,
		                                  const std::string& effectiveStart,
		                                  const std::string& effectiveEnd) {
			const std::string key = history_change_key(entityId, instanceId, attributeId);
			auto it = changedAttributeIndexByKey.find(key);

			if(it == changedAttributeIndexByKey.end())
			{
				ChangedHistoryAttribute change;

				change.entityId = entityId;
				change.instanceId = instanceId;
				change.attributeId = attributeId;
				change.segmentsAdded = 1;
				change.effectiveIntervals.push_back({effectiveStart, effectiveEnd});

				changedAttributeIndexByKey[key] = changedAttributes.size();
				changedAttributes.push_back(change);

				return;
			}

			changedAttributes[it->second].segmentsAdded++;
			changedAttributes[it->second].effectiveIntervals.push_back({effectiveStart, effectiveEnd});
		};

		auto recordChangedEntityExistence = [&](const std::string& entityId,
		                                        const std::string& instanceId,
		                                        const std::string& effectiveStart,
		                                        const std::string& effectiveEnd) {
			const std::string key = RuleSet::EntityExistenceHistoryDependencyKey(entityId, instanceId);
			auto it = changedEntityExistenceIndexByKey.find(key);

			if(it == changedEntityExistenceIndexByKey.end())
			{
				ChangedEntityExistenceHistory change;

				change.entityId = entityId;
				change.instanceId = instanceId;
				change.segmentsAdded = 1;
				change.effectiveIntervals.push_back({effectiveStart, effectiveEnd});

				changedEntityExistenceIndexByKey[key] = changedEntityExistence.size();
				changedEntityExistence.push_back(change);

				return;
			}

			changedEntityExistence[it->second].segmentsAdded++;
			changedEntityExistence[it->second].effectiveIntervals.push_back({effectiveStart, effectiveEnd});
		};

		auto recordChangedRelationshipLink = [&](const RuleSet::RelationshipLinkHistorySegment& segment) {
			const std::string key = RuleSet::RelationshipLinkHistoryDependencyKey(segment.relationshipId, segment.sourceInstanceId, segment.targetInstanceId);
			auto it = changedRelationshipLinkIndexByKey.find(key);

			if(it == changedRelationshipLinkIndexByKey.end())
			{
				ChangedRelationshipLinkHistory change;

				change.relationshipId = segment.relationshipId;
				change.sourceEntityId = segment.sourceEntityId;
				change.sourceInstanceId = segment.sourceInstanceId;
				change.targetEntityId = segment.targetEntityId;
				change.targetInstanceId = segment.targetInstanceId;
				change.segmentsAdded = 1;
				change.effectiveIntervals.push_back({segment.effectiveStart, segment.effectiveEnd});

				changedRelationshipLinkIndexByKey[key] = changedRelationshipLinks.size();
				changedRelationshipLinks.push_back(change);

				return;
			}

			changedRelationshipLinks[it->second].segmentsAdded++;
			changedRelationshipLinks[it->second].effectiveIntervals.push_back({segment.effectiveStart, segment.effectiveEnd});
		};

		auto recordChangedRelationshipCoverage = [&](const RuleSet::RelationshipCoverageHistorySegment& segment, const std::string& sourceEntityId) {
			const std::string key = RuleSet::RelationshipCoverageHistoryDependencyKey(segment.relationshipId, segment.sourceInstanceId);
			auto it = changedRelationshipCoverageIndexByKey.find(key);

			if(it == changedRelationshipCoverageIndexByKey.end())
			{
				ChangedRelationshipCoverageHistory change;

				change.relationshipId = segment.relationshipId;
				change.sourceEntityId = sourceEntityId;
				change.sourceInstanceId = segment.sourceInstanceId;
				change.segmentsAdded = 1;
				change.effectiveIntervals.push_back({segment.effectiveStart, segment.effectiveEnd});

				changedRelationshipCoverageIndexByKey[key] = changedRelationshipCoverage.size();
				changedRelationshipCoverage.push_back(change);

				return;
			}

			changedRelationshipCoverage[it->second].segmentsAdded++;
			changedRelationshipCoverage[it->second].effectiveIntervals.push_back({segment.effectiveStart, segment.effectiveEnd});
		};

		auto appendSegment = [&](const std::string& entityId,
		                         const std::string& instanceId,
		                         const std::string& attributeId,
		                         const json& segmentJson) {
			if(entityId.empty() || instanceId.empty() || attributeId.empty())
			{
				errorStatus = 400;
				errorCode = "history_target_missing";
				errorMessage = "History mutation requires entityId, instanceId, and attributeId.";

				return false;
			}

			std::shared_ptr<Entity> entity = find_ruleset_entity(s.ruleset, entityId);

			if(entity == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "History mutation entity was not found.";

				return false;
			}

			std::shared_ptr<Instance> instance = entity->GetInstanceById(instanceId);

			if(instance == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "History mutation instance was not found.";

				return false;
			}

			std::shared_ptr<Attribute> attribute = find_instance_attribute(instance, attributeId);

			if(attribute == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "History mutation attribute was not found.";

				return false;
			}

			RuleSet::AttributeHistorySegment segment;

			segment.entityId = entityId;
			segment.instanceId = instanceId;
			segment.attributeId = attributeId;
			segment.effectiveStart = get_json_scalar_string(segmentJson, "effectiveStart");
			segment.effectiveEnd = segmentJson.contains("effectiveEnd") && segmentJson.at("effectiveEnd").is_null()
				? ""
				: get_json_scalar_string(segmentJson, "effectiveEnd");
			segment.value = get_json_scalar_string(segmentJson, "value");
			segment.valueType = parse_history_value_type(first_json_scalar_string(segmentJson, {"valueType", "type"}), static_cast<Attribute::AttributeType>(attribute->GetType()));
			segment.knowledgeState = parse_history_knownness(first_json_scalar_string(segmentJson, {"knownness", "knowledgeState"}));
			segment.source = first_json_scalar_string(segmentJson, {"source"});
			segment.transactionStart = first_json_scalar_string(segmentJson, {"transactionStart", "asOfTransactionTime"});
			segment.transactionEnd = get_json_scalar_string(segmentJson, "transactionEnd");
			segment.assertedAt = get_json_scalar_string(segmentJson, "assertedAt");
			segment.evaluationTime = get_json_scalar_string(segmentJson, "evaluationTime");
			segment.modelTimeZone = get_json_scalar_string(segmentJson, "modelTimeZone");
			segment.provenance = get_json_scalar_string(segmentJson, "provenance");

			if(segment.effectiveStart.empty())
			{
				errorStatus = 400;
				errorCode = "history_segment_invalid";
				errorMessage = "History segment requires effectiveStart.";

				return false;
			}

			if(segment.source.empty())
			{
				segment.source = rootSource;
			}

			if(segment.transactionStart.empty())
			{
				segment.transactionStart = rootTransactionStart;
			}

			if(segment.transactionEnd.empty())
			{
				segment.transactionEnd = rootTransactionEnd;
			}

			if(segment.assertedAt.empty())
			{
				segment.assertedAt = rootAssertedAt;
			}

			if(segment.evaluationTime.empty())
			{
				segment.evaluationTime = rootEvaluationTime;
			}

			if(segment.modelTimeZone.empty())
			{
				segment.modelTimeZone = rootModelTimeZone;
			}

			if(!RuleSet::IsValidTransactionWindow(segment.transactionStart, segment.transactionEnd))
			{
				errorStatus = 400;
				errorCode = "history_transaction_time_invalid";
				errorMessage = "History segment transactionStart/transactionEnd must be valid ISO date or date-time values with transactionEnd >= transactionStart.";

				return false;
			}

			s.ruleset.AddAttributeHistorySegment(segment);
			recordChangedAttribute(entityId, instanceId, attributeId, segment.effectiveStart, segment.effectiveEnd);
			segmentsAdded++;
			attributeSegmentsAdded++;

			return true;
		};

		auto appendSegments = [&](const std::string& entityId,
		                          const std::string& instanceId,
		                          const std::string& attributeId,
		                          const json& segmentsJson) {
			const json* arrayJson = &segmentsJson;

			if(arrayJson->is_object() && arrayJson->contains("segments") && arrayJson->at("segments").is_array())
			{
				arrayJson = &arrayJson->at("segments");
			}

			if(!arrayJson->is_array())
			{
				return appendSegment(entityId, instanceId, attributeId, *arrayJson);
			}

			if(arrayJson->empty())
			{
				errorStatus = 400;
				errorCode = "history_segments_empty";
				errorMessage = "History mutation contains no segments.";

				return false;
			}

			for(const json& segmentJson : *arrayJson)
			{
				if(!segmentJson.is_object())
				{
					errorStatus = 400;
					errorCode = "history_segment_invalid";
					errorMessage = "History segment must be a JSON object.";

					return false;
				}

				if(!appendSegment(entityId, instanceId, attributeId, segmentJson))
				{
					return false;
				}
			}

			return true;
		};

		auto appendExistenceSegment = [&](const std::string& entityId,
		                                  const std::string& instanceId,
		                                  const json& segmentJson) {
			if(entityId.empty() || instanceId.empty())
			{
				errorStatus = 400;
				errorCode = "history_target_missing";
				errorMessage = "Entity existence history mutation requires entityId and instanceId.";

				return false;
			}

			std::shared_ptr<Entity> entity = find_ruleset_entity(s.ruleset, entityId);

			if(entity == nullptr || entity->GetInstanceById(instanceId) == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "Entity existence history mutation target was not found.";

				return false;
			}

			RuleSet::EntityExistenceHistorySegment segment;

			segment.entityId = entityId;
			segment.instanceId = instanceId;
			segment.effectiveStart = get_json_scalar_string(segmentJson, "effectiveStart");
			segment.effectiveEnd = segmentJson.contains("effectiveEnd") && segmentJson.at("effectiveEnd").is_null()
				? ""
				: get_json_scalar_string(segmentJson, "effectiveEnd");
			segment.knowledgeState = parse_history_knownness(first_json_scalar_string(segmentJson, {"knownness", "knowledgeState"}));
			segment.source = first_json_scalar_string(segmentJson, {"source"});
			segment.transactionStart = first_json_scalar_string(segmentJson, {"transactionStart", "asOfTransactionTime"});
			segment.transactionEnd = get_json_scalar_string(segmentJson, "transactionEnd");
			segment.assertedAt = get_json_scalar_string(segmentJson, "assertedAt");
			segment.evaluationTime = get_json_scalar_string(segmentJson, "evaluationTime");
			segment.modelTimeZone = get_json_scalar_string(segmentJson, "modelTimeZone");
			segment.provenance = get_json_scalar_string(segmentJson, "provenance");

			if(segment.effectiveStart.empty())
			{
				errorStatus = 400;
				errorCode = "history_segment_invalid";
				errorMessage = "Entity existence history segment requires effectiveStart.";

				return false;
			}

			if(segment.source.empty())
			{
				segment.source = rootSource;
			}

			if(segment.transactionStart.empty())
			{
				segment.transactionStart = rootTransactionStart;
			}

			if(segment.transactionEnd.empty())
			{
				segment.transactionEnd = rootTransactionEnd;
			}

			if(segment.assertedAt.empty())
			{
				segment.assertedAt = rootAssertedAt;
			}

			if(segment.evaluationTime.empty())
			{
				segment.evaluationTime = rootEvaluationTime;
			}

			if(segment.modelTimeZone.empty())
			{
				segment.modelTimeZone = rootModelTimeZone;
			}

			s.ruleset.AddEntityExistenceHistorySegment(segment);
			recordChangedEntityExistence(entityId, instanceId, segment.effectiveStart, segment.effectiveEnd);
			segmentsAdded++;
			entityExistenceSegmentsAdded++;

			return true;
		};

		auto appendExistenceSegments = [&](const std::string& entityId,
		                                   const std::string& instanceId,
		                                   const json& segmentsJson) {
			const json* arrayJson = &segmentsJson;

			if(arrayJson->is_object() && arrayJson->contains("segments") && arrayJson->at("segments").is_array())
			{
				arrayJson = &arrayJson->at("segments");
			}

			if(!arrayJson->is_array())
			{
				return appendExistenceSegment(entityId, instanceId, *arrayJson);
			}

			if(arrayJson->empty())
			{
				errorStatus = 400;
				errorCode = "history_segments_empty";
				errorMessage = "Entity existence history mutation contains no segments.";

				return false;
			}

			for(const json& segmentJson : *arrayJson)
			{
				if(!segmentJson.is_object())
				{
					errorStatus = 400;
					errorCode = "history_segment_invalid";
					errorMessage = "Entity existence history segment must be a JSON object.";

					return false;
				}

				if(!appendExistenceSegment(entityId, instanceId, segmentJson))
				{
					return false;
				}
			}

			return true;
		};

		auto appendRelationshipLinkSegment = [&](const std::string& relationshipId,
		                                         const std::string& parentSourceInstanceId,
		                                         const json& linkJson) {
			const std::string sourceInstanceId = first_json_scalar_string(linkJson, {"sourceInstanceId", "fromInstanceId"}).empty()
				? parentSourceInstanceId
				: first_json_scalar_string(linkJson, {"sourceInstanceId", "fromInstanceId"});
			const std::string targetInstanceId = first_json_scalar_string(linkJson, {"targetInstanceId", "toInstanceId", "instanceId"});

			if(relationshipId.empty() || sourceInstanceId.empty() || targetInstanceId.empty())
			{
				errorStatus = 400;
				errorCode = "history_target_missing";
				errorMessage = "Relationship link history mutation requires relationshipId, sourceInstanceId, and targetInstanceId.";

				return false;
			}

			std::shared_ptr<Relationship> relationship = find_ruleset_relationship(s.ruleset, relationshipId);

			if(relationship == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "Relationship link history mutation relationship was not found.";

				return false;
			}

			std::shared_ptr<Entity> sourceEntity = find_ruleset_entity(s.ruleset, relationship->sourceEntity);
			std::shared_ptr<Entity> targetEntity = find_ruleset_entity(s.ruleset, relationship->targetEntity);

			if(sourceEntity == nullptr || targetEntity == nullptr || sourceEntity->GetInstanceById(sourceInstanceId) == nullptr || targetEntity->GetInstanceById(targetInstanceId) == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "Relationship link history mutation source or target instance was not found.";

				return false;
			}

			RuleSet::RelationshipLinkHistorySegment segment;

			segment.relationshipId = relationshipId;
			segment.sourceEntityId = relationship->sourceEntity;
			segment.sourceInstanceId = sourceInstanceId;
			segment.targetEntityId = relationship->targetEntity;
			segment.targetInstanceId = targetInstanceId;
			segment.effectiveStart = get_json_scalar_string(linkJson, "effectiveStart");
			segment.effectiveEnd = linkJson.contains("effectiveEnd") && linkJson.at("effectiveEnd").is_null()
				? ""
				: get_json_scalar_string(linkJson, "effectiveEnd");
			segment.knowledgeState = parse_history_knownness(first_json_scalar_string(linkJson, {"knownness", "knowledgeState"}));
			segment.source = first_json_scalar_string(linkJson, {"source"});
			segment.transactionStart = first_json_scalar_string(linkJson, {"transactionStart", "asOfTransactionTime"});
			segment.transactionEnd = get_json_scalar_string(linkJson, "transactionEnd");
			segment.assertedAt = get_json_scalar_string(linkJson, "assertedAt");
			segment.evaluationTime = get_json_scalar_string(linkJson, "evaluationTime");
			segment.modelTimeZone = get_json_scalar_string(linkJson, "modelTimeZone");
			segment.provenance = get_json_scalar_string(linkJson, "provenance");

			if(segment.effectiveStart.empty())
			{
				errorStatus = 400;
				errorCode = "history_segment_invalid";
				errorMessage = "Relationship link history segment requires effectiveStart.";

				return false;
			}

			if(segment.source.empty())
			{
				segment.source = rootSource;
			}

			if(segment.transactionStart.empty())
			{
				segment.transactionStart = rootTransactionStart;
			}

			if(segment.transactionEnd.empty())
			{
				segment.transactionEnd = rootTransactionEnd;
			}

			if(segment.assertedAt.empty())
			{
				segment.assertedAt = rootAssertedAt;
			}

			if(segment.evaluationTime.empty())
			{
				segment.evaluationTime = rootEvaluationTime;
			}

			if(segment.modelTimeZone.empty())
			{
				segment.modelTimeZone = rootModelTimeZone;
			}

			s.ruleset.AddRelationshipLinkHistorySegment(segment);
			recordChangedRelationshipLink(segment);
			segmentsAdded++;
			relationshipLinkSegmentsAdded++;

			return true;
		};

		auto appendRelationshipCoverageSegment = [&](const std::string& relationshipId,
		                                             const std::string& parentSourceInstanceId,
		                                             const json& segmentJson) {
			const std::string sourceInstanceId = first_json_scalar_string(segmentJson, {"sourceInstanceId", "fromInstanceId"}).empty()
				? parentSourceInstanceId
				: first_json_scalar_string(segmentJson, {"sourceInstanceId", "fromInstanceId"});

			if(relationshipId.empty() || sourceInstanceId.empty())
			{
				errorStatus = 400;
				errorCode = "history_target_missing";
				errorMessage = "Relationship coverage history mutation requires relationshipId and sourceInstanceId.";

				return false;
			}

			std::shared_ptr<Relationship> relationship = find_ruleset_relationship(s.ruleset, relationshipId);

			if(relationship == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "Relationship coverage history mutation relationship was not found.";

				return false;
			}

			std::shared_ptr<Entity> sourceEntity = find_ruleset_entity(s.ruleset, relationship->sourceEntity);

			if(sourceEntity == nullptr || sourceEntity->GetInstanceById(sourceInstanceId) == nullptr)
			{
				errorStatus = 404;
				errorCode = "history_target_not_found";
				errorMessage = "Relationship coverage history mutation source instance was not found.";

				return false;
			}

			RuleSet::RelationshipCoverageHistorySegment segment;

			segment.relationshipId = relationshipId;
			segment.sourceInstanceId = sourceInstanceId;
			segment.effectiveStart = get_json_scalar_string(segmentJson, "effectiveStart");
			segment.effectiveEnd = segmentJson.contains("effectiveEnd") && segmentJson.at("effectiveEnd").is_null()
				? ""
				: get_json_scalar_string(segmentJson, "effectiveEnd");
			segment.knowledgeState = parse_history_knownness(first_json_scalar_string(segmentJson, {"knownness", "knowledgeState"}));
			segment.source = first_json_scalar_string(segmentJson, {"source"});
			segment.transactionStart = first_json_scalar_string(segmentJson, {"transactionStart", "asOfTransactionTime"});
			segment.transactionEnd = get_json_scalar_string(segmentJson, "transactionEnd");
			segment.assertedAt = get_json_scalar_string(segmentJson, "assertedAt");
			segment.evaluationTime = get_json_scalar_string(segmentJson, "evaluationTime");
			segment.modelTimeZone = get_json_scalar_string(segmentJson, "modelTimeZone");
			segment.provenance = get_json_scalar_string(segmentJson, "provenance");

			if(segment.effectiveStart.empty())
			{
				errorStatus = 400;
				errorCode = "history_segment_invalid";
				errorMessage = "Relationship coverage history segment requires effectiveStart.";

				return false;
			}

			if(segment.source.empty())
			{
				segment.source = rootSource;
			}

			if(segment.transactionStart.empty())
			{
				segment.transactionStart = rootTransactionStart;
			}

			if(segment.transactionEnd.empty())
			{
				segment.transactionEnd = rootTransactionEnd;
			}

			if(segment.assertedAt.empty())
			{
				segment.assertedAt = rootAssertedAt;
			}

			if(segment.evaluationTime.empty())
			{
				segment.evaluationTime = rootEvaluationTime;
			}

			if(segment.modelTimeZone.empty())
			{
				segment.modelTimeZone = rootModelTimeZone;
			}

			s.ruleset.AddRelationshipCoverageHistorySegment(segment);
			recordChangedRelationshipCoverage(segment, relationship->sourceEntity);
			segmentsAdded++;
			relationshipCoverageSegmentsAdded++;

			return true;
		};

		auto appendRelationshipHistory = [&](const json& relationshipJson) {
			const std::string relationshipId = get_json_scalar_string(relationshipJson, "relationshipId");
			const std::string sourceInstanceId = first_json_scalar_string(relationshipJson, {"sourceInstanceId", "fromInstanceId"});
			bool handledCoverage = false;

			if(relationshipJson.contains("coverage"))
			{
				const json* coverageJson = &relationshipJson.at("coverage");

				if(coverageJson->is_object() && coverageJson->contains("segments") && coverageJson->at("segments").is_array())
				{
					coverageJson = &coverageJson->at("segments");
				}

				if(!coverageJson->is_array())
				{
					if(!coverageJson->is_object())
					{
						errorStatus = 400;
						errorCode = "history_coverage_invalid";
						errorMessage = "Relationship coverage history mutation coverage payload must be an array or object.";

						return false;
					}

					if(!appendRelationshipCoverageSegment(relationshipId, sourceInstanceId, *coverageJson))
					{
						return false;
					}

					handledCoverage = true;
				}
				else
				{
					if(coverageJson->empty())
					{
						errorStatus = 400;
						errorCode = "history_coverage_empty";
						errorMessage = "Relationship coverage history mutation contains no coverage segments.";

						return false;
					}

					for(const json& segmentJson : *coverageJson)
					{
						if(!segmentJson.is_object())
						{
							errorStatus = 400;
							errorCode = "history_segment_invalid";
							errorMessage = "Relationship coverage history segment must be a JSON object.";

							return false;
						}

						if(!appendRelationshipCoverageSegment(relationshipId, sourceInstanceId, segmentJson))
						{
							return false;
						}
					}

					handledCoverage = true;
				}
			}

			if(relationshipJson.contains("links"))
			{
				const json& linksJson = relationshipJson.at("links");

				if(!linksJson.is_array())
				{
					errorStatus = 400;
					errorCode = "history_links_invalid";
					errorMessage = "Relationship link history mutation links payload must be an array.";

					return false;
				}

				if(linksJson.empty())
				{
					if(handledCoverage)
					{
						return true;
					}

					errorStatus = 400;
					errorCode = "history_links_empty";
					errorMessage = "Relationship link history mutation contains no links.";

					return false;
				}

				for(const json& linkJson : linksJson)
				{
					if(!linkJson.is_object())
					{
						errorStatus = 400;
						errorCode = "history_segment_invalid";
						errorMessage = "Relationship link history segment must be a JSON object.";

						return false;
					}

					if(!appendRelationshipLinkSegment(relationshipId, sourceInstanceId, linkJson))
					{
						return false;
					}
				}

				return true;
			}

			if(handledCoverage)
			{
				return true;
			}

			return appendRelationshipLinkSegment(relationshipId, sourceInstanceId, relationshipJson);
		};

		bool mutationAccepted = true;
		bool handledStructuredPayload = false;

		if(j.contains("instances"))
		{
			handledStructuredPayload = true;

			if(!j.at("instances").is_array())
			{
				errorStatus = 400;
				errorCode = "history_instances_invalid";
				errorMessage = "History instances payload must be an array.";
				mutationAccepted = false;
			}
			else
			{
				for(const json& instanceJson : j.at("instances"))
				{
					if(!instanceJson.is_object())
					{
						errorStatus = 400;
						errorCode = "history_instance_invalid";
						errorMessage = "History instance item must be a JSON object.";
						mutationAccepted = false;

						break;
					}

					const std::string entityId = get_json_scalar_string(instanceJson, "entityId");
					const std::string instanceId = get_json_scalar_string(instanceJson, "instanceId");

					if(instanceJson.contains("existence"))
					{
						if(!appendExistenceSegments(entityId, instanceId, instanceJson.at("existence")))
						{
							mutationAccepted = false;

							break;
						}
					}

					if(instanceJson.contains("attributes"))
					{
						if(!instanceJson.at("attributes").is_object())
						{
							errorStatus = 400;
							errorCode = "history_attributes_invalid";
							errorMessage = "History instance attributes payload must be an object.";
							mutationAccepted = false;

							break;
						}

						for(auto attributeIt = instanceJson.at("attributes").begin(); attributeIt != instanceJson.at("attributes").end(); ++attributeIt)
						{
							if(!appendSegments(entityId, instanceId, attributeIt.key(), attributeIt.value()))
							{
								mutationAccepted = false;

								break;
							}
						}
					}

					if(!mutationAccepted)
					{
						break;
					}
				}
			}
		}

		if(mutationAccepted && j.contains("relationships"))
		{
			handledStructuredPayload = true;

			if(!j.at("relationships").is_array())
			{
				errorStatus = 400;
				errorCode = "history_relationships_invalid";
				errorMessage = "History relationships payload must be an array.";
				mutationAccepted = false;
			}
			else
			{
				for(const json& relationshipJson : j.at("relationships"))
				{
					if(!relationshipJson.is_object())
					{
						errorStatus = 400;
						errorCode = "history_relationship_invalid";
						errorMessage = "History relationship item must be a JSON object.";
						mutationAccepted = false;

						break;
					}

					if(!appendRelationshipHistory(relationshipJson))
					{
						mutationAccepted = false;

						break;
					}
				}
			}
		}

		if(mutationAccepted && !handledStructuredPayload)
		{
			const std::string entityId = get_json_scalar_string(j, "entityId");
			const std::string instanceId = get_json_scalar_string(j, "instanceId");
			const std::string attributeId = get_json_scalar_string(j, "attributeId");

			if(j.contains("links") || j.contains("coverage") || !get_json_scalar_string(j, "relationshipId").empty())
			{
				mutationAccepted = appendRelationshipHistory(j);
			}
			else if(j.contains("existence"))
			{
				mutationAccepted = appendExistenceSegments(entityId, instanceId, j.at("existence"));
			}
			else if(j.contains("segments"))
			{
				mutationAccepted = appendSegments(entityId, instanceId, attributeId, j.at("segments"));
			}
			else
			{
				mutationAccepted = appendSegment(entityId, instanceId, attributeId, j);
			}
		}

		if(!mutationAccepted)
		{
			rollbackHistoryMutation();
			res.status = errorStatus;
			res.set_content(make_error(errorCode, errorMessage).dump(2), "application/json");

			return;
		}

		if(s.ruleset.sessionModelRules != nullptr && !s.ruleset.sessionModelRules->empty())
		{
			SessionModelRuleEvaluator evaluator;

			for(const ChangedHistoryAttribute& change : changedAttributes)
			{
				MergeEvaluationStats(evaluationStats, evaluator.EvaluateChangedAttributeHistory(s.ruleset, change.entityId, change.instanceId, change.attributeId, change.effectiveIntervals));
			}

			for(const ChangedEntityExistenceHistory& change : changedEntityExistence)
			{
				MergeEvaluationStats(evaluationStats, evaluator.EvaluateChangedEntityExistenceHistory(s.ruleset, change.entityId, change.instanceId, change.effectiveIntervals));
			}

			for(const ChangedRelationshipLinkHistory& change : changedRelationshipLinks)
			{
				MergeEvaluationStats(evaluationStats,
				                     evaluator.EvaluateChangedRelationshipHistory(s.ruleset,
				                                                                  change.relationshipId,
				                                                                  change.sourceEntityId,
				                                                                  change.sourceInstanceId,
				                                                                  change.targetEntityId,
				                                                                  change.targetInstanceId,
				                                                                  change.effectiveIntervals));
			}

			for(const ChangedRelationshipCoverageHistory& change : changedRelationshipCoverage)
			{
				MergeEvaluationStats(evaluationStats,
				                     evaluator.EvaluateChangedRelationshipHistory(s.ruleset,
				                                                                  change.relationshipId,
				                                                                  change.sourceEntityId,
				                                                                  change.sourceInstanceId,
				                                                                  "",
				                                                                  "",
				                                                                  change.effectiveIntervals));
			}

			hasSessionModelEvaluation = true;
		}
	}
	catch(const std::exception& e)
	{
		rollbackHistoryMutation();
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	json changedAttributeJson = json::array();

	for(const ChangedHistoryAttribute& change : changedAttributes)
	{
		changedAttributeJson.push_back({
			{"entityId", change.entityId},
			{"instanceId", change.instanceId},
			{"attributeId", change.attributeId},
			{"segmentsAdded", change.segmentsAdded}
		});
	}

	json changedEntityExistenceJson = json::array();

	for(const ChangedEntityExistenceHistory& change : changedEntityExistence)
	{
		changedEntityExistenceJson.push_back({
			{"entityId", change.entityId},
			{"instanceId", change.instanceId},
			{"segmentsAdded", change.segmentsAdded}
		});
	}

	json changedRelationshipLinkJson = json::array();

	for(const ChangedRelationshipLinkHistory& change : changedRelationshipLinks)
	{
		changedRelationshipLinkJson.push_back({
			{"relationshipId", change.relationshipId},
			{"sourceEntityId", change.sourceEntityId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"targetEntityId", change.targetEntityId},
			{"targetInstanceId", change.targetInstanceId},
			{"segmentsAdded", change.segmentsAdded}
		});
	}

	json changedRelationshipCoverageJson = json::array();

	for(const ChangedRelationshipCoverageHistory& change : changedRelationshipCoverage)
	{
		changedRelationshipCoverageJson.push_back({
			{"relationshipId", change.relationshipId},
			{"sourceEntityId", change.sourceEntityId},
			{"sourceInstanceId", change.sourceInstanceId},
			{"segmentsAdded", change.segmentsAdded}
		});
	}

	json out = {
		{"ok", true},
		{"historyMutation", {
			{"segmentsAdded", segmentsAdded},
			{"attributeSegmentsAdded", attributeSegmentsAdded},
			{"entityExistenceSegmentsAdded", entityExistenceSegmentsAdded},
			{"relationshipLinkSegmentsAdded", relationshipLinkSegmentsAdded},
			{"relationshipCoverageSegmentsAdded", relationshipCoverageSegmentsAdded},
			{"changedAttributes", changedAttributeJson},
			{"changedEntityExistence", changedEntityExistenceJson},
			{"changedRelationshipLinks", changedRelationshipLinkJson},
			{"changedRelationshipCoverage", changedRelationshipCoverageJson},
			{"attributeHistorySegmentCount", updated.ruleset.attributeHistorySegments != nullptr ? updated.ruleset.attributeHistorySegments->size() : 0},
			{"entityExistenceHistorySegmentCount", updated.ruleset.entityExistenceHistorySegments != nullptr ? updated.ruleset.entityExistenceHistorySegments->size() : 0},
			{"relationshipLinkHistorySegmentCount", updated.ruleset.relationshipLinkHistorySegments != nullptr ? updated.ruleset.relationshipLinkHistorySegments->size() : 0},
			{"relationshipCoverageHistorySegmentCount", updated.ruleset.relationshipCoverageHistorySegments != nullptr ? updated.ruleset.relationshipCoverageHistorySegments->size() : 0}
		}}
	};

	if(hasSessionModelEvaluation)
	{
		out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats);
	}

	res.set_content(out.dump(4), "application/json");
}

void RuleServer::HandleUpdateRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/update", req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	SessionModelRuleEvaluator::EvaluationStats evaluationStats;
	bool hasSessionModelEvaluation = false;
	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
	json updatedAttribute = nullptr;
	json refreshedScreen = nullptr;
	json uiState = nullptr;
	json screenLocation = nullptr;
	json actions = nullptr;

	try
	{
		auto j = json::parse(req.body);
		responseOptions = BuildEndpointResponseOptions(req, &j);

		const string elementId = first_json_scalar_string(j, {"elementId", "elementid", "id"});
		const string id = first_json_scalar_string(j, {"attributeId", "attributeid"});
		const string attributeId = !id.empty() ? id : elementId;
		const string screenId = first_json_scalar_string(j, {"screenId", "screenid"});
		const string screenEntityId = first_json_scalar_string(j, {"screenEntityId", "screenentityid"});
		const string screenInstanceId = first_json_scalar_string(j, {"screenInstanceId", "screeninstanceid"});
		const string instanceId = first_json_scalar_string(j, {"instanceId", "instanceid"});
		const string entityId = first_json_scalar_string(j, {"entityId", "entityid"});
		const string strValue = get_json_scalar_string(j, "value");
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(j, {"screenSequence", "sequence"});
		const string requestEntityId = !screenEntityId.empty() ? screenEntityId : entityId;
		const string requestInstanceId = !screenInstanceId.empty() ? screenInstanceId : instanceId;

		if(attributeId.empty() || entityId.empty() || instanceId.empty())
		{
			res.status = 400;

			res.set_content(make_error("missing_update_context", "Update requests require attributeId or elementId, entityId, and instanceId.").dump(2), "application/json");

			return;
		}

		if(!j.contains("value"))
		{
			res.status = 400;

			res.set_content(make_error("missing_update_value", "Update requests require an explicit value field.").dump(2), "application/json");

			return;
		}

		if(ScreenRequestIsStale(s, clientSequence, screenId, requestEntityId, requestInstanceId))
		{
			WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

			return;
		}

		screenLocation = BuildScreenLocationJson(screenId, requestEntityId, requestInstanceId, s.message_count + 1);
		actions = BuildDetachedScreenActionsJson();

		DataManager dm = DataManager();
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);

		shared_ptr<Instance> updatedInstance = dm.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId);

		if(updatedInstance == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("instance_not_found", "Update target instance not found.").dump(2), "application/json");

			return;
		}

		shared_ptr<Attribute> attribute = updatedInstance->GetAttributeByID(attributeId);

		if(attribute == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("attribute_not_found", "Update target attribute not found.").dump(2), "application/json");

			return;
		}

		dm.UpdateAttributeValue(s.ruleset.entities, attributeId, entityId, instanceId, strValue, screenId);

		if(s.ruleset.sessionModelRules != nullptr && !s.ruleset.sessionModelRules->empty())
		{
			SessionModelRuleEvaluator evaluator;

			evaluationStats = evaluator.EvaluateChangedAttribute(s.ruleset, entityId, instanceId, attributeId);
			hasSessionModelEvaluation = true;
		}

		updatedAttribute = attribute->CreateJSON();

		if(!screenId.empty())
		{
			shared_ptr<ScreenFile::Screen> screen = sessionScreenData.GetScreenByID(screenId);

			if(screen != nullptr)
			{
				const string refreshEntityId = !screenEntityId.empty() ? screenEntityId : screen->entityId;
				const string refreshInstanceId = !screenInstanceId.empty() ? screenInstanceId : instanceId;

				screen->instanceId = refreshInstanceId;

				screen = RefreshScreenForInstance(
					dm,
					s.ruleset,
					screen,
					refreshEntityId,
					refreshInstanceId,
					&sessionScreenData,
					&evaluationStats,
					&hasSessionModelEvaluation);

				refreshedScreen = BuildFrontendScreen(*screen);
				if(responseOptions.includeUiState)
				{
					uiState = BuildScreenUiState(*screen);
				}
				screenLocation = BuildScreenLocationJson(*screen, refreshEntityId, refreshInstanceId, s.message_count + 1);
				actions = BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData);
			}
		}
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	json out = json::object();
	out["ok"] = true;

	if(hasSessionModelEvaluation)
	{
		out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats);
	}

	if(!updatedAttribute.is_null())
	{
		out["attribute"] = updatedAttribute;
	}

	if(!refreshedScreen.is_null())
	{
		out["screen"] = refreshedScreen;
	}

	if(!uiState.is_null())
	{
		out["uiState"] = uiState;
	}

	if(screenLocation.is_null())
	{
		screenLocation = BuildCompleteScreenLocationJson(updated.message_count);
	}

	out["screenLocation"] = screenLocation;
	out["actions"] = actions.is_null() ? BuildDetachedScreenActionsJson() : actions;

	WriteEndpointJsonResponse(res, out, responseOptions);
}

void RuleServer::HandleSubmitScreenRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/submitscreen", req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	SessionModelRuleEvaluator::EvaluationStats evaluationStats;
	bool hasSessionModelEvaluation = false;
	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
	json out = json::object();
	ForgetPolicyPlan forgetPolicyPlan;
	ForgetPolicyApplyResult forgetPolicyResult;
	string forgetPolicyMode = "Skip";
	bool forgetPolicyApplied = false;

	try
	{
		auto requestJson = json::parse(req.body);
		responseOptions = BuildEndpointResponseOptions(req, &requestJson);
		const bool dataConnectionCommitRequested = DataConnectionCommitRequested(requestJson);

		std::string submitValidationCode;
		std::string submitValidationMessage;

		if(!ValidateSubmittedValueCollection(requestJson, submitValidationCode, submitValidationMessage))
		{
			res.status = 400;

			res.set_content(make_error(submitValidationCode, submitValidationMessage).dump(2), "application/json");

			return;
		}

		ScreenData::DataRoot submitted = RuleServer::ParseScreenDataJson(req.body);

		if(submitted.screenid.empty())
		{
			submitted.screenid = requestJson.value("screenId", std::string());
		}

		if(submitted.data.empty() && requestJson.contains("attributes") && requestJson["attributes"].is_array())
		{
			for(const json& item : requestJson["attributes"])
			{
				ScreenData::Data data;
				data.attributeId = first_json_scalar_string(item, {"attributeId", "attributeid", "id", "elementId", "elementid"});
				data.value = get_json_scalar_string(item, "value");
				submitted.data.push_back(std::move(data));
			}
		}

		if(submitted.data.empty() && requestJson.contains("values") && requestJson["values"].is_object())
		{
			for(auto it = requestJson["values"].begin(); it != requestJson["values"].end(); ++it)
			{
				ScreenData::Data data;
				data.attributeId = it.key();

				if(it.value().is_boolean())
				{
					data.value = it.value().get<bool>() ? "true" : "false";
				}
				else if(it.value().is_string())
				{
					data.value = it.value().get<std::string>();
				}
				else if(it.value().is_null())
				{
					data.value = "";
				}
				else
				{
					data.value = it.value().dump();
				}

				submitted.data.push_back(std::move(data));
			}
		}

		const string directTo = requestJson.value("directTo", std::string());
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(requestJson, {"screenSequence", "sequence"});

		if(submitted.screenid.empty() || submitted.entityId.empty() || submitted.instanceId.empty())
		{
			res.status = 400;

			res.set_content(make_error("missing_submit_context", "Submit screen requests require screenId/screenid, entityId, and instanceId.").dump(2), "application/json");

			return;
		}

		if(ScreenRequestIsStale(s, clientSequence, submitted.screenid, submitted.entityId, submitted.instanceId))
		{
			WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

			return;
		}

		DataManager dataManager;
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);

		shared_ptr<ScreenFile::Screen> currentScreen = sessionScreenData.GetScreenByID(submitted.screenid);

		if(currentScreen == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("screen_not_found", "Submitted screen not found.").dump(2), "application/json");

			return;
		}

		shared_ptr<Instance> currentInstance = dataManager.GetEntityInstanceById(s.ruleset.entities, submitted.entityId, submitted.instanceId);

		if(currentInstance == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("instance_not_found", "Submitted screen instance not found.").dump(2), "application/json");

			return;
		}

		std::vector<std::string> changedAttributeIds;
		std::unordered_set<std::string> changedAttributeIdSet;

		for(const ScreenData::Data& item : submitted.data)
		{
			if(item.attributeId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_submit_attribute", "Submitted values require an attributeId or equivalent element identifier.").dump(2), "application/json");

				return;
			}

			shared_ptr<Attribute> attribute = currentInstance->GetAttributeByID(item.attributeId);

			if(attribute == nullptr)
			{
				res.status = 400;

				res.set_content(make_error("attribute_not_found", "Submitted attribute not found on the screen entity instance: " + item.attributeId).dump(2), "application/json");

				return;
			}

			const std::string before = AttributeStateSignature(attribute);

			dataManager.UpdateAttributeValue(s.ruleset.entities, item.attributeId, submitted.entityId, submitted.instanceId, item.value, submitted.screenid);

			const std::string after = AttributeStateSignature(attribute);

			if(before != after && changedAttributeIdSet.insert(item.attributeId).second)
			{
				changedAttributeIds.push_back(item.attributeId);
			}
		}

		if(s.ruleset.sessionModelRules != nullptr && !s.ruleset.sessionModelRules->empty())
		{
			SessionModelRuleEvaluator evaluator;

			for(const std::string& attributeId : changedAttributeIds)
			{
				MergeEvaluationStats(evaluationStats, evaluator.EvaluateChangedAttribute(s.ruleset, submitted.entityId, submitted.instanceId, attributeId));
			}

			hasSessionModelEvaluation = true;
		}

		currentScreen = RefreshScreenForInstance(dataManager, s.ruleset, currentScreen, submitted.entityId, submitted.instanceId, &sessionScreenData);
		ValidateSubmittedScreen(*currentScreen, currentInstance, SubmittedValueMap(submitted));

		bool hasFieldErrors = ScreenHasElementErrors(*currentScreen);
		bool hasBlockingValidationErrors = false;
		json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);
		bool blocked = hasFieldErrors || hasBlockingValidationErrors;

		shared_ptr<ScreenFile::Screen> responseScreen = currentScreen;
		string responseEntityId = submitted.entityId;
		string responseInstanceId = submitted.instanceId;
		string action = "stay";
		NavigationTarget selectedFlowTarget;
		bool dataConnectionSubmitRequested = false;
		bool dataConnectionSubmitSucceeded = false;
		json dataConnectionSaveResult;
		forgetPolicyMode = ResolveSubmitForgetPolicyMode(sessionScreenData, s.ruleset, submitted.screenid);
		const auto forgetPolicyStartedAt = std::chrono::steady_clock::now();

		if(!blocked && forgetPolicyMode == "Apply")
		{
			forgetPolicyPlan = BuildForgetPolicyPlan(s.ruleset, *currentScreen, submitted.entityId, submitted.instanceId);
			forgetPolicyResult = ApplyForgetPolicyPlan(s.ruleset, forgetPolicyPlan);

			if(!forgetPolicyResult.succeeded)
			{
				res.status = 500;
				res.set_content(make_error("forget_policy_apply_failed", "Forget policy cleanup could not be applied atomically; the active session data was restored.").dump(2), "application/json");
				return;
			}

			forgetPolicyApplied = true;
			if(s.ruleset.sessionModelRules != nullptr && !s.ruleset.sessionModelRules->empty())
			{
				SessionModelRuleEvaluator evaluator;
				for(const ForgetAttributeAction& action : forgetPolicyResult.changedAttributes)
				{
					MergeEvaluationStats(evaluationStats, evaluator.EvaluateChangedAttribute(s.ruleset, action.entityId, action.instanceId, action.attributeId));
				}

				MergeEvaluationStats(evaluationStats, EvaluateRelationshipMutations(s.ruleset, forgetPolicyResult.changedRelationships));
				hasSessionModelEvaluation = true;
			}

			currentScreen = RefreshScreenForInstance(dataManager, s.ruleset, currentScreen, submitted.entityId, submitted.instanceId, &sessionScreenData);
			ValidateSubmittedScreen(*currentScreen, currentInstance, SubmittedValueMap(submitted));
			hasFieldErrors = ScreenHasElementErrors(*currentScreen);
			validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);
			blocked = hasFieldErrors || hasBlockingValidationErrors;
			responseScreen = currentScreen;
		}

		const auto forgetPolicyElapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - forgetPolicyStartedAt).count();
		std::cerr
			<< "forgetPolicyMetrics screenId=" << submitted.screenid
			<< " mode=" << forgetPolicyMode
			<< " applied=" << (forgetPolicyApplied ? 1 : 0)
			<< " attributes=" << forgetPolicyResult.forgottenAttributeCount
			<< " instances=" << forgetPolicyResult.forgottenInstanceCount
			<< " retained=" << (forgetPolicyResult.retainedAttributeCount + forgetPolicyResult.retainedSubtreeCount)
			<< " unknownVisibility=" << forgetPolicyPlan.unknownVisibilityCount
			<< " elapsedMicros=" << forgetPolicyElapsedMicros
			<< std::endl;

		if(!blocked)
		{
			shared_ptr<ScreenFile::Screen> targetScreen;
			string targetEntityId = submitted.entityId;
			string targetInstanceId = submitted.instanceId;

			if(!submitted.nextScreenId.empty())
			{
				targetScreen = dataManager.GetScreenByID(sessionScreenDataView, submitted.nextScreenId);
				targetEntityId = !submitted.nextEntityId.empty() ? submitted.nextEntityId : (targetScreen != nullptr ? targetScreen->entityId : submitted.entityId);

				if(targetScreen == nullptr)
				{
					res.status = 404;

					res.set_content(make_error("screen_not_found", "Requested next screen not found.").dump(2), "application/json");

					return;
				}

				targetInstanceId = !submitted.nextInstanceId.empty()
					? submitted.nextInstanceId
					: ResolveScreenInstanceId(dataManager, s.ruleset, *targetScreen, submitted.entityId, submitted.instanceId);
			}
			else if(!directTo.empty())
			{
				ScreenLocation redirect = s.ruleset.GetLatestScreenLocation(directTo);

				if(!redirect.screenId.empty())
				{
					targetScreen = dataManager.GetScreenByID(sessionScreenDataView, redirect.screenId);
					targetEntityId = redirect.entityId;
					targetInstanceId = redirect.instanceId;
				}
				else
				{
					targetScreen = dataManager.GetScreenByID(sessionScreenDataView, directTo);
					targetEntityId = targetScreen != nullptr ? targetScreen->entityId : submitted.entityId;
				}

				if(targetScreen == nullptr)
				{
					res.status = 404;

					res.set_content(make_error("screen_not_found", "Requested direct screen not found.").dump(2), "application/json");

					return;
				}

				if(targetInstanceId.empty())
				{
					targetInstanceId = ResolveScreenInstanceId(dataManager, s.ruleset, *targetScreen, submitted.entityId, submitted.instanceId);
				}
			}
			else
			{
				selectedFlowTarget = FindNextScreen(sessionScreenData,
				                                    s.ruleset,
				                                    dataManager,
				                                    submitted.screenid,
				                                    submitted.entityId,
				                                    submitted.instanceId);
				targetScreen = selectedFlowTarget.screen;

				if(targetScreen != nullptr)
				{
					targetEntityId = selectedFlowTarget.entityId;
					targetInstanceId = selectedFlowTarget.instanceId;
				}
			}

			if(selectedFlowTarget.usedFlow && IsDataSubmitTransitionAction(selectedFlowTarget.transitionAction))
			{
				dataConnectionSubmitRequested = true;
				const json savePlan = s.ruleset.BuildDataConnectionSavePlan().ToJSON();
				dataConnectionSaveResult = BuildDataConnectionSavePreviewJson(&s.ruleset, savePlan, true);
				dataConnectionSubmitSucceeded = DataConnectionSavePreviewCommittedSuccessfully(dataConnectionSaveResult);

				if(!dataConnectionSubmitSucceeded)
				{
					if(forgetPolicyResult.transaction != nullptr)
					{
						forgetPolicyResult.transaction->Rollback();
						forgetPolicyResult.transaction.reset();
						forgetPolicyResult.rolledBack = true;
						forgetPolicyApplied = false;
						currentScreen = RefreshScreenForInstance(dataManager, s.ruleset, currentScreen, submitted.entityId, submitted.instanceId, &sessionScreenData);
					}

					blocked = true;
					responseScreen = currentScreen;
					responseEntityId = submitted.entityId;
					responseInstanceId = submitted.instanceId;
					action = "stay";
				}
			}

			if(!blocked && targetScreen == nullptr)
			{
				responseScreen = nullptr;
				action = "complete";
				responseEntityId.clear();
				responseInstanceId.clear();
			}
			else if(!blocked)
			{
				responseScreen = RefreshScreenForInstance(dataManager, s.ruleset, targetScreen, targetEntityId, targetInstanceId, &sessionScreenData);
				responseEntityId = targetEntityId;
				responseInstanceId = targetInstanceId;
				action = (responseScreen != nullptr && responseScreen->id == submitted.screenid && responseInstanceId == submitted.instanceId) ? "stay" : "next";
			}
		}

		out["ok"] = true;
		out["navigation"] = {
			{"action", action},
			{"fromScreenId", submitted.screenid},
			{"toScreenId", responseScreen != nullptr ? responseScreen->id : std::string()},
			{"entityId", responseEntityId},
			{"instanceId", responseInstanceId},
			{"blocked", blocked}
		};
		if(!blocked && selectedFlowTarget.usedFlow && responseScreen != nullptr)
		{
			if(selectedFlowTarget.screen != nullptr && selectedFlowTarget.screen->id == responseScreen->id)
			{
				out["navigation"]["flow"] = {
					{"used", true},
					{"transitionId", selectedFlowTarget.transitionId},
					{"transitionLabel", selectedFlowTarget.transitionLabel},
					{"transitionAction", selectedFlowTarget.transitionAction}
				};
			}
		}
		if(dataConnectionSubmitRequested)
		{
			out["navigation"]["dataSubmit"] = {
				{"requested", true},
				{"succeeded", dataConnectionSubmitSucceeded}
			};
			out["dataConnectionSave"] = dataConnectionSaveResult;
		}
		out["validationEvents"] = validationEvents;
		out["forgetPolicy"] = ForgetPolicyResultToJson(forgetPolicyPlan, forgetPolicyResult, forgetPolicyApplied, forgetPolicyMode);

		if(hasSessionModelEvaluation)
		{
			out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats);
		}

		if(responseScreen != nullptr)
		{
			ScreenLocation location;
			location.screenId = responseScreen->id;
			location.entityId = responseEntityId;
			location.instanceId = responseInstanceId;
			s.ruleset.AddScreenLocation(location);

			out["screen"] = BuildFrontendScreen(*responseScreen);
			if(responseOptions.includeUiState)
			{
				out["uiState"] = BuildScreenUiState(*responseScreen);
			}
			out["screenLocation"] = BuildScreenLocationJson(*responseScreen, responseEntityId, responseInstanceId, s.message_count + 1);
			out["actions"] = BuildScreenActionsJson(s.ruleset, *responseScreen, sessionScreenData);
		}
		else
		{
			out["screenLocation"] = BuildCompleteScreenLocationJson(s.message_count + 1);
			out["actions"] = BuildCompleteActionsJson();

			const json savePlan = s.ruleset.BuildDataConnectionSavePlan().ToJSON();
			const json deletedInstanceDeletePlan = s.ruleset.BuildDataConnectionDeletedInstanceDeletePlan().ToJSON();

			if(!dataConnectionSubmitRequested
				&& (DataConnectionSavePlanHasContent(savePlan) || DataConnectionLoadPlanHasContent(deletedInstanceDeletePlan)))
			{
				out["dataConnectionSave"] = BuildDataConnectionSavePreviewJson(&s.ruleset, savePlan, dataConnectionCommitRequested);
			}
		}

		if(!blocked && selectedFlowTarget.usedFlow)
		{
			const ScreenFile::FlowTransition* committedTransition = FindFlowTransition(sessionScreenData, selectedFlowTarget.transitionId);
			if(committedTransition != nullptr && !committedTransition->emailTemplateId.empty())
			{
				const std::uint64_t committedSequence = s.message_count + 1;
				EmailCommandResolver::Result email = EmailCommandResolver::Resolve(
					s.ruleset,
					committedTransition->emailTemplateId,
					committedTransition->transitionId,
					token,
					committedSequence,
					submitted.entityId,
					submitted.instanceId);
				if(email.configured && !email.command.is_null())
				{
					s.emailCommandsBySequence[committedSequence] = email.command;
					TrimCommittedEmailCommands(s.emailCommandsBySequence, 16);
				}
			}
		}
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!store_session_runtime(token, s, s.message_count, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	if(forgetPolicyResult.transaction != nullptr)
	{
		forgetPolicyResult.transaction->Commit();
	}

	const auto committedEmail = updated.emailCommandsBySequence.find(updated.message_count);
	if(committedEmail != updated.emailCommandsBySequence.end())
	{
		out["emailCommands"] = json::array({committedEmail->second});
	}

	WriteEndpointJsonResponse(res, out, responseOptions);
}

void RuleServer::HandleStartSessionRequest(const httplib::Request& req, httplib::Response& res)
{
	HandleStartOrCurrentScreenRequest(req, res, "/startsession", false);
}

void RuleServer::HandleCurrentScreenRequest(const httplib::Request& req, httplib::Response& res)
{
	HandleStartOrCurrentScreenRequest(req, res, "/currentscreen", true);
}

void RuleServer::HandleStartOrCurrentScreenRequest(const httplib::Request& req, httplib::Response& res, const char* endpointName, bool currentScreenEndpoint)
{
	EndpointMetricsScope metrics(endpointName, req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);

	try
	{
		json requestJson = json::object();

		if(!req.body.empty())
		{
			requestJson = json::parse(req.body);
		}

		responseOptions = BuildEndpointResponseOptions(req, &requestJson);

		if(!requestJson.is_object())
		{
			res.status = 400;

			res.set_content(make_error("invalid_request_shape", "Start/current screen requests must be JSON objects when a body is supplied.").dump(2), "application/json");

			return;
		}

		const auto startPipelineStarted = Clock::now();

		const FormLaunchSelection formSelection = ParseFormLaunchSelection(requestJson);

		if(currentScreenEndpoint && formSelection.requested)
		{
			res.status = 400;

			res.set_content(make_error("form_selection_not_supported", "Form selection is only supported when starting a session.").dump(2), "application/json");

			return;
		}

		const bool reset = requestJson.value("reset", false);
		const bool resume = requestJson.value("resume", true) && !reset;
		const string requestedScreenId = first_json_scalar_string(requestJson, {"screenId", "screenid"});
		const string requestedEntityId = first_json_scalar_string(requestJson, {"entityId", "entityid"});
		const string requestedInstanceId = first_json_scalar_string(requestJson, {"instanceId", "instanceid"});
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(requestJson, {"screenSequence", "sequence"});
		const bool hasExplicitScreenContext = !requestedScreenId.empty() || !requestedEntityId.empty() || !requestedInstanceId.empty();
		const bool hasStartValues = requestJson.contains("startValues");

		if(hasStartValues && !requestJson.at("startValues").is_array())
		{
			res.status = 400;
			res.set_content(make_error("invalid_start_values", "startValues must be an array of attribute value objects.").dump(2), "application/json");
			return;
		}

		if(currentScreenEndpoint && hasStartValues)
		{
			res.status = 400;
			res.set_content(make_error("start_values_not_supported", "Start-supplied values can only be provided when starting a clean session.").dump(2), "application/json");
			return;
		}

		const bool sessionHadNavigation = !LatestScreenLocation(s.ruleset).screenId.empty();

		if(currentScreenEndpoint && (hasExplicitScreenContext || clientSequence.has_value()))
		{
			if(requestedScreenId.empty() || requestedEntityId.empty() || requestedInstanceId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_current_screen_context", "Current-screen target requests require screenId, entityId, and instanceId.").dump(2), "application/json");

				return;
			}

			if(ScreenRequestIsStale(s, clientSequence, requestedScreenId, requestedEntityId, requestedInstanceId))
			{
				WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

				return;
			}
		}

		bool selectedFormLoaded = false;

		if(formSelection.requested)
		{
			FormCatalogSettings settings = LoadFormCatalogSettings();
			SelectedFormSessionModelLoadResult selectedFormLoad = LoadSelectedFormSessionModelFromCatalog(settings, s, formSelection);

			if(!selectedFormLoad.succeeded)
			{
				res.status = selectedFormLoad.httpStatus;
				json error = make_error(selectedFormLoad.code, selectedFormLoad.message);

				if(!selectedFormLoad.diagnostics.empty())
				{
					error["diagnostics"] = selectedFormLoad.diagnostics;
				}

				res.set_content(error.dump(2), "application/json");

				return;
			}

			const SessionAuthenticationModeValidation modeValidation = ValidateSessionAuthenticationModeForForm(
				s,
				selectedFormLoad.selectedForm,
				selectedFormLoad.selectedForm.externalSecurity);
			if(WriteSessionAuthenticationModeFailure(res, modeValidation))
			{
				return;
			}

			const RuleSet::SessionContext previousContext = s.ruleset.sessionContext;
			s.selectedForm = selectedFormLoad.selectedForm;
			s.screenData = selectedFormLoad.screenData;
			s.ruleset = BuildCleanRuleSetForSession(s, previousContext, false);
			s.message_count = 0;
			selectedFormLoaded = true;
		}

		if(reset && !selectedFormLoaded)
		{
			const RuleSet::SessionContext previousContext = s.ruleset.sessionContext;
			s.ruleset = BuildCleanRuleSetForSession(s, previousContext, false);
			s.message_count = 0;
		}

		if(!selectedFormLoaded)
		{
			const SessionAuthenticationModeValidation modeValidation = ValidateSessionAuthenticationModeForCurrentRuntime(s);
			if(WriteSessionAuthenticationModeFailure(res, modeValidation))
			{
				return;
			}
		}

		bool cleanStart = !currentScreenEndpoint && (selectedFormLoaded || reset || !sessionHadNavigation);

		if(hasStartValues && (hasExplicitScreenContext || !cleanStart))
		{
			res.status = 409;
			res.set_content(make_error("start_values_resume_not_allowed", "Start-supplied values cannot be applied while resuming an existing session or targeting an existing screen.").dump(2), "application/json");
			return;
		}

		if(hasStartValues && !selectedFormLoaded && !reset && !sessionHadNavigation)
		{
			// try_get_session returns a value object whose RuleSet owns shared runtime
			// collections. Clone a completely clean candidate before applying any
			// supplied values so a rejected request cannot leak partial mutations.
			const RuleSet::SessionContext previousContext = s.ruleset.sessionContext;
			s.ruleset = BuildCleanRuleSetForSession(s, previousContext, false);
			s.message_count = 0;
			cleanStart = true;
		}

		int startInputAppliedCount = 0;

		if(cleanStart && s.ruleset.startInputs != nullptr && (!s.ruleset.startInputs->empty() || hasStartValues))
		{
			const json suppliedValues = hasStartValues ? requestJson.at("startValues") : json::array();
			std::unordered_map<std::string, const RuleSet::StartInputDefinition*> definitionsByAttributeId;
			std::unordered_map<std::string, json> valuesByAttributeId;

			for(const RuleSet::StartInputDefinition& input : *s.ruleset.startInputs)
			{
				definitionsByAttributeId[input.attributeId] = &input;
			}

			for(const json& item : suppliedValues)
			{
				if(!item.is_object())
				{
					res.status = 400;
					res.set_content(make_error("invalid_start_value", "Each startValues entry must be an object.").dump(2), "application/json");
					return;
				}

				const std::string attributeId = TrimAscii(first_json_scalar_string(item, {"attributeId", "attributeid", "id"}));

				if(attributeId.empty())
				{
					res.status = 400;
					res.set_content(make_error("missing_start_attribute", "Each startValues entry requires a non-empty attributeId.").dump(2), "application/json");
					return;
				}

				if(valuesByAttributeId.find(attributeId) != valuesByAttributeId.end())
				{
					res.status = 400;
					json error = make_error("duplicate_start_attribute", "A start-supplied attribute can only be provided once.");
					error["attributeId"] = attributeId;
					res.set_content(error.dump(2), "application/json");
					return;
				}

				auto definition = definitionsByAttributeId.find(attributeId);

				if(definition == definitionsByAttributeId.end())
				{
					res.status = 400;
					json error = make_error("start_attribute_not_declared", "The supplied attribute is not declared as a Root start input for this form.");
					error["attributeId"] = attributeId;
					res.set_content(error.dump(2), "application/json");
					return;
				}

				const std::string suppliedEntityId = TrimAscii(first_json_scalar_string(item, {"entityId", "entityid"}));

				if(!suppliedEntityId.empty() && suppliedEntityId != definition->second->entityId)
				{
					res.status = 400;
					json error = make_error("start_input_non_root", "The supplied start value does not target the declared Root entity.");
					error["attributeId"] = attributeId;
					res.set_content(error.dump(2), "application/json");
					return;
				}

				if(!item.contains("value") || !SubmitValueIsSupported(item.at("value")) || item.at("value").is_null())
				{
					res.status = 400;
					json error = make_error("invalid_start_value", "Each startValues entry requires a non-null scalar value.");
					error["attributeId"] = attributeId;
					res.set_content(error.dump(2), "application/json");
					return;
				}

				const std::string value = SubmitValueToString(item.at("value"));

				if(TrimAscii(value).empty())
				{
					res.status = 400;
					json error = make_error("start_input_required", "Start-supplied values cannot be blank.");
					error["attributeId"] = attributeId;
					res.set_content(error.dump(2), "application/json");
					return;
				}

				valuesByAttributeId.emplace(attributeId, item.at("value"));
			}

			json missingAttributeIds = json::array();

			for(const RuleSet::StartInputDefinition& input : *s.ruleset.startInputs)
			{
				if(input.required && valuesByAttributeId.find(input.attributeId) == valuesByAttributeId.end())
				{
					missingAttributeIds.push_back(input.attributeId);
				}
			}

			if(!missingAttributeIds.empty())
			{
				res.status = 400;
				json error = make_error("start_inputs_required", "All required start-supplied values must be provided before the session can start.");
				error["missingAttributeIds"] = std::move(missingAttributeIds);
				res.set_content(error.dump(2), "application/json");
				return;
			}

			DataManager startDataManager;

			for(const RuleSet::StartInputDefinition& input : *s.ruleset.startInputs)
			{
				auto suppliedValue = valuesByAttributeId.find(input.attributeId);

				if(suppliedValue == valuesByAttributeId.end())
				{
					continue;
				}

				const std::string rootInstanceId = startDataManager.GetFirstInstanceId(s.ruleset.entities, input.entityId);
				std::shared_ptr<Instance> rootInstance = startDataManager.GetEntityInstanceById(s.ruleset.entities, input.entityId, rootInstanceId);
				std::shared_ptr<Attribute> attribute = rootInstance != nullptr ? rootInstance->GetAttributeByID(input.attributeId) : nullptr;

				if(attribute == nullptr)
				{
					res.status = 500;
					json error = make_error("start_attribute_unavailable", "A declared start-supplied attribute is unavailable in the clean Root instance.");
					error["attributeId"] = input.attributeId;
					res.set_content(error.dump(2), "application/json");
					return;
				}

				if(!attribute->SetValueFromString(SubmitValueToString(suppliedValue->second), false))
				{
					res.status = 400;
					json error = make_error("start_input_type_invalid", "The supplied value is not valid for the declared start-input type.");
					error["attributeId"] = input.attributeId;
					error["attributeType"] = input.attributeType;
					res.set_content(error.dump(2), "application/json");
					return;
				}

				++startInputAppliedCount;
			}

		}

		bool startHydrationAttempted = false;
		bool startRootRecordFound = false;
		bool startHydrated = false;
		int startHydratedAttributeCount = 0;
		int startDescendantInstanceCount = 0;
		int startDescendantOperationCount = 0;

		if(cleanStart && startInputAppliedCount > 0)
		{
			const json loadPlan = s.ruleset.BuildDataConnectionLoadPlan().ToJSON();
			const json startLoadPlan = BuildStartRootLoadPlan(loadPlan, s.ruleset);

			if(DataConnectionLoadPlanTableCount(startLoadPlan) > 0)
			{
				startHydrationAttempted = true;
				json execution = BuildDataConnectionLoadPlanExecutionResult(startLoadPlan, 2);

				if(!execution.value("succeeded", false) || !execution.value("readExecuted", false))
				{
					res.status = 502;
					res.set_content(make_error("start_hydration_failed", "The Root record could not be loaded from its configured data connection.").dump(2), "application/json");
					return;
				}

				if(!execution.contains("operations") || !execution.at("operations").is_array())
				{
					res.status = 502;
					res.set_content(make_error("start_hydration_invalid_response", "The Root record load returned an invalid response.").dump(2), "application/json");
					return;
				}

				for(const json& operation : execution.at("operations"))
				{
					if(!operation.is_object() || !operation.contains("rows") || !operation.at("rows").is_array())
					{
						res.status = 502;
						res.set_content(make_error("start_hydration_invalid_response", "The Root record load returned an invalid operation response.").dump(2), "application/json");
						return;
					}

					const json& rows = operation.at("rows");

					if(rows.empty())
					{
						continue;
					}

					if(rows.size() > 1)
					{
						res.status = 409;
						res.set_content(make_error("start_record_ambiguous", "More than one Root record matched the supplied start values.").dump(2), "application/json");
						return;
					}

					startRootRecordFound = true;
				}

				if(startRootRecordFound)
				{
					std::unordered_set<std::string> startInputKeys;
				for(const RuleSet::StartInputDefinition& input : *s.ruleset.startInputs)
				{
					startInputKeys.insert(input.entityId + "|" + input.attributeId);
				}

				for(const json& operation : execution.at("operations"))
				{
					const json& rows = operation.at("rows");
					if(rows.empty())
					{
						continue;
					}

					const json& row = rows[0];
					if(!row.is_object() || !row.contains("values") || !row.at("values").is_array())
					{
						continue;
					}

					for(const json& returnedValue : row.at("values"))
					{
						if(!returnedValue.is_object() || !returnedValue.contains("targets") || !returnedValue.at("targets").is_array())
						{
							continue;
						}

						const json* rawValue = FindObjectValue(returnedValue, {"value", "Value"});
						const bool isNull = JsonBoolValue(returnedValue, {"isNull", "IsNull"}).value_or(rawValue == nullptr || rawValue->is_null());
						const std::string returnedText = DataConnectionHydrationValueToString(rawValue, isNull);

						for(const json& target : returnedValue.at("targets"))
						{
							if(!target.is_object())
							{
								continue;
							}

							const std::string targetEntityId = target.value("entityId", std::string());
							const std::string targetAttributeId = target.value("attributeId", std::string());

							if(startInputKeys.find(targetEntityId + "|" + targetAttributeId) == startInputKeys.end())
							{
								continue;
							}

							DataManager conflictDataManager;
							const std::string rootInstanceId = conflictDataManager.GetFirstInstanceId(s.ruleset.entities, targetEntityId);
							std::shared_ptr<Instance> rootInstance = conflictDataManager.GetEntityInstanceById(s.ruleset.entities, targetEntityId, rootInstanceId);
							std::shared_ptr<Attribute> startAttribute = rootInstance != nullptr ? rootInstance->GetAttributeByID(targetAttributeId) : nullptr;
							std::shared_ptr<Attribute> returnedAttribute = startAttribute != nullptr ? std::make_shared<Attribute>(*startAttribute) : nullptr;

							if(startAttribute == nullptr
								|| startAttribute->IsUnknown()
								|| isNull
								|| returnedAttribute == nullptr
								|| !returnedAttribute->SetValueFromString(returnedText, false)
								|| returnedAttribute->IsUnknown()
								|| returnedAttribute->GetStringValue() != startAttribute->GetStringValue())
							{
								res.status = 409;
								json error = make_error("start_record_conflict", "The loaded Root record does not match a supplied start value.");
								error["attributeId"] = targetAttributeId;
								res.set_content(error.dump(2), "application/json");
								return;
							}
						}
					}
				}

				ApplyDataConnectionLoadHydration(s.ruleset, execution, false);

				if(!execution.value("hydrated", false) || execution.value("hydrationRefused", false))
				{
					res.status = 502;
					res.set_content(make_error("start_hydration_refused", "The Root record was returned but could not be applied safely to the session.").dump(2), "application/json");
					return;
				}

				startHydrated = true;
				startHydratedAttributeCount = execution.value("hydratedAttributeCount", 0);

				struct StartHydrationTraversalItem
				{
					json identity;
					std::string entityId;
					std::string instanceId;
					int depth = 0;
				};

				constexpr int maxTraversalDepth = 8;
				constexpr int maxTraversalOperations = 128;
				constexpr int maxTraversalRows = 1000;
				std::vector<StartHydrationTraversalItem> traversalQueue;
				std::unordered_set<std::string> queuedInstances;
				int traversedRows = 0;
				DataManager traversalDataManager;

				for(std::size_t operationIndex = 0; operationIndex < execution.at("operations").size(); ++operationIndex)
				{
					const json& rootOperation = execution.at("operations")[operationIndex];
					const RuleSet::DataConnectionEntityMapping* rootMapping =
						FindDataConnectionEntityMappingForLoadPlanTable(s.ruleset, rootOperation);

					if(rootMapping == nullptr || !rootOperation.contains("rows") || !rootOperation.at("rows").is_array() || rootOperation.at("rows").empty())
					{
						continue;
					}

					const std::string rootInstanceId = traversalDataManager.GetFirstInstanceId(s.ruleset.entities, rootMapping->entityId);
					json rootIdentity = BuildTraversalRecordIdentity(
						s.ruleset,
						rootOperation,
						rootOperation.at("rows")[0],
						static_cast<int>(operationIndex),
						0,
						rootMapping->entityId,
						rootInstanceId);

					if(rootIdentity.value("hasPrimaryKey", false)
						&& queuedInstances.insert(rootMapping->entityId + "|" + rootInstanceId).second)
					{
						traversalQueue.push_back({std::move(rootIdentity), rootMapping->entityId, rootInstanceId, 0});
					}
				}

				for(std::size_t queueIndex = 0; queueIndex < traversalQueue.size(); ++queueIndex)
				{
					const StartHydrationTraversalItem parent = traversalQueue[queueIndex];
					json descendantPlan = BuildOwnedDescendantLoadPlan(loadPlan, parent.identity, s.ruleset, parent.entityId);

					if(DataConnectionLoadPlanTableCount(descendantPlan) == 0)
					{
						continue;
					}

					if(parent.depth >= maxTraversalDepth)
					{
						res.status = 409;
						res.set_content(make_error("start_hydration_depth_limit", "Owned descendant loading exceeded the maximum safe relationship depth.").dump(2), "application/json");
						return;
					}

					startDescendantOperationCount += static_cast<int>(DataConnectionLoadPlanTableCount(descendantPlan));
					if(startDescendantOperationCount > maxTraversalOperations)
					{
						res.status = 409;
						res.set_content(make_error("start_hydration_operation_limit", "Owned descendant loading exceeded the maximum safe operation count.").dump(2), "application/json");
						return;
					}

					json descendantExecution = BuildDataConnectionLoadPlanExecutionResult(descendantPlan, 501);

					if(!descendantExecution.value("succeeded", false)
						|| !descendantExecution.value("readExecuted", false)
						|| !descendantExecution.contains("operations")
						|| !descendantExecution.at("operations").is_array())
					{
						res.status = 502;
						res.set_content(make_error("start_descendant_hydration_failed", "Owned descendant records could not be loaded from their configured data connection.").dump(2), "application/json");
						return;
					}

					for(const json& descendantOperation : descendantExecution.at("operations"))
					{
						if(!descendantOperation.is_object() || !descendantOperation.contains("rows") || !descendantOperation.at("rows").is_array())
						{
							res.status = 502;
							res.set_content(make_error("start_descendant_hydration_invalid_response", "Owned descendant loading returned an invalid operation response.").dump(2), "application/json");
							return;
						}

						traversedRows += static_cast<int>(descendantOperation.at("rows").size());
					}

					if(traversedRows > maxTraversalRows)
					{
						res.status = 409;
						res.set_content(make_error("start_hydration_row_limit", "Owned descendant loading exceeded the maximum safe row count.").dump(2), "application/json");
						return;
					}

					ApplyDataConnectionLoadHydration(s.ruleset, descendantExecution, false);

					if(!descendantExecution.value("hydrated", false) || descendantExecution.value("hydrationRefused", false))
					{
						res.status = 502;
						res.set_content(make_error("start_descendant_hydration_refused", "Owned descendant records were returned but could not be applied safely to the session.").dump(2), "application/json");
						return;
					}

					startHydratedAttributeCount += descendantExecution.value("hydratedAttributeCount", 0);

					for(std::size_t operationIndex = 0; operationIndex < descendantExecution.at("operations").size(); ++operationIndex)
					{
						const json& descendantOperation = descendantExecution.at("operations")[operationIndex];
						const RuleSet::DataConnectionEntityMapping* childMapping =
							FindDataConnectionEntityMappingForLoadPlanTable(s.ruleset, descendantOperation);
						const RuleSet::DataConnectionRelationshipFieldMapping* relationshipMapping = childMapping != nullptr
							? FindOwnedTraversalRelationshipMapping(s.ruleset, *childMapping, descendantOperation, parent.entityId)
							: nullptr;

						if(childMapping == nullptr || relationshipMapping == nullptr)
						{
							res.status = 502;
							res.set_content(make_error("start_descendant_scope_invalid", "Owned descendant loading could not resolve a safe parent-child scope.").dump(2), "application/json");
							return;
						}

						const json& rows = descendantOperation.at("rows");
						startDescendantInstanceCount += static_cast<int>(rows.size());

						for(std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
						{
							json childIdentity = BuildTraversalRecordIdentity(
								s.ruleset,
								descendantOperation,
								rows[rowIndex],
								static_cast<int>(operationIndex),
								static_cast<int>(rowIndex),
								childMapping->entityId,
								std::string());

							if(!childIdentity.value("hasPrimaryKey", false))
							{
								// This child has no key needed by a further owned mapping, so it is
								// fully hydrated and does not need to enter the traversal queue.
								continue;
							}

							std::shared_ptr<Instance> childInstance = FindOwnedHydratedInstanceForIdentity(
								s.ruleset,
								childMapping->entityId,
								parent.instanceId,
								relationshipMapping->relationshipId,
								childIdentity);

							if(childInstance == nullptr)
							{
								res.status = 502;
								res.set_content(make_error("start_descendant_instance_unresolved", "A loaded descendant row could not be matched to its concrete session instance.").dump(2), "application/json");
								return;
							}

							SetRecordIdentityInstanceId(childIdentity, childMapping->entityId, childInstance->instanceId);
							const std::string queueKey = childMapping->entityId + "|" + childInstance->instanceId;

							if(queuedInstances.insert(queueKey).second)
							{
								traversalQueue.push_back({
									std::move(childIdentity),
									childMapping->entityId,
									childInstance->instanceId,
									parent.depth + 1
								});
							}
						}
					}
				}
				}
			}

		}

		if(cleanStart && (selectedFormLoaded || reset || hasStartValues))
		{
			// Clean template, start values, Root data, descendants, and links are now
			// complete. Evaluate once before resolving the first flow target.
			EvaluateInitialSessionModelRules(s.ruleset);
		}

		DataManager dataManager;
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<ScreenFile::Screen> screen;
		string entityId;
		string instanceId;
		string action = "start";
		NavigationTarget selectedFlowTarget;

		if(!requestedScreenId.empty())
		{
			screen = sessionScreenData.GetScreenByID(requestedScreenId);

			if(screen == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("screen_not_found", "Requested screen not found.").dump(2), "application/json");

				return;
			}

			entityId = !requestedEntityId.empty() ? requestedEntityId : screen->entityId;
			instanceId = !requestedInstanceId.empty() ? requestedInstanceId : (screen->instanceId.has_value() ? screen->instanceId.value() : std::string());
			action = "resume";
		}
		else if(resume)
		{
			const ScreenLocation latest = LatestScreenLocation(s.ruleset);

			if(!latest.screenId.empty())
			{
				screen = sessionScreenData.GetScreenByID(latest.screenId);
				entityId = latest.entityId;
				instanceId = latest.instanceId;

				if(screen != nullptr)
				{
					action = "resume";
				}
			}
		}

		if(screen == nullptr)
		{
			selectedFlowTarget = FindFirstScreen(sessionScreenData, s.ruleset, dataManager);
			screen = selectedFlowTarget.screen;

			if(screen != nullptr)
			{
				entityId = selectedFlowTarget.entityId;
				instanceId = selectedFlowTarget.instanceId;
				action = "start";
			}
		}

		if(screen == nullptr)
		{
			bool hasBlockingValidationErrors = false;
			json out = {
				{"ok", true},
				{"session", {
					{"token", token},
					{"sequence", s.message_count + 1}
				}},
				{"navigation", {
					{"action", "complete"},
					{"fromScreenId", std::string()},
					{"toScreenId", std::string()},
					{"entityId", std::string()},
					{"instanceId", std::string()},
					{"blocked", false}
				}},
				{"validationEvents", BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors)}
			};
			out["screenLocation"] = BuildCompleteScreenLocationJson(s.message_count + 1);
			out["actions"] = BuildCompleteActionsJson();

			if(s.selectedForm.selected)
			{
				out["form"] = SelectedFormRuntimeToJson(s.selectedForm);
			}

			if(startInputAppliedCount > 0)
			{
				out["startInputsApplied"] = startInputAppliedCount;
			}

			if(startHydrationAttempted)
			{
				out["startHydration"] = {
					{"attempted", true},
					{"recordFound", startRootRecordFound},
					{"hydrated", startHydrated},
					{"hydratedAttributeCount", startHydratedAttributeCount},
					{"descendantInstanceCount", startDescendantInstanceCount},
					{"descendantOperationCount", startDescendantOperationCount}
				};
			}

			if(cleanStart)
			{
				out["startPipeline"] = {
					{"elapsedMicros", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - startPipelineStarted).count()},
					{"evaluationPassCount", (selectedFormLoaded || reset || hasStartValues) ? 1 : 0},
					{"startInputCount", startInputAppliedCount},
					{"descendantInstanceCount", startDescendantInstanceCount}
				};
			}

			Session updated;

			const bool stored = (reset || selectedFormLoaded)
				? store_session_runtime(token, s, s.message_count, &updated)
				: touch_session(token, &updated);

			if(!stored)
			{
				res.status = 401;

				res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

				return;
			}

			WriteEndpointJsonResponse(res, out, responseOptions);

			return;
		}

		if(entityId.empty())
		{
			res.status = 400;

			res.set_content(make_error("missing_screen_entity", "The selected screen does not define an entityId.").dump(2), "application/json");

			return;
		}

		if(instanceId.empty())
		{
			instanceId = dataManager.GetFirstInstanceId(s.ruleset.entities, entityId);
		}

		if(instanceId.empty() || dataManager.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId) == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("instance_not_found", "No session instance was found for the selected screen.").dump(2), "application/json");

			return;
		}

		screen = RefreshScreenForInstance(dataManager, s.ruleset, screen, entityId, instanceId, &sessionScreenData);

		bool hasBlockingValidationErrors = false;
		json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);

		ScreenLocation location;
		location.screenId = screen->id;
		location.entityId = entityId;
		location.instanceId = instanceId;
		s.ruleset.AddScreenLocation(location);

		json out = {
			{"ok", true},
			{"session", {
				{"token", token},
				{"sequence", s.message_count + 1}
			}},
			{"navigation", {
				{"action", action},
				{"fromScreenId", std::string()},
				{"toScreenId", screen->id},
				{"entityId", entityId},
				{"instanceId", instanceId},
				{"blocked", hasBlockingValidationErrors}
			}},
			{"validationEvents", validationEvents},
			{"screen", BuildFrontendScreen(*screen)},
			{"screenLocation", BuildScreenLocationJson(*screen, entityId, instanceId, s.message_count + 1)},
			{"actions", BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData)}
		};

		if(responseOptions.includeUiState)
		{
			out["uiState"] = BuildScreenUiState(*screen);
		}

		if(s.selectedForm.selected)
		{
			out["form"] = SelectedFormRuntimeToJson(s.selectedForm);
		}

		if(startInputAppliedCount > 0)
		{
			out["startInputsApplied"] = startInputAppliedCount;
		}

		if(startHydrationAttempted)
		{
			out["startHydration"] = {
				{"attempted", true},
				{"recordFound", startRootRecordFound},
				{"hydrated", startHydrated},
				{"hydratedAttributeCount", startHydratedAttributeCount},
				{"descendantInstanceCount", startDescendantInstanceCount},
				{"descendantOperationCount", startDescendantOperationCount}
			};
		}

		if(cleanStart)
		{
			out["startPipeline"] = {
				{"elapsedMicros", std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - startPipelineStarted).count()},
				{"evaluationPassCount", (selectedFormLoaded || reset || hasStartValues) ? 1 : 0},
				{"startInputCount", startInputAppliedCount},
				{"descendantInstanceCount", startDescendantInstanceCount}
			};
		}

		if(selectedFlowTarget.usedFlow)
		{
			out["navigation"]["flow"] = {
				{"used", true},
				{"transitionId", selectedFlowTarget.transitionId},
				{"transitionLabel", selectedFlowTarget.transitionLabel}
			};
		}

		Session updated;

		const bool stored = (reset || selectedFormLoaded)
			? store_session_runtime(token, s, s.message_count, &updated)
			: touch_session(token, &updated);

		if(!stored)
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		WriteEndpointJsonResponse(res, out, responseOptions);
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}
}

bool IsCollectFileControlElement(const ScreenFile::Element& element)
{
	const std::string controlType = lowercase_copy(element.controlType);
	return (controlType == "collect-file" || controlType == "collectfile") && element.collectFile.has_value();
}

std::string FormatCollectFileSizeForResponse(unsigned long long bytes)
{
	constexpr unsigned long long bytesPerMb = 1024ULL * 1024ULL;
	std::ostringstream formatted;
	if(bytes < bytesPerMb)
	{
		formatted << ((bytes + 512ULL) / 1024ULL) << " KB";
	}
	else
	{
		formatted << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / static_cast<double>(bytesPerMb)) << " MB";
	}
	return formatted.str();
}

struct CollectFileMutationRollbackState
{
	RuleSet* ruleSet{nullptr};
	bool finalized{false};
	vector<ForgetAttributeSnapshot> attributeSnapshots;
	vector<ForgetEntityInstancesSnapshot> entitySnapshots;
	vector<ForgetInstanceLinksSnapshot> linkSnapshots;
	vector<RuleSet::UploadInfo> uploadsBefore;
	vector<RuleSet::PaginationInfo> paginationsBefore;
	vector<ScreenLocation> screenLocationsBefore;
	unordered_map<string, string> inferredInstanceIdsBefore;
	unordered_map<string, RuleSet::ValidationEvent> validationEventsBefore;

	explicit CollectFileMutationRollbackState(RuleSet& value)
		: ruleSet(&value)
	{
		if(value.uploads != nullptr) uploadsBefore = *value.uploads;
		if(value.paginations != nullptr) paginationsBefore = *value.paginations;
		if(value.screenLocations != nullptr) screenLocationsBefore = *value.screenLocations;
		if(value.inferredInstanceIdsByKey != nullptr) inferredInstanceIdsBefore = *value.inferredInstanceIdsByKey;
		if(value.activeValidationEventsByKey != nullptr) validationEventsBefore = *value.activeValidationEventsByKey;

		if(value.entities == nullptr)
		{
			return;
		}

		for(const shared_ptr<Entity>& entity : *value.entities)
		{
			if(entity == nullptr) continue;
			entitySnapshots.push_back({entity, entity->instances});
			for(const shared_ptr<Instance>& instance : entity->instances)
			{
				if(instance == nullptr) continue;
				linkSnapshots.push_back({instance, instance->linkedInstances, instance->relationshipLinks});
				for(const shared_ptr<Attribute>& attribute : instance->attributes)
				{
					if(attribute != nullptr) attributeSnapshots.push_back({attribute, *attribute});
				}
			}
		}
	}

	void Commit()
	{
		finalized = true;
	}

	void Rollback()
	{
		if(finalized || ruleSet == nullptr) return;
		finalized = true;

		for(ForgetAttributeSnapshot& snapshot : attributeSnapshots)
		{
			if(snapshot.target != nullptr) *snapshot.target = std::move(snapshot.value);
		}
		for(ForgetInstanceLinksSnapshot& snapshot : linkSnapshots)
		{
			if(snapshot.instance == nullptr) continue;
			snapshot.instance->linkedInstances = std::move(snapshot.linkedInstances);
			snapshot.instance->relationshipLinks = std::move(snapshot.relationshipLinks);
			snapshot.instance->InvalidateRelationshipLinkIndex();
		}
		for(ForgetEntityInstancesSnapshot& snapshot : entitySnapshots)
		{
			if(snapshot.entity == nullptr) continue;
			snapshot.entity->instances = std::move(snapshot.instances);
			snapshot.entity->instanceIndexSize = static_cast<size_t>(-1);
		}
		if(ruleSet->uploads != nullptr) *ruleSet->uploads = std::move(uploadsBefore);
		if(ruleSet->paginations != nullptr) *ruleSet->paginations = std::move(paginationsBefore);
		if(ruleSet->screenLocations != nullptr) *ruleSet->screenLocations = std::move(screenLocationsBefore);
		if(ruleSet->inferredInstanceIdsByKey != nullptr) *ruleSet->inferredInstanceIdsByKey = std::move(inferredInstanceIdsBefore);
		if(ruleSet->activeValidationEventsByKey != nullptr) *ruleSet->activeValidationEventsByKey = std::move(validationEventsBefore);
	}

	~CollectFileMutationRollbackState()
	{
		try { Rollback(); } catch(...) {}
	}
};

bool SetCollectFileMappedAttribute(const shared_ptr<Instance>& instance,
	const optional<ScreenFile::CollectFileAttribute>& mapping,
	const std::string& value,
	bool number,
	std::string& error)
{
	if(!mapping.has_value()) return true;
	if(instance == nullptr)
	{
		error = "The file entity instance could not be prepared.";
		return false;
	}

	shared_ptr<Attribute> attribute = instance->GetAttributeByID(mapping->attributeId);
	if(attribute == nullptr)
	{
		error = "A configured file metadata attribute does not exist on the selected entity.";
		return false;
	}

	const int attributeType = attribute->GetType();
	const bool compatible = number
		? attributeType == Attribute::INTEGER || attributeType == Attribute::DOUBLE
		: attributeType == Attribute::STRING;
	if(!compatible || !attribute->SetValueFromString(value, false))
	{
		error = number
			? "The configured file-size attribute is not a Number attribute."
			: "A configured file-name or extension attribute is not a Text attribute.";
		return false;
	}

	return true;
}

bool SetCollectFileDisplayedFieldAttributes(const shared_ptr<Instance>& instance,
	const optional<ScreenFile::CollectData>& collectData,
	const std::string& fileName,
	const std::string& extension,
	const std::string& sizeBytes,
	std::string& error)
{
	if(!collectData.has_value() || !collectData->columns.has_value()) return true;

	unordered_set<string> populatedAttributeIds;
	for(const ScreenFile::Column& column : collectData->columns.value())
	{
		if((column.source != "FileName" && column.source != "Extension" && column.source != "Size") ||
		   column.attributeId.empty() || column.attributeId.rfind("__file_", 0) == 0 ||
		   !populatedAttributeIds.insert(column.attributeId).second)
		{
			continue;
		}

		ScreenFile::CollectFileAttribute configuredAttribute;
		configuredAttribute.attributeId = column.attributeId;
		optional<ScreenFile::CollectFileAttribute> mapping = configuredAttribute;
		if(column.source == "FileName")
		{
			if(!SetCollectFileMappedAttribute(instance, mapping, fileName, false, error)) return false;
		}
		else if(column.source == "Extension")
		{
			if(!SetCollectFileMappedAttribute(instance, mapping, extension, false, error)) return false;
		}
		else if(column.source == "Size")
		{
			if(!SetCollectFileMappedAttribute(instance, mapping, sizeBytes, true, error)) return false;
		}
	}

	return true;
}

void LinkCollectFileInstance(const shared_ptr<Instance>& parent,
	const shared_ptr<Instance>& child,
	const shared_ptr<Relationship>& relationship)
{
	parent->AddLinkedInstance(child);
	child->AddLinkedInstance(parent);
	parent->AddRelationshipLink(child, relationship, true);
	child->AddRelationshipLink(parent, relationship, false);
}

void RuleServer::HandleCollectFileUploadRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/collect-file/upload", req, res);
	const std::string token = extract_token(req);
	if(token.empty())
	{
		res.status = 401;
		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");
		return;
	}

	Session s;
	if(!try_get_session(token, s))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
		return;
	}

	if(!req.is_multipart_form_data() || req.form.get_file_count("file") != 1)
	{
		res.status = 400;
		res.set_content(make_error("missing_file", "Collect file upload requests require exactly one file field named file.").dump(2), "application/json");
		return;
	}

	try
	{
		const string screenId = CollectFileFormField(req, {"screenId", "screenid"});
		const string screenEntityId = CollectFileFormField(req, {"screenEntityId", "screenentityid", "parentEntityId", "parententityid"});
		const string screenInstanceId = CollectFileFormField(req, {"screenInstanceId", "screeninstanceid", "parentInstanceId", "parentinstanceid"});
		const string elementId = CollectFileFormField(req, {"elementId", "elementid", "collectControlId", "collectcontrolid"});
		const string requestedEntityId = CollectFileFormField(req, {"entityId", "entityid", "childEntityId", "childentityid"});
		const optional<std::uint64_t> clientSequence = CollectFileFormSequence(req);

		if(screenId.empty() || screenEntityId.empty() || screenInstanceId.empty() || elementId.empty())
		{
			res.status = 400;
			res.set_content(make_error("missing_collect_file_context", "Collect file uploads require screenId, screenEntityId, screenInstanceId, and elementId.").dump(2), "application/json");
			return;
		}

		if(ScreenRequestIsStale(s, clientSequence, screenId, screenEntityId, screenInstanceId))
		{
			WriteStaleScreenResponse(token, s, clientSequence, res, BuildEndpointResponseOptions(req));
			return;
		}

		DataManager dataManager;
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<Instance> parentInstance = dataManager.GetEntityInstanceById(s.ruleset.entities, screenEntityId, screenInstanceId);
		if(parentInstance == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("parent_not_found", "The Collect file parent instance could not be found.").dump(2), "application/json");
			return;
		}

		ScreenLocation location;
		location.screenId = screenId;
		location.entityId = screenEntityId;
		location.instanceId = screenInstanceId;
		shared_ptr<ScreenFile::Screen> screen = dataManager.GetScreen(
			s.ruleset.entities,
			RuleServerScreenData(sessionScreenData),
			location,
			BorrowRuleSet(s.ruleset),
			true);
		if(screen == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("screen_not_found", "The Collect file screen could not be found.").dump(2), "application/json");
			return;
		}

		shared_ptr<ScreenFile::Element> element = dataManager.GetElementById(screen, elementId);
		if(element == nullptr || !IsCollectFileControlElement(*element))
		{
			res.status = element == nullptr ? 404 : 400;
			res.set_content(make_error(element == nullptr ? "element_not_found" : "element_not_collect_file", "The requested element is not a configured Collect file control.").dump(2), "application/json");
			return;
		}

		const string entityId = element->collectEntity.value_or("");
		const string relationshipId = element->collectRelationshipId.value_or("");
		if(entityId.empty() || relationshipId.empty() || (!requestedEntityId.empty() && requestedEntityId != entityId))
		{
			res.status = 400;
			res.set_content(make_error("collect_file_scope_invalid", "The Collect file entity or relationship does not match the request.").dump(2), "application/json");
			return;
		}

		shared_ptr<Entity> entity = dataManager.GetEntityByID(s.ruleset.entities, entityId);
		shared_ptr<Relationship> relationship = FindRuntimeRelationshipById(s.ruleset, relationshipId);
		if(entity == nullptr || relationship == nullptr || relationship->sourceEntity != screenEntityId || relationship->targetEntity != entityId)
		{
			res.status = 400;
			res.set_content(make_error("collect_file_relationship_invalid", "The Collect file relationship is not a one-way parent-to-file-entity relationship in this screen context.").dump(2), "application/json");
			return;
		}

		const httplib::FormData file = req.form.get_file("file");
		const string fileName = CollectFileBaseName(file.filename);
		const string extension = CollectFileExtension(fileName);
		if(fileName.empty())
		{
			res.status = 400;
			res.set_content(make_error("invalid_file_name", "The selected file has an invalid file name.").dump(2), "application/json");
			return;
		}

		const ScreenFile::CollectFile& configuration = element->collectFile.value();
		if(configuration.maximumCount.has_value())
		{
			const size_t currentCount = static_cast<size_t>(std::count_if(
				entity->instances.begin(),
				entity->instances.end(),
				[&](const shared_ptr<Instance>& candidate)
				{
					return CollectActionTargetBelongsToParent(*element, parentInstance, candidate);
				}));
			if(currentCount >= static_cast<size_t>(configuration.maximumCount.value()))
			{
				res.status = 409;
				res.set_content(make_error("collect_file_count_limit_reached", "The maximum number of files has already been selected.").dump(2), "application/json");
				return;
			}
		}
		const int maximumSizeMb = configuration.maximumSizeMb.value_or(sessionScreenData.fileTransfer.maximumSizeMb);
		const unsigned long long maximumBytes = static_cast<unsigned long long>(std::max(1, maximumSizeMb)) * 1024ULL * 1024ULL;
		const unsigned long long fileSizeBytes = static_cast<unsigned long long>(file.content.size());
		if(fileSizeBytes > maximumBytes)
		{
			res.status = 413;
			res.set_content(make_error("file_too_large", "The selected file exceeds this control's maximum upload size.").dump(2), "application/json");
			return;
		}

		if(!CollectFileExtensionAllowed(configuration, extension))
		{
			res.status = 415;
			res.set_content(make_error("file_type_not_allowed", "The selected file extension is not allowed for this control.").dump(2), "application/json");
			return;
		}

		if(!CollectFileMimeMatchesExtension(extension, file.content_type) || !CollectFileHasExpectedSignature(extension, file.content))
		{
			res.status = 415;
			res.set_content(make_error("file_type_mismatch", "The selected file content or MIME type does not match its file extension.").dump(2), "application/json");
			return;
		}

		shared_ptr<Instance> childInstance = entity->GetCleanInstance();
		if(childInstance == nullptr)
		{
			res.status = 500;
			res.set_content(make_error("collect_file_instance_failed", "The file entity instance could not be prepared.").dump(2), "application/json");
			return;
		}

		const string fileId = Util::GenerateGUID();
		childInstance->entityId = entityId;
		childInstance->SetInstanceId(fileId);
		string mappingError;
		if(!SetCollectFileMappedAttribute(childInstance, configuration.fileNameAttribute, fileName, false, mappingError)
			|| !SetCollectFileMappedAttribute(childInstance, configuration.extensionAttribute, extension, false, mappingError)
			|| !SetCollectFileMappedAttribute(childInstance, configuration.sizeAttribute, std::to_string(fileSizeBytes), true, mappingError)
			|| !SetCollectFileDisplayedFieldAttributes(
				childInstance,
				element->collectData,
				fileName,
				extension,
				std::to_string(fileSizeBytes),
				mappingError))
		{
			res.status = 500;
			res.set_content(make_error("collect_file_mapping_invalid", mappingError).dump(2), "application/json");
			return;
		}

		const string uploadUrl = TrimAscii(sessionScreenData.fileTransfer.uploadUrl);
		const string deleteUrl = TrimAscii(sessionScreenData.fileTransfer.deleteUrl);
		const bool hasUploadEndpoint = !uploadUrl.empty();
		if(hasUploadEndpoint != !deleteUrl.empty())
		{
			res.status = 500;
			res.set_content(make_error("file_transfer_configuration_invalid", "File upload and delete endpoints must either both be configured or both be blank.").dump(2), "application/json");
			return;
		}

		ParsedHttpEndpoint uploadEndpoint;
		ParsedHttpEndpoint deleteEndpoint;
		if(hasUploadEndpoint)
		{
			uploadEndpoint = ParseCollectFileEndpoint(uploadUrl);
			deleteEndpoint = ParseCollectFileEndpoint(deleteUrl);
			if(!uploadEndpoint.valid || !deleteEndpoint.valid)
			{
				res.status = 500;
				res.set_content(make_error("file_transfer_configuration_invalid", "The configured file-transfer endpoint is invalid.").dump(2), "application/json");
				return;
			}

			const DataConnectionHttpPostResult externalUpload = PostCollectFileMultipart(uploadEndpoint, fileId, fileName, file.content_type, file.content);
			if(!externalUpload.completed || externalUpload.status < 200 || externalUpload.status >= 300)
			{
				res.status = 502;
				res.set_content(make_error("file_upload_failed", externalUpload.completed
					? "The configured file upload endpoint returned HTTP " + std::to_string(externalUpload.status) + "."
					: "The configured file upload endpoint could not be reached.").dump(2), "application/json");
				return;
			}
		}

		CollectFileMutationRollbackState transaction(s.ruleset);
		try
		{
			entity->instances.push_back(childInstance);
			entity->instanceIndexSize = static_cast<size_t>(-1);
			LinkCollectFileInstance(parentInstance, childInstance, relationship);

			RuleSet::UploadInfo uploadInfo;
			uploadInfo.fileId = fileId;
			uploadInfo.uploadControlId = elementId;
			uploadInfo.fileName = fileName;
			uploadInfo.fileExtension = extension;
			uploadInfo.fileSizeBytes = fileSizeBytes;
			uploadInfo.fileSize = FormatCollectFileSizeForResponse(fileSizeBytes);
			uploadInfo.instanceId = fileId;
			uploadInfo.parentInstanceId = screenInstanceId;
			uploadInfo.entityId = entityId;
			uploadInfo.relationshipId = relationshipId;
			uploadInfo.externallyUploaded = hasUploadEndpoint;
			s.ruleset.uploads->push_back(uploadInfo);

			childInstance->EvaluateFunctions();
			parentInstance->EvaluateFunctions();
			vector<RelationshipMutation> mutations;
			unordered_set<string> mutationKeys;
			AddRelationshipMutation(mutations, mutationKeys, screenEntityId, screenInstanceId, relationshipId);
			const SessionModelRuleEvaluator::EvaluationStats evaluationStats = EvaluateRelationshipMutations(s.ruleset, mutations);

			screen = RefreshScreenForInstance(dataManager, s.ruleset, screen, screenEntityId, screenInstanceId, &sessionScreenData);
			bool hasBlockingValidationErrors = false;
			json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);
			json out = {
				{"ok", true},
				{"session", {{"token", token}, {"sequence", s.message_count + 1}}},
				{"navigation", {{"action", "collectFileUpload"}, {"fromScreenId", screenId}, {"toScreenId", screenId}, {"entityId", screenEntityId}, {"instanceId", screenInstanceId}, {"blocked", hasBlockingValidationErrors}}},
				{"validationEvents", validationEvents},
				{"screen", screen != nullptr ? BuildFrontendScreen(*screen) : json(nullptr)},
				{"screenLocation", screen != nullptr ? BuildScreenLocationJson(*screen, screenEntityId, screenInstanceId, s.message_count + 1) : BuildScreenLocationJson(screenId, screenEntityId, screenInstanceId, s.message_count + 1)},
				{"actions", screen != nullptr ? BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData) : BuildDetachedScreenActionsJson()},
				{"file", {{"fileId", fileId}, {"fileName", fileName}, {"extension", extension}, {"sizeBytes", fileSizeBytes}, {"size", uploadInfo.fileSize}, {"externallyUploaded", hasUploadEndpoint}}},
				{"createdInstance", {{"entityId", entityId}, {"instanceId", fileId}, {"parentEntityId", screenEntityId}, {"parentInstanceId", screenInstanceId}, {"elementId", elementId}}},
				{"sessionModelEvaluation", RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats)}
			};

			const EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
			if(responseOptions.includeUiState && screen != nullptr) out["uiState"] = BuildScreenUiState(*screen);

			Session updated;
			if(!touch_session(token, &updated))
			{
				throw std::runtime_error("The session expired before the uploaded file could be committed.");
			}

			transaction.Commit();
			WriteEndpointJsonResponse(res, out, responseOptions);
			return;
		}
		catch(...)
		{
			transaction.Rollback();
			if(hasUploadEndpoint)
			{
				PostCollectFileDelete(deleteEndpoint, fileId);
			}
			throw;
		}
	}
	catch(const std::exception& e)
	{
		res.status = 500;
		res.set_content(make_error("collect_file_upload_failed", std::string("The file could not be recorded: ") + e.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleCollectFileDeleteRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/collect-file/delete", req, res);
	const std::string token = extract_token(req);
	if(token.empty())
	{
		res.status = 401;
		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");
		return;
	}

	Session s;
	if(!try_get_session(token, s))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
		return;
	}

	try
	{
		const json request = json::parse(req.body);
		const EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req, &request);
		const string screenId = first_json_scalar_string(request, {"screenId", "screenid"});
		const string screenEntityId = first_json_scalar_string(request, {"screenEntityId", "screenentityid", "parentEntityId", "parententityid"});
		const string screenInstanceId = first_json_scalar_string(request, {"screenInstanceId", "screeninstanceid", "parentInstanceId", "parentinstanceid"});
		const string elementId = first_json_scalar_string(request, {"elementId", "elementid", "collectControlId", "collectcontrolid"});
		const string requestedEntityId = first_json_scalar_string(request, {"entityId", "entityid", "childEntityId", "childentityid"});
		const string requestedInstanceId = first_json_scalar_string(request, {"instanceId", "instanceid", "deletedInstanceId", "deletedinstanceid"});
		const string requestedFileId = first_json_scalar_string(request, {"fileId", "fileid"});
		const optional<std::uint64_t> clientSequence = first_json_sequence(request, {"screenSequence", "sequence"});

		if(screenId.empty() || screenEntityId.empty() || screenInstanceId.empty() || elementId.empty() || requestedInstanceId.empty())
		{
			res.status = 400;
			res.set_content(make_error("missing_collect_file_delete_context", "Collect file deletion requires screenId, screenEntityId, screenInstanceId, elementId, and instanceId.").dump(2), "application/json");
			return;
		}

		if(!requestedFileId.empty() && requestedFileId != requestedInstanceId)
		{
			res.status = 400;
			res.set_content(make_error("collect_file_identity_mismatch", "The requested fileId and instanceId do not identify the same file.").dump(2), "application/json");
			return;
		}

		if(ScreenRequestIsStale(s, clientSequence, screenId, screenEntityId, screenInstanceId))
		{
			WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);
			return;
		}

		DataManager dataManager;
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<Instance> parentInstance = dataManager.GetEntityInstanceById(s.ruleset.entities, screenEntityId, screenInstanceId);
		if(parentInstance == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("parent_not_found", "The Collect file parent instance could not be found.").dump(2), "application/json");
			return;
		}

		ScreenLocation location;
		location.screenId = screenId;
		location.entityId = screenEntityId;
		location.instanceId = screenInstanceId;
		shared_ptr<ScreenFile::Screen> screen = dataManager.GetScreen(
			s.ruleset.entities,
			RuleServerScreenData(sessionScreenData),
			location,
			BorrowRuleSet(s.ruleset),
			true);
		if(screen == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("screen_not_found", "The Collect file screen could not be found.").dump(2), "application/json");
			return;
		}

		shared_ptr<ScreenFile::Element> element = dataManager.GetElementById(screen, elementId);
		if(element == nullptr || !IsCollectFileControlElement(*element))
		{
			res.status = element == nullptr ? 404 : 400;
			res.set_content(make_error(element == nullptr ? "element_not_found" : "element_not_collect_file", "The requested element is not a configured Collect file control.").dump(2), "application/json");
			return;
		}

		const string entityId = element->collectEntity.value_or("");
		const string relationshipId = element->collectRelationshipId.value_or("");
		if(entityId.empty() || relationshipId.empty() || (!requestedEntityId.empty() && requestedEntityId != entityId))
		{
			res.status = 400;
			res.set_content(make_error("collect_file_scope_invalid", "The Collect file entity or relationship does not match the request.").dump(2), "application/json");
			return;
		}

		shared_ptr<Instance> deletedInstance = dataManager.GetEntityInstanceById(s.ruleset.entities, entityId, requestedInstanceId);
		if(deletedInstance == nullptr || !CollectActionTargetBelongsToParent(*element, parentInstance, deletedInstance))
		{
			res.status = 404;
			res.set_content(make_error("collect_file_not_found", "The requested file is not part of this Collect file control and parent instance.").dump(2), "application/json");
			return;
		}

		const auto uploadIt = std::find_if(s.ruleset.uploads->begin(), s.ruleset.uploads->end(), [&](const RuleSet::UploadInfo& info)
		{
			return info.fileId == requestedInstanceId
				&& info.instanceId == requestedInstanceId
				&& info.uploadControlId == elementId
				&& info.parentInstanceId == screenInstanceId
				&& info.entityId == entityId
				&& info.relationshipId == relationshipId;
		});
		if(uploadIt == s.ruleset.uploads->end())
		{
			res.status = 404;
			res.set_content(make_error("collect_file_metadata_not_found", "The requested file metadata could not be found in this session.").dump(2), "application/json");
			return;
		}

		const RuleSet::UploadInfo uploadInfo = *uploadIt;
		if(uploadInfo.externallyUploaded)
		{
			const string uploadUrl = TrimAscii(sessionScreenData.fileTransfer.uploadUrl);
			const string deleteUrl = TrimAscii(sessionScreenData.fileTransfer.deleteUrl);
			if(uploadUrl.empty() || deleteUrl.empty())
			{
				res.status = 500;
				res.set_content(make_error("file_transfer_configuration_invalid", "This file was uploaded externally, but the paired file-transfer endpoints are no longer configured.").dump(2), "application/json");
				return;
			}

			const ParsedHttpEndpoint deleteEndpoint = ParseCollectFileEndpoint(deleteUrl);
			if(!deleteEndpoint.valid)
			{
				res.status = 500;
				res.set_content(make_error("file_transfer_configuration_invalid", "The configured file delete endpoint is invalid.").dump(2), "application/json");
				return;
			}

			const DataConnectionHttpPostResult externalDelete = PostCollectFileDelete(deleteEndpoint, uploadInfo.fileId);
			if(!externalDelete.completed || externalDelete.status < 200 || externalDelete.status >= 300)
			{
				res.status = 502;
				res.set_content(make_error("file_delete_failed", externalDelete.completed
					? "The configured file delete endpoint returned HTTP " + std::to_string(externalDelete.status) + ". The file record has been retained."
					: "The configured file delete endpoint could not be reached. The file record has been retained.").dump(2), "application/json");
				return;
			}
		}

		CollectFileMutationRollbackState transaction(s.ruleset);
		vector<RelationshipMutation> relationshipMutations = CollectRelationshipMutationsForInstance(s.ruleset, entityId, requestedInstanceId);
		dataManager.DeleteInstance(entityId, requestedInstanceId, s.ruleset.entities);
		s.ruleset.uploads->erase(std::remove_if(s.ruleset.uploads->begin(), s.ruleset.uploads->end(), [&](const RuleSet::UploadInfo& info)
		{
			return info.fileId == uploadInfo.fileId && info.instanceId == uploadInfo.instanceId;
		}), s.ruleset.uploads->end());

		const SessionModelRuleEvaluator::EvaluationStats evaluationStats = EvaluateRelationshipMutations(s.ruleset, relationshipMutations);
		screen = RefreshScreenForInstance(dataManager, s.ruleset, screen, screenEntityId, screenInstanceId, &sessionScreenData);
		bool hasBlockingValidationErrors = false;
		json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);
		json out = {
			{"ok", true},
			{"session", {{"token", token}, {"sequence", s.message_count + 1}}},
			{"navigation", {{"action", "collectFileDelete"}, {"fromScreenId", screenId}, {"toScreenId", screenId}, {"entityId", screenEntityId}, {"instanceId", screenInstanceId}, {"blocked", hasBlockingValidationErrors}}},
			{"validationEvents", validationEvents},
			{"screen", screen != nullptr ? BuildFrontendScreen(*screen) : json(nullptr)},
			{"screenLocation", screen != nullptr ? BuildScreenLocationJson(*screen, screenEntityId, screenInstanceId, s.message_count + 1) : BuildScreenLocationJson(screenId, screenEntityId, screenInstanceId, s.message_count + 1)},
			{"actions", screen != nullptr ? BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData) : BuildDetachedScreenActionsJson()},
			{"deletedFile", {{"fileId", uploadInfo.fileId}, {"entityId", entityId}, {"instanceId", requestedInstanceId}, {"elementId", elementId}}},
			{"sessionModelEvaluation", RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats)}
		};
		if(responseOptions.includeUiState && screen != nullptr) out["uiState"] = BuildScreenUiState(*screen);

		Session updated;
		if(!touch_session(token, &updated))
		{
			throw std::runtime_error("The session expired before the file deletion could be committed.");
		}

		transaction.Commit();
		WriteEndpointJsonResponse(res, out, responseOptions);
	}
	catch(const std::exception& e)
	{
		res.status = 500;
		res.set_content(make_error("collect_file_delete_failed", std::string("The file could not be deleted: ") + e.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleCreateInstanceRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/createinstance", req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	string instanceId = "";

	shared_ptr<ScreenFile::Screen> screen;
	SessionModelRuleEvaluator::EvaluationStats relationshipEvaluationStats;
	bool hasSessionModelEvaluation = false;
	bool contextualResponse = false;
	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);

	try
	{
		auto j = json::parse(req.body);
		responseOptions = BuildEndpointResponseOptions(req, &j);

		string entityId = first_json_scalar_string(j, {"entityId", "entityid", "childEntityId", "childentityid"});
		const string screenId = first_json_scalar_string(j, {"screenId", "screenid"});
		const string screenEntityId = first_json_scalar_string(j, {"screenEntityId", "screenentityid", "parentEntityId", "parententityid"});
		const string requestedScreenInstanceId = first_json_scalar_string(j, {"screenInstanceId", "screeninstanceid"});
		const string elementId = first_json_scalar_string(j, {"elementId", "elementid", "collectControlId", "collectcontrolid"});
		const string requestedTransitionId = first_json_scalar_string(j, {"transitionId", "transitionid"});
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(j, {"screenSequence", "sequence"});
		string parentInstanceId = first_json_scalar_string(j, {"parentInstanceId", "parentinstanceid"});
		string currentEntityId = first_json_scalar_string(j, {"currentEntityId", "currententityid"});

		if(currentEntityId.empty())
		{
			currentEntityId = screenEntityId;
		}

		if(parentInstanceId.empty())
		{
			parentInstanceId = requestedScreenInstanceId;
		}

		const string screenInstanceId = !requestedScreenInstanceId.empty() ? requestedScreenInstanceId : parentInstanceId;
			contextualResponse = !screenId.empty()
			|| !elementId.empty()
			|| !requestedTransitionId.empty()
			|| clientSequence.has_value();

		DataManager dm = DataManager();
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);
		std::optional<ScreenFile::FlowTransition> selectedAddTransition;
		shared_ptr<Instance> parentInstance;
		shared_ptr<ScreenFile::Element> collectElement;

		if(contextualResponse)
		{
			if(screenId.empty() || currentEntityId.empty() || screenInstanceId.empty() || elementId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_create_instance_context", "Create-instance requests from collect controls require screenId, screenEntityId, screenInstanceId, and elementId.").dump(2), "application/json");

				return;
			}

			if(ScreenRequestIsStale(s, clientSequence, screenId, currentEntityId, screenInstanceId))
			{
				WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

				return;
			}

			ScreenLocation sourceLocation;
			sourceLocation.screenId = screenId;
			sourceLocation.entityId = currentEntityId;
			sourceLocation.instanceId = screenInstanceId;

			shared_ptr<ScreenFile::Screen> currentScreen = dm.GetScreen(
				s.ruleset.entities,
				sessionScreenDataView,
				sourceLocation,
				BorrowRuleSet(s.ruleset),
				true);

			if(currentScreen == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("screen_not_found", "The collect screen could not be found.").dump(2), "application/json");

				return;
			}

			collectElement = dm.GetElementById(currentScreen, elementId);

			if(collectElement == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("element_not_found", "The collect control could not be found on the current screen.").dump(2), "application/json");

				return;
			}

			if(!IsCollectListControlElement(*collectElement))
			{
				res.status = 400;

				res.set_content(make_error("element_not_collect", "The requested element is not an entity-instance-list control.").dump(2), "application/json");

				return;
			}

			if(IsReadOnlyCollectListControlElement(*collectElement))
			{
				res.status = 403;
				res.set_content(make_error("collect_read_only", "Collect View does not allow instances to be created.").dump(2), "application/json");
				return;
			}

			if(entityId.empty() && collectElement->collectEntity.has_value())
			{
				entityId = collectElement->collectEntity.value();
			}

			if(entityId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_entity", "The collect control does not define the entity to create.").dump(2), "application/json");

				return;
			}

			if(!collectElement->collectEntity.has_value() || collectElement->collectEntity.value() != entityId)
			{
				res.status = 400;

				res.set_content(make_error("collect_entity_mismatch", "The requested entity does not match the collect control entity.").dump(2), "application/json");

				return;
			}

			selectedAddTransition = FindAddFlowTransitionForCollectAction(sessionScreenData, screenId, elementId, requestedTransitionId);

			if(!selectedAddTransition.has_value())
			{
				res.status = 400;

				res.set_content(make_error("add_transition_not_found", "No Add transition was found for the collect control.").dump(2), "application/json");

				return;
			}

			if(selectedAddTransition->toScreenId.empty())
			{
				res.status = 400;

				res.set_content(make_error("add_transition_target_missing", "The Add transition does not specify a target screen.").dump(2), "application/json");

				return;
			}
		}

		if(entityId.empty())
		{
			if(contextualResponse)
			{
				res.status = 400;

				res.set_content(make_error("missing_entity", "Create-instance requests require an entityId.").dump(2), "application/json");

				return;
			}
		}

		parentInstance = dm.GetEntityInstanceById(s.ruleset.entities, currentEntityId, parentInstanceId);

		if(!parentInstanceId.empty())
		{
			if(parentInstance == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("parent_not_found", "Parent instance not found.").dump(2), "application/json");

				return;
			}

			instanceId = RuleServer::AddInstance(
				entityId,
				BorrowRuleSet(s.ruleset),
				parentInstance,
				collectElement != nullptr && collectElement->collectRelationshipId.has_value()
					? collectElement->collectRelationshipId.value()
					: string());

			if(s.ruleset.sessionModelRules != nullptr && !s.ruleset.sessionModelRules->empty() && !instanceId.empty())
			{
				shared_ptr<Instance> childInstance = dm.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId);

				if(childInstance != nullptr)
				{
					vector<RelationshipMutation> relationshipMutations;
					unordered_set<string> relationshipMutationKeys;

					if(collectElement != nullptr && collectElement->collectRelationshipId.has_value())
					{
						AddCollectRelationshipMutation(relationshipMutations,
						                               relationshipMutationKeys,
						                               s.ruleset,
						                               collectElement->collectRelationshipId.value(),
						                               parentInstance,
						                               childInstance);
					}

					for(const InstanceRelationshipLink& link : childInstance->relationshipLinks)
					{
						if(link.sourceInstanceId == parentInstance->instanceId || link.targetInstanceId == parentInstance->instanceId)
						{
							AddRelationshipMutationFromLink(relationshipMutations, relationshipMutationKeys, link);
						}
					}

					if(!relationshipMutations.empty())
					{
						relationshipEvaluationStats = EvaluateRelationshipMutations(s.ruleset, relationshipMutations);
						hasSessionModelEvaluation = true;
					}
				}
			}
		}
		else if(contextualResponse)
		{
			res.status = 400;

			res.set_content(make_error("missing_parent_instance", "Create-instance requests from collect controls require a parent instance.").dump(2), "application/json");

			return;
		}

		if(contextualResponse)
		{
			if(instanceId.empty())
			{
				res.status = 500;

				res.set_content(make_error("create_instance_failed", "The entity instance could not be created.").dump(2), "application/json");

				return;
			}

			screen = sessionScreenData.GetScreenByID(selectedAddTransition->toScreenId);

			if(screen == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("add_target_screen_not_found", "The Add transition target screen could not be found.").dump(2), "application/json");

				return;
			}

			if(!screen->entityId.empty() && screen->entityId != entityId)
			{
				res.status = 400;

				res.set_content(make_error("add_target_entity_mismatch", "The Add transition target screen does not collect the requested entity.").dump(2), "application/json");

				return;
			}

			if(screen->entityId.empty())
			{
				screen->entityId = entityId;
			}

			ScreenLocation loc = ScreenLocation();
			loc.screenId = screen->id;
			loc.entityId = entityId;
			loc.instanceId = instanceId;
			s.ruleset.AddScreenLocation(loc);

			screen = RefreshScreenForInstance(dm, s.ruleset, screen, entityId, instanceId, &sessionScreenData);

			bool hasBlockingValidationErrors = false;
			json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);

			json out = {
				{"ok", true},
				{"session", {
					{"token", token},
					{"sequence", s.message_count + 1}
				}},
				{"navigation", {
					{"action", "addInstance"},
					{"fromScreenId", screenId},
					{"toScreenId", screen != nullptr ? screen->id : std::string()},
					{"entityId", entityId},
					{"instanceId", instanceId},
					{"blocked", hasBlockingValidationErrors},
					{"flow", {
						{"used", true},
						{"transitionId", selectedAddTransition->transitionId},
						{"transitionLabel", selectedAddTransition->label},
						{"transitionRole", selectedAddTransition->transitionRole},
						{"collectControlId", elementId}
					}}
				}},
				{"validationEvents", validationEvents},
				{"screen", screen != nullptr ? BuildFrontendScreen(*screen) : json(nullptr)},
				{"screenLocation", screen != nullptr ? BuildScreenLocationJson(*screen, entityId, instanceId, s.message_count + 1) : BuildScreenLocationJson(selectedAddTransition->toScreenId, entityId, instanceId, s.message_count + 1)},
				{"actions", screen != nullptr ? BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData) : BuildDetachedScreenActionsJson()},
				{"instanceid", instanceId},
				{"createdInstance", {
					{"entityId", entityId},
					{"instanceId", instanceId},
					{"parentEntityId", currentEntityId},
					{"parentInstanceId", parentInstanceId},
					{"elementId", elementId}
				}}
			};

			if(responseOptions.includeUiState && screen != nullptr)
			{
				out["uiState"] = BuildScreenUiState(*screen);
			}

			if(hasSessionModelEvaluation)
			{
				out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(relationshipEvaluationStats);
			}

			if(!hasBlockingValidationErrors && !selectedAddTransition->emailTemplateId.empty())
			{
				const std::uint64_t committedSequence = s.message_count + 1;
				EmailCommandResolver::Result email = EmailCommandResolver::Resolve(
					s.ruleset,
					selectedAddTransition->emailTemplateId,
					selectedAddTransition->transitionId,
					token,
					committedSequence,
					entityId,
					instanceId);
				if(email.configured && !email.command.is_null())
				{
					s.emailCommandsBySequence[committedSequence] = email.command;
					TrimCommittedEmailCommands(s.emailCommandsBySequence, 16);
				}
			}

			Session updated;

			if(!store_session_runtime(token, s, s.message_count, &updated))
			{
				res.status = 401;

				res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

				return;
			}

			const auto committedEmail = updated.emailCommandsBySequence.find(updated.message_count);
			if(committedEmail != updated.emailCommandsBySequence.end())
			{
				out["emailCommands"] = json::array({committedEmail->second});
			}

			WriteEndpointJsonResponse(res, out, responseOptions);

			return;
		}

		screen = dm.GetFirstEntityScreen(sessionScreenDataView, entityId);

		if(screen != nullptr)
		{
			ScreenLocation loc = ScreenLocation();

			loc.screenId = screen->id;
			loc.entityId = entityId;
			loc.instanceId = instanceId;

			s.ruleset.AddScreenLocation(loc);

			auto rulesetPtr = BorrowRuleSet(s.ruleset);

			screen = dm.GetSessionData(s.ruleset.entities, screen, loc.entityId, loc.instanceId, rulesetPtr, &sessionScreenData);
		}
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	if(screen != nullptr)
	{
		screen->instanceId = instanceId;

		json out = BuildFrontendScreen(*screen);
		out["instanceid"] = instanceId;
		out["uiState"] = BuildScreenUiState(*screen);

		if(hasSessionModelEvaluation)
		{
			out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(relationshipEvaluationStats);
		}

		res.set_content(out.dump(4), "application/json");
	}
	else
	{
		json out = {
			{"instanceid", instanceId}
		};

		if(hasSessionModelEvaluation)
		{
			out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(relationshipEvaluationStats);
		}

		res.set_content(out.dump(4), "application/json");
	}
}

void RuleServer::HandleEditInstanceRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/edit", req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	shared_ptr<ScreenFile::Screen> screen;
	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
	json out = json::object();

	try
	{
		auto j = json::parse(req.body);
		responseOptions = BuildEndpointResponseOptions(req, &j);

		string entityId = first_json_scalar_string(j, {"entityId", "entityid", "childEntityId", "childentityid"});
		const string instanceId = first_json_scalar_string(j, {"instanceId", "instanceid", "editedInstanceId", "editedinstanceid"});
		const string requestedScreenId = first_json_scalar_string(j, {"screenId", "screenid"});
		const string legacyCurrentScreenId = first_json_scalar_string(j, {"currentScreenId", "currentscreenid"});
		const string screenId = !requestedScreenId.empty() ? requestedScreenId : legacyCurrentScreenId;
		string currentEntityId = first_json_scalar_string(j, {"screenEntityId", "screenentityid", "currentEntityId", "currententityid", "parentEntityId", "parententityid"});
		string screenInstanceId = first_json_scalar_string(j, {"screenInstanceId", "screeninstanceid", "currentInstanceId", "currentinstanceid", "parentInstanceId", "parentinstanceid"});
		const string elementId = first_json_scalar_string(j, {"elementId", "elementid", "collectControlId", "collectcontrolid"});
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(j, {"screenSequence", "sequence"});

		const bool contextualResponse = !requestedScreenId.empty()
			|| !elementId.empty()
			|| clientSequence.has_value();

		DataManager dm = DataManager();
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);
		std::optional<ScreenFile::FlowTransition> selectedFlowEntry;

		if(contextualResponse)
		{
			if(screenId.empty() || currentEntityId.empty() || screenInstanceId.empty() || elementId.empty() || instanceId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_edit_instance_context", "Edit-instance requests from collect controls require screenId, screenEntityId, screenInstanceId, elementId, entityId, and instanceId.").dump(2), "application/json");

				return;
			}

			if(ScreenRequestIsStale(s, clientSequence, screenId, currentEntityId, screenInstanceId))
			{
				WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

				return;
			}

			shared_ptr<Instance> parentInstance = dm.GetEntityInstanceById(s.ruleset.entities, currentEntityId, screenInstanceId);

			if(parentInstance == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("parent_not_found", "The collect parent instance could not be found.").dump(2), "application/json");

				return;
			}

			ScreenLocation sourceLocation;
			sourceLocation.screenId = screenId;
			sourceLocation.entityId = currentEntityId;
			sourceLocation.instanceId = screenInstanceId;

			shared_ptr<ScreenFile::Screen> currentScreen = dm.GetScreen(
				s.ruleset.entities,
				sessionScreenDataView,
				sourceLocation,
				BorrowRuleSet(s.ruleset),
				true);

			if(currentScreen == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("screen_not_found", "The collect screen could not be found.").dump(2), "application/json");

				return;
			}

			shared_ptr<ScreenFile::Element> collectElement = dm.GetElementById(currentScreen, elementId);

			if(collectElement == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("element_not_found", "The collect control could not be found on the current screen.").dump(2), "application/json");

				return;
			}

			if(!IsCollectListControlElement(*collectElement))
			{
				res.status = 400;

				res.set_content(make_error("element_not_collect", "The requested element is not an entity-instance-list control.").dump(2), "application/json");

				return;
			}

			if(IsReadOnlyCollectListControlElement(*collectElement))
			{
				res.status = 403;
				res.set_content(make_error("collect_read_only", "Collect View does not allow instances to be edited.").dump(2), "application/json");
				return;
			}

			if(entityId.empty() && collectElement->collectEntity.has_value())
			{
				entityId = collectElement->collectEntity.value();
			}

			if(entityId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_entity", "The collect control does not define the entity to edit.").dump(2), "application/json");

				return;
			}

			if(!collectElement->collectEntity.has_value() || collectElement->collectEntity.value() != entityId)
			{
				res.status = 400;

				res.set_content(make_error("collect_entity_mismatch", "The requested entity does not match the collect control entity.").dump(2), "application/json");

				return;
			}

			shared_ptr<Instance> editInstance = dm.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId);

			if(editInstance == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("instance_not_found", "The entity instance to edit could not be found.").dump(2), "application/json");

				return;
			}

			if(!CollectActionTargetBelongsToParent(*collectElement, parentInstance, editInstance))
			{
				res.status = 404;

				res.set_content(make_error("collect_instance_not_found", "The entity instance to edit is not part of the requested collect control context.").dump(2), "application/json");

				return;
			}

			selectedFlowEntry = FindAddFlowTransitionForCollectAction(sessionScreenData, screenId, elementId, std::string());

			if(selectedFlowEntry.has_value() && !selectedFlowEntry->toScreenId.empty())
			{
				screen = sessionScreenData.GetScreenByID(selectedFlowEntry->toScreenId);
			}

			if(screen == nullptr)
			{
				screen = dm.GetFirstEntityScreen(sessionScreenDataView, entityId);
			}

			if(screen == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("screen_not_found", "First screen for entity not found.").dump(2), "application/json");

				return;
			}

			if(!screen->entityId.empty() && screen->entityId != entityId)
			{
				res.status = 400;

				res.set_content(make_error("edit_target_entity_mismatch", "The Edit target screen does not collect the requested entity.").dump(2), "application/json");

				return;
			}

			if(screen->entityId.empty())
			{
				screen->entityId = entityId;
			}

			ScreenLocation loc = ScreenLocation();
			loc.screenId = screen->id;
			loc.entityId = entityId;
			loc.instanceId = instanceId;
			s.ruleset.AddScreenLocation(loc);

			screen = RefreshScreenForInstance(dm, s.ruleset, screen, entityId, instanceId, &sessionScreenData);

			bool hasBlockingValidationErrors = false;
			json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);

			out = {
				{"ok", true},
				{"session", {
					{"token", token},
					{"sequence", s.message_count + 1}
				}},
				{"navigation", {
					{"action", "editInstance"},
					{"fromScreenId", screenId},
					{"toScreenId", screen != nullptr ? screen->id : std::string()},
					{"entityId", entityId},
					{"instanceId", instanceId},
					{"blocked", hasBlockingValidationErrors},
					{"flow", {
						{"used", selectedFlowEntry.has_value()},
						{"transitionId", selectedFlowEntry.has_value() ? selectedFlowEntry->transitionId : std::string()},
						{"transitionLabel", selectedFlowEntry.has_value() ? selectedFlowEntry->label : std::string()},
						{"transitionRole", "Edit"},
						{"collectControlId", elementId}
					}}
				}},
				{"validationEvents", validationEvents},
				{"screen", screen != nullptr ? BuildFrontendScreen(*screen) : json(nullptr)},
				{"screenLocation", screen != nullptr ? BuildScreenLocationJson(*screen, entityId, instanceId, s.message_count + 1) : BuildScreenLocationJson(std::string(), entityId, instanceId, s.message_count + 1)},
				{"actions", screen != nullptr ? BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData) : BuildDetachedScreenActionsJson()},
				{"editedInstance", {
					{"entityId", entityId},
					{"instanceId", instanceId},
					{"parentEntityId", currentEntityId},
					{"parentInstanceId", screenInstanceId},
					{"elementId", elementId}
				}}
			};

			if(responseOptions.includeUiState && screen != nullptr)
			{
				out["uiState"] = BuildScreenUiState(*screen);
			}

			Session updated;

			if(!touch_session(token, &updated))
			{
				res.status = 401;

				res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

				return;
			}

			WriteEndpointJsonResponse(res, out, responseOptions);

			return;
		}

		if(entityId.empty() || instanceId.empty())
		{
			res.status = 400;

			res.set_content(make_error("missing_edit_context", "Edit requests require entityid and instanceid.").dump(2), "application/json");

			return;
		}

		screen = dm.GetFirstEntityScreen(sessionScreenDataView, entityId);

		if(screen == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("screen_not_found", "First screen for entity not found.").dump(2), "application/json");

			return;
		}

		screen->instanceId = instanceId;

		auto rulesetPtr = BorrowRuleSet(s.ruleset);

		dm.GetSessionData(s.ruleset.entities, screen, entityId, instanceId, rulesetPtr, &sessionScreenData);

		dm.SetDisplayElement(s.ruleset.entities, screen);

		ScreenLocation loc = ScreenLocation();

		loc.screenId = screen->id;
		loc.entityId = screen->entityId;
		loc.instanceId = instanceId;

		s.ruleset.AddScreenLocation(loc);
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	if(screen != nullptr)
	{
		string j = SerializeScreenJson(screen);

		res.set_content(j, "application/json");
	}
}

void RuleServer::HandleDeleteInstanceRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/delete", req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	shared_ptr<ScreenFile::Screen> screen;
	SessionModelRuleEvaluator::EvaluationStats relationshipEvaluationStats;
	bool hasSessionModelEvaluation = false;
	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
	json out = json::object();

	try
	{
		auto j = json::parse(req.body);
		responseOptions = BuildEndpointResponseOptions(req, &j);

		string entityId = first_json_scalar_string(j, {"entityId", "entityid", "childEntityId", "childentityid"});
		const string instanceId = first_json_scalar_string(j, {"instanceId", "instanceid", "deletedInstanceId", "deletedinstanceid"});
		const string requestedScreenId = first_json_scalar_string(j, {"screenId", "screenid"});
		const string legacyCurrentScreenId = first_json_scalar_string(j, {"currentScreenId", "currentscreenid"});
		const string screenId = !requestedScreenId.empty() ? requestedScreenId : legacyCurrentScreenId;
		string currentEntityId = first_json_scalar_string(j, {"screenEntityId", "screenentityid", "currentEntityId", "currententityid", "parentEntityId", "parententityid"});
		string screenInstanceId = first_json_scalar_string(j, {"screenInstanceId", "screeninstanceid", "currentInstanceId", "currentinstanceid", "parentInstanceId", "parentinstanceid"});
		const string elementId = first_json_scalar_string(j, {"elementId", "elementid", "collectControlId", "collectcontrolid"});
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(j, {"screenSequence", "sequence"});

		const bool contextualResponse = !requestedScreenId.empty()
			|| !elementId.empty();

		DataManager dm = DataManager();
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);

		if(contextualResponse)
		{
			if(screenId.empty() || currentEntityId.empty() || screenInstanceId.empty() || elementId.empty() || instanceId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_delete_instance_context", "Delete-instance requests from collect controls require screenId, screenEntityId, screenInstanceId, elementId, entityId, and instanceId.").dump(2), "application/json");

				return;
			}

			if(ScreenRequestIsStale(s, clientSequence, screenId, currentEntityId, screenInstanceId))
			{
				WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

				return;
			}

			shared_ptr<Instance> parentInstance = dm.GetEntityInstanceById(s.ruleset.entities, currentEntityId, screenInstanceId);

			if(parentInstance == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("parent_not_found", "The collect parent instance could not be found.").dump(2), "application/json");

				return;
			}

			ScreenLocation sourceLocation;
			sourceLocation.screenId = screenId;
			sourceLocation.entityId = currentEntityId;
			sourceLocation.instanceId = screenInstanceId;

			screen = dm.GetScreen(
				s.ruleset.entities,
				sessionScreenDataView,
				sourceLocation,
				BorrowRuleSet(s.ruleset),
				true);

			if(screen == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("screen_not_found", "The collect screen could not be found.").dump(2), "application/json");

				return;
			}

			shared_ptr<ScreenFile::Element> collectElement = dm.GetElementById(screen, elementId);

			if(collectElement == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("element_not_found", "The collect control could not be found on the current screen.").dump(2), "application/json");

				return;
			}

			if(!IsCollectListControlElement(*collectElement))
			{
				res.status = 400;

				res.set_content(make_error("element_not_collect", "The requested element is not an entity-instance-list control.").dump(2), "application/json");

				return;
			}

			if(IsReadOnlyCollectListControlElement(*collectElement))
			{
				res.status = 403;
				res.set_content(make_error("collect_read_only", "Collect View does not allow instances to be deleted.").dump(2), "application/json");
				return;
			}

			if(entityId.empty() && collectElement->collectEntity.has_value())
			{
				entityId = collectElement->collectEntity.value();
			}

			if(entityId.empty())
			{
				res.status = 400;

				res.set_content(make_error("missing_entity", "The collect control does not define the entity to delete.").dump(2), "application/json");

				return;
			}

			if(!collectElement->collectEntity.has_value() || collectElement->collectEntity.value() != entityId)
			{
				res.status = 400;

				res.set_content(make_error("collect_entity_mismatch", "The requested entity does not match the collect control entity.").dump(2), "application/json");

				return;
			}

			shared_ptr<Instance> deletedInstance = dm.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId);

			if(deletedInstance == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("instance_not_found", "The entity instance to delete could not be found.").dump(2), "application/json");

				return;
			}

			if(!CollectActionTargetBelongsToParent(*collectElement, parentInstance, deletedInstance))
			{
				res.status = 404;

				res.set_content(make_error("collect_instance_not_found", "The entity instance to delete is not part of the requested collect control context.").dump(2), "application/json");

				return;
			}

			vector<RelationshipMutation> relationshipMutations = CollectRelationshipMutationsForInstance(s.ruleset, entityId, instanceId);
			unordered_set<string> relationshipMutationKeys;

			for(const RelationshipMutation& mutation : relationshipMutations)
			{
				relationshipMutationKeys.insert(RelationshipMutationKey(mutation));
			}

			if(collectElement->collectRelationshipId.has_value())
			{
				AddRelationshipMutation(relationshipMutations,
				                        relationshipMutationKeys,
				                        currentEntityId,
				                        screenInstanceId,
				                        collectElement->collectRelationshipId.value());

				AddCollectRelationshipMutation(relationshipMutations,
				                               relationshipMutationKeys,
				                               s.ruleset,
				                               collectElement->collectRelationshipId.value(),
				                               parentInstance,
				                               deletedInstance);
			}

			dm.DeleteInstance(entityId, instanceId, s.ruleset.entities);
			deletedInstance.reset();

			if(!relationshipMutations.empty())
			{
				relationshipEvaluationStats = EvaluateRelationshipMutations(s.ruleset, relationshipMutations);
				hasSessionModelEvaluation = true;
			}

			screen = RefreshScreenForInstance(dm, s.ruleset, screen, currentEntityId, screenInstanceId, &sessionScreenData);

			bool hasBlockingValidationErrors = false;
			json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);

			out = {
				{"ok", true},
				{"session", {
					{"token", token},
					{"sequence", s.message_count + 1}
				}},
				{"navigation", {
					{"action", "deleteInstance"},
					{"fromScreenId", screenId},
					{"toScreenId", screen != nullptr ? screen->id : screenId},
					{"entityId", currentEntityId},
					{"instanceId", screenInstanceId},
					{"deletedEntityId", entityId},
					{"deletedInstanceId", instanceId},
					{"blocked", hasBlockingValidationErrors},
					{"flow", {
						{"used", false},
						{"collectControlId", elementId}
					}}
				}},
				{"validationEvents", validationEvents},
				{"screen", screen != nullptr ? BuildFrontendScreen(*screen) : json(nullptr)},
				{"screenLocation", screen != nullptr ? BuildScreenLocationJson(*screen, currentEntityId, screenInstanceId, s.message_count + 1) : BuildScreenLocationJson(screenId, currentEntityId, screenInstanceId, s.message_count + 1)},
				{"actions", screen != nullptr ? BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData) : BuildDetachedScreenActionsJson()},
				{"deletedInstance", {
					{"entityId", entityId},
					{"instanceId", instanceId},
					{"parentEntityId", currentEntityId},
					{"parentInstanceId", screenInstanceId},
					{"elementId", elementId}
				}}
			};

			if(responseOptions.includeUiState && screen != nullptr)
			{
				out["uiState"] = BuildScreenUiState(*screen);
			}

			if(hasSessionModelEvaluation)
			{
				out["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(relationshipEvaluationStats);
			}

			Session updated;

			if(!touch_session(token, &updated))
			{
				res.status = 401;

				res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

				return;
			}

			WriteEndpointJsonResponse(res, out, responseOptions);

			return;
		}

		if(entityId.empty() || instanceId.empty() || screenId.empty())
		{
			res.status = 400;

			res.set_content(make_error("missing_delete_context", "Delete requests require entityid, instanceid, and currentscreenid.").dump(2), "application/json");

			return;
		}

		vector<RelationshipMutation> relationshipMutations = CollectRelationshipMutationsForInstance(s.ruleset, entityId, instanceId);

		dm.DeleteInstance(entityId, instanceId, s.ruleset.entities);

		if(!relationshipMutations.empty())
		{
			relationshipEvaluationStats = EvaluateRelationshipMutations(s.ruleset, relationshipMutations);
			hasSessionModelEvaluation = true;
		}

		screen = sessionScreenData.GetScreenByID(screenId);

		if(screen == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("screen_not_found", "Current screen not found.").dump(2), "application/json");

			return;
		}

		if(currentEntityId.empty())
		{
			currentEntityId = screen->entityId;
		}

		screen->instanceId = screenInstanceId;

		auto rulesetPtr = BorrowRuleSet(s.ruleset);

		screen = dm.GetSessionData(s.ruleset.entities, screen, screen->entityId, screenInstanceId, rulesetPtr, &sessionScreenData);

		dm.SetDisplayElement(s.ruleset.entities, screen);
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	if(screen != nullptr)
	{
		string screenJson = SerializeScreenJson(screen);

		if(hasSessionModelEvaluation)
		{
			json legacyOut = json::parse(screenJson);
			legacyOut["sessionModelEvaluation"] = RuleServer::SessionModelEvaluationStatsToJSON(relationshipEvaluationStats);
			res.set_content(legacyOut.dump(4), "application/json");
		}
		else
		{
			res.set_content(screenJson, "application/json");
		}
	}
}

void RuleServer::HandlePageRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/page", req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
	json out = json::object();

	try
	{
		auto j = json::parse(req.body);
		responseOptions = BuildEndpointResponseOptions(req, &j);

		const string screenId = first_json_scalar_string(j, {"screenId", "screenid"});
		const string entityId = first_json_scalar_string(j, {"entityId", "entityid"});
		const string instanceId = first_json_scalar_string(j, {"instanceId", "instanceid"});
		const string elementId = first_json_scalar_string(j, {"elementId", "elementid", "id"});
		const string filter = first_json_scalar_string(j, {"filter"});
		const bool hasPage = j.is_object() && j.contains("page") && !j.at("page").is_null();
		const std::optional<std::uint64_t> requestedPage = hasPage ? json_sequence_value(j.at("page")) : std::nullopt;
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(j, {"screenSequence", "sequence"});

		if(screenId.empty() || entityId.empty() || instanceId.empty() || elementId.empty())
		{
			res.status = 400;

			res.set_content(make_error("missing_page_context", "Page requests require screenId, entityId, instanceId, and elementId.").dump(2), "application/json");

			return;
		}

		if(!hasPage)
		{
			res.status = 400;

			res.set_content(make_error("missing_page", "Page requests require a page value.").dump(2), "application/json");

			return;
		}

		if(!requestedPage.has_value() || requestedPage.value() == 0)
		{
			res.status = 400;

			res.set_content(make_error("invalid_page", "Page must be a positive integer.").dump(2), "application/json");

			return;
		}

		const int page = static_cast<int>(std::min<std::uint64_t>(requestedPage.value(), static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));

		if(ScreenRequestIsStale(s, clientSequence, screenId, entityId, instanceId))
		{
			WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

			return;
		}

		DataManager dataManager;
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);
		ScreenLocation location;
		location.screenId = screenId;
		location.entityId = entityId;
		location.instanceId = instanceId;

		shared_ptr<Instance> parentInstance = dataManager.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId);

		if(parentInstance == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("parent_not_found", "The collect parent instance could not be found.").dump(2), "application/json");

			return;
		}

		shared_ptr<ScreenFile::Screen> screen = dataManager.GetScreen(
			s.ruleset.entities,
			sessionScreenDataView,
			location,
			BorrowRuleSet(s.ruleset),
			true);

		if(screen == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("screen_not_found", "Requested screen not found.").dump(2), "application/json");

			return;
		}

		shared_ptr<ScreenFile::Element> element = dataManager.GetElementById(screen, elementId);

		if(element == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("element_not_found", "Requested page element not found.").dump(2), "application/json");

			return;
		}

		if(!IsPageableCollectElement(*element))
		{
			res.status = 400;

			res.set_content(make_error("element_not_pageable", "Requested element is not a pageable entity-instance list.").dump(2), "application/json");

			return;
		}

		shared_ptr<Entity> collectEntity = dataManager.GetEntityByID(s.ruleset.entities, element->collectEntity.value());

		if(collectEntity == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("collect_entity_not_found", "The pageable collect control target entity could not be found.").dump(2), "application/json");

			return;
		}

		if(element->collectRelationshipId.has_value() && !element->collectRelationshipId->empty())
		{
			shared_ptr<Relationship> relationship = FindRuntimeRelationshipById(s.ruleset, element->collectRelationshipId.value());

			if(relationship == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("collect_relationship_not_found", "The pageable collect control relationship could not be found.").dump(2), "application/json");

				return;
			}

			shared_ptr<Instance> relationshipSourceInstance = ResolveUniqueForgetBindingInstance(
				s.ruleset,
				parentInstance,
				relationship->sourceEntity);

			if(relationshipSourceInstance == nullptr || relationship->targetEntity != collectEntity->entityid)
			{
				res.status = 400;

				res.set_content(make_error("collect_relationship_mismatch", "The pageable collection relationship is not in scope for the current instance or does not target the displayed entity.").dump(2), "application/json");

				return;
			}
		}

		RuleSet::PaginationInfo& pageInfo = s.ruleset.GetPaginationInfo(elementId);
		pageInfo.currentPage = page;
		pageInfo.filter = filter;

		screen = RefreshScreenForInstance(dataManager, s.ruleset, screen, entityId, instanceId, &sessionScreenData);

		bool hasBlockingValidationErrors = false;
		json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);

		out = {
			{"ok", true},
			{"session", {
				{"token", token},
				{"sequence", s.message_count + 1}
			}},
			{"navigation", {
				{"action", "page"},
				{"fromScreenId", screenId},
				{"toScreenId", screen != nullptr ? screen->id : screenId},
				{"entityId", entityId},
				{"instanceId", instanceId},
				{"blocked", hasBlockingValidationErrors}
			}},
			{"validationEvents", validationEvents},
			{"screen", screen != nullptr ? BuildFrontendScreen(*screen) : json(nullptr)},
			{"screenLocation", screen != nullptr ? BuildScreenLocationJson(*screen, entityId, instanceId, s.message_count + 1) : BuildScreenLocationJson(screenId, entityId, instanceId, s.message_count + 1)},
			{"actions", screen != nullptr ? BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData) : BuildDetachedScreenActionsJson()},
			{"page", {
				{"elementId", elementId},
				{"requestedPage", page}
			}}
		};

		if(responseOptions.includeUiState && screen != nullptr)
		{
			out["uiState"] = BuildScreenUiState(*screen);
		}
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	WriteEndpointJsonResponse(res, out, responseOptions);
}

void RuleServer::HandleCollectViewMembershipRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/collect-view/membership", req, res);
	const std::string token = extract_token(req);
	if(token.empty())
	{
		res.status = 401;
		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");
		return;
	}

	Session s;
	if(!try_get_session(token, s))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
		return;
	}

	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
	json out = json::object();
	try
	{
		const json requestJson = json::parse(req.body);
		responseOptions = BuildEndpointResponseOptions(req, &requestJson);
		const string screenId = first_json_scalar_string(requestJson, {"screenId", "screenid"});
		const string entityId = first_json_scalar_string(requestJson, {"entityId", "entityid", "screenEntityId", "screenentityid"});
		const string instanceId = first_json_scalar_string(requestJson, {"instanceId", "instanceid", "screenInstanceId", "screeninstanceid"});
		const string elementId = first_json_scalar_string(requestJson, {"elementId", "elementid", "id"});
		const string targetInstanceId = first_json_scalar_string(requestJson, {"targetInstanceId", "targetinstanceid"});
		const std::optional<bool> selectedValue = JsonBoolValue(requestJson, {"selected"});
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(requestJson, {"screenSequence", "sequence"});

		if(screenId.empty() || entityId.empty() || instanceId.empty() || elementId.empty() || !selectedValue.has_value())
		{
			res.status = 400;
			res.set_content(make_error("missing_membership_context", "Collect View membership requests require screenId, entityId, instanceId, elementId, and a boolean selected value.").dump(2), "application/json");
			return;
		}

		if(ScreenRequestIsStale(s, clientSequence, screenId, entityId, instanceId))
		{
			WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);
			return;
		}

		DataManager dataManager;
		ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
		shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);
		ScreenLocation location;
		location.screenId = screenId;
		location.entityId = entityId;
		location.instanceId = instanceId;

		shared_ptr<Instance> sourceInstance = dataManager.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId);
		if(sourceInstance == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("source_instance_not_found", "The current screen instance could not be found.").dump(2), "application/json");
			return;
		}

		shared_ptr<ScreenFile::Screen> screen = dataManager.GetScreen(
			s.ruleset.entities,
			sessionScreenDataView,
			location,
			BorrowRuleSet(s.ruleset),
			true);
		if(screen == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("screen_not_found", "Requested screen not found.").dump(2), "application/json");
			return;
		}

		if(!screen->entityId.empty() && screen->entityId != entityId)
		{
			res.status = 400;
			res.set_content(make_error("screen_entity_mismatch", "The requested instance does not belong to the Collect View screen entity.").dump(2), "application/json");
			return;
		}

		shared_ptr<ScreenFile::Element> element = dataManager.GetElementById(screen, elementId);
		if(element == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("element_not_found", "The Collect View control could not be found on the current screen.").dump(2), "application/json");
			return;
		}

		if(!IsReadOnlyCollectListControlElement(*element) || !element->collectEntity.has_value() ||
		   !element->collectData.has_value() || !element->collectData->select.value_or(false) ||
		   !element->collectData->membershipRelationshipId.has_value())
		{
			res.status = 400;
			res.set_content(make_error("collect_view_selection_not_configured", "The requested element is not a selectable, fully configured Collect View control.").dump(2), "application/json");
			return;
		}

		const string membershipRelationshipId = element->collectData->membershipRelationshipId.value();
		shared_ptr<Relationship> relationship = FindRuntimeRelationshipById(s.ruleset, membershipRelationshipId);
		if(relationship == nullptr)
		{
			res.status = 404;
			res.set_content(make_error("membership_relationship_not_found", "The configured Collect View membership relationship could not be found.").dump(2), "application/json");
			return;
		}

		const bool relationshipIsManyToOne = IsManyToOneRelationship(*relationship);
		const bool relationshipIsManyToMany = IsManyToManyRelationship(*relationship);
		if(relationship->sourceEntity != entityId || relationship->targetEntity != element->collectEntity.value() ||
		   (!relationshipIsManyToOne && !relationshipIsManyToMany))
		{
			res.status = 400;
			res.set_content(make_error("membership_relationship_mismatch", "The configured membership relationship must run from the current screen entity to the Collect View row entity and be Many-to-one or Many-to-many.").dump(2), "application/json");
			return;
		}

		const bool multiSelect = element->collectData->multiSelect.value_or(false);
		if(multiSelect && !relationshipIsManyToMany)
		{
			res.status = 400;
			res.set_content(make_error("membership_cardinality_mismatch", "Multi-select is only valid for a Many-to-many membership relationship.").dump(2), "application/json");
			return;
		}

		if((multiSelect || selectedValue.value()) && targetInstanceId.empty())
		{
			res.status = 400;
			res.set_content(make_error("missing_membership_target", "This membership change requires a target row instance.").dump(2), "application/json");
			return;
		}

		shared_ptr<Instance> targetInstance;
		if(!targetInstanceId.empty())
		{
			targetInstance = dataManager.GetEntityInstanceById(s.ruleset.entities, element->collectEntity.value(), targetInstanceId);
			if(targetInstance == nullptr || !dataManager.IsCollectTargetInScope(s.ruleset.entities, *element, sourceInstance, targetInstanceId))
			{
				res.status = 404;
				res.set_content(make_error("collect_view_row_not_found", "The requested target is not part of this Collect View instance context.").dump(2), "application/json");
				return;
			}
		}

		vector<shared_ptr<Instance>> existingTargets;
		for(const InstanceRelationshipLink& link : sourceInstance->relationshipLinks)
		{
			if(!link.outgoing || link.relationshipId != membershipRelationshipId ||
			   link.sourceEntityId != sourceInstance->entityId || link.sourceInstanceId != sourceInstance->instanceId ||
			   link.targetEntityId != relationship->targetEntity)
			{
				continue;
			}

			shared_ptr<Instance> existingTarget = dataManager.GetEntityInstanceById(s.ruleset.entities, link.targetEntityId, link.targetInstanceId);
			if(existingTarget != nullptr && std::find(existingTargets.begin(), existingTargets.end(), existingTarget) == existingTargets.end())
			{
				existingTargets.push_back(existingTarget);
			}
		}

		vector<shared_ptr<Instance>> snapshotInstances = existingTargets;
		if(targetInstance != nullptr && std::find(snapshotInstances.begin(), snapshotInstances.end(), targetInstance) == snapshotInstances.end())
		{
			snapshotInstances.push_back(targetInstance);
		}
		snapshotInstances.push_back(sourceInstance);

		struct RelationshipStateSnapshot
		{
			shared_ptr<Instance> instance;
			vector<shared_ptr<Instance>> linkedInstances;
			vector<InstanceRelationshipLink> relationshipLinks;
		};
		vector<RelationshipStateSnapshot> snapshots;
		for(const shared_ptr<Instance>& snapshotInstance : snapshotInstances)
		{
			if(snapshotInstance != nullptr)
			{
				snapshots.push_back({snapshotInstance, snapshotInstance->linkedInstances, snapshotInstance->relationshipLinks});
			}
		}

		bool changed = false;
		try
		{
			if(!multiSelect)
			{
				for(const shared_ptr<Instance>& existingTarget : existingTargets)
				{
					changed = RemoveExactRelationshipMembership(sourceInstance, existingTarget, membershipRelationshipId) || changed;
				}
			}
			else if(targetInstance != nullptr && !selectedValue.value())
			{
				changed = RemoveExactRelationshipMembership(sourceInstance, targetInstance, membershipRelationshipId) || changed;
			}

			if(targetInstance != nullptr && selectedValue.value() && !HasExactOutgoingRelationshipLink(sourceInstance, targetInstance, membershipRelationshipId))
			{
				sourceInstance->AddLinkedInstance(targetInstance);
				targetInstance->AddLinkedInstance(sourceInstance);
				sourceInstance->AddRelationshipLink(targetInstance, relationship, true);
				targetInstance->AddRelationshipLink(sourceInstance, relationship, false);
				changed = true;
			}

			SessionModelRuleEvaluator::EvaluationStats evaluationStats;
			if(changed)
			{
				vector<RelationshipMutation> mutations;
				unordered_set<string> mutationKeys;
				AddRelationshipMutation(mutations, mutationKeys, sourceInstance->entityId, sourceInstance->instanceId, membershipRelationshipId);
				evaluationStats = EvaluateRelationshipMutations(s.ruleset, mutations);
			}

			screen = RefreshScreenForInstance(dataManager, s.ruleset, screen, entityId, instanceId, &sessionScreenData);
			bool hasBlockingValidationErrors = false;
			json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);
			out = {
				{"ok", true},
				{"session", {{"token", token}, {"sequence", s.message_count + 1}}},
				{"navigation", {
					{"action", "collectViewMembership"},
					{"fromScreenId", screenId},
					{"toScreenId", screen != nullptr ? screen->id : screenId},
					{"entityId", entityId},
					{"instanceId", instanceId},
					{"blocked", hasBlockingValidationErrors}
				}},
				{"validationEvents", validationEvents},
				{"screen", screen != nullptr ? BuildFrontendScreen(*screen) : json(nullptr)},
				{"screenLocation", screen != nullptr ? BuildScreenLocationJson(*screen, entityId, instanceId, s.message_count + 1) : BuildScreenLocationJson(screenId, entityId, instanceId, s.message_count + 1)},
				{"actions", screen != nullptr ? BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData) : BuildDetachedScreenActionsJson()},
				{"membership", {
					{"elementId", elementId},
					{"relationshipId", membershipRelationshipId},
					{"targetInstanceId", targetInstanceId},
					{"selected", selectedValue.value()},
					{"multiSelect", multiSelect},
					{"changed", changed}
				}},
				{"sessionModelEvaluation", RuleServer::SessionModelEvaluationStatsToJSON(evaluationStats)}
			};

			if(responseOptions.includeUiState && screen != nullptr)
			{
				out["uiState"] = BuildScreenUiState(*screen);
			}
		}
		catch(...)
		{
			for(RelationshipStateSnapshot& snapshot : snapshots)
			{
				snapshot.instance->linkedInstances = snapshot.linkedInstances;
				snapshot.instance->relationshipLinks = snapshot.relationshipLinks;
				snapshot.instance->InvalidateRelationshipLinkIndex();
			}
			throw;
		}
	}
	catch(const std::exception& e)
	{
		res.status = 400;
		res.set_content(make_error("bad_membership_request", std::string("Collect View membership update failed: ") + e.what()).dump(2), "application/json");
		return;
	}

	Session updated;
	if(!touch_session(token, &updated))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
		return;
	}

	WriteEndpointJsonResponse(res, out, responseOptions);
}

void RuleServer::HandlePreviousScreenRequest(const httplib::Request& req, httplib::Response& res)
{
	EndpointMetricsScope metrics("/previousscreen", req, res);

	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	shared_ptr<ScreenFile::Screen> screen;
	ScreenLocation previousLocation;
	std::string requestScreenId;
	EndpointResponseOptions responseOptions = BuildEndpointResponseOptions(req);
	bool useFrontendEnvelope = false;
	ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
	shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);

	try
	{
		auto j = json::parse(req.body);

		const string screenId = first_json_scalar_string(j, {"screenId", "screenid"});
		const string entityId = first_json_scalar_string(j, {"entityId", "entityid"});
		const string instanceId = first_json_scalar_string(j, {"instanceId", "instanceid"});
		const std::optional<std::uint64_t> clientSequence = first_json_sequence(j, {"screenSequence", "sequence"});
		requestScreenId = screenId;
		responseOptions = BuildEndpointResponseOptions(req, &j);
		useFrontendEnvelope = RequestBooleanOption(
			req,
			&j,
			{"frontendEnvelope", "includeEnvelope", "canonical", "envelope"},
			{"X-Frontend-Envelope"},
			"CCP_PREVIOUS_SCREEN_FRONTEND_ENVELOPE").value_or(false);

		if(screenId.empty() || entityId.empty() || instanceId.empty())
		{
			res.status = 400;

			res.set_content(make_error("missing_previous_screen_context", "Previous-screen requests require screenId, entityId, and instanceId.").dump(2), "application/json");

			return;
		}

		if(ScreenRequestIsStale(s, clientSequence, screenId, entityId, instanceId))
		{
			WriteStaleScreenResponse(token, s, clientSequence, res, responseOptions);

			return;
		}

		if(s.ruleset.screenLocations == nullptr || s.ruleset.screenLocations->size() < 2)
		{
			res.status = 404;

			res.set_content(make_error("previous_screen_not_found", "No previous screen is available for this session.").dump(2), "application/json");

			return;
		}

		DataManager dm = DataManager();

		shared_ptr<ScreenFile::Screen> currentScreen = sessionScreenData.GetScreenByID(screenId);

		if(currentScreen == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("screen_not_found", "Current screen not found.").dump(2), "application/json");

			return;
		}

		shared_ptr<Instance> currentInstance = dm.GetEntityInstanceById(s.ruleset.entities, entityId, instanceId);

		if(currentInstance == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("instance_not_found", "Current screen instance not found.").dump(2), "application/json");

			return;
		}

		ScreenLocation loc = s.ruleset.GetPreviousScreenLocation();

		if(loc.screenId.empty() || loc.entityId.empty() || loc.instanceId.empty())
		{
			res.status = 404;

			res.set_content(make_error("previous_screen_not_found", "No previous screen is available for this session.").dump(2), "application/json");

			return;
		}

		shared_ptr<Instance> previousInstance = dm.GetEntityInstanceById(s.ruleset.entities, loc.entityId, loc.instanceId);

		if(previousInstance == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("instance_not_found", "Previous screen instance not found.").dump(2), "application/json");

			return;
		}

		previousLocation = loc;
		screen = dm.GetScreen(s.ruleset.entities, sessionScreenDataView, loc, BorrowRuleSet(s.ruleset));

		if(screen == nullptr)
		{
			res.status = 404;

			res.set_content(make_error("screen_not_found", "Previous screen not found.").dump(2), "application/json");

			return;
		}

		dm.SetDisplayElement(s.ruleset.entities, screen);
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	if(screen != nullptr)
	{
		if(useFrontendEnvelope)
		{
			const std::uint64_t responseSequence = updated.message_count;
			bool hasBlockingValidationErrors = false;
			json validationEvents = BuildActiveValidationEventsJson(s.ruleset, hasBlockingValidationErrors);
			json out = {
				{"ok", true},
				{"session", {
					{"token", token},
					{"sequence", responseSequence}
				}},
				{"navigation", {
					{"action", "previous"},
					{"fromScreenId", requestScreenId},
					{"toScreenId", screen->id},
					{"entityId", previousLocation.entityId},
					{"instanceId", previousLocation.instanceId},
					{"blocked", hasBlockingValidationErrors}
				}},
				{"validationEvents", validationEvents},
				{"screen", BuildFrontendScreen(*screen)},
				{"screenLocation", BuildScreenLocationJson(*screen, previousLocation.entityId, previousLocation.instanceId, responseSequence)},
				{"actions", BuildScreenActionsJson(s.ruleset, *screen, sessionScreenData)}
			};

			if(responseOptions.includeUiState)
			{
				out["uiState"] = BuildScreenUiState(*screen);
			}

			WriteEndpointJsonResponse(res, out, responseOptions);

			return;
		}

		string j = SerializeScreenJson(screen);

		res.set_content(j, "application/json");
	}
}

void RuleServer::HandlePdfResolveRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;
		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");
		return;
	}

	Session session;
	if(!try_get_session(token, session))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
		return;
	}

	try
	{
		const json body = json::parse(req.body);
		if(!body.is_object() || !body.contains("templateId") || !body.at("templateId").is_string())
		{
			res.status = 400;
			res.set_content(make_error("pdf_template_id_required", "Send templateId as a string.").dump(2), "application/json");
			return;
		}

		const std::string templateId = TrimAscii(body.at("templateId").get<std::string>());
		PdfTemplateResolver::Result resolution = PdfTemplateResolver::Resolve(session.ruleset, templateId);
		if(!resolution.succeeded)
		{
			json error = make_error("pdf_template_invalid", "The PDF template could not be resolved against the active session.");
			error["diagnostics"] = resolution.DiagnosticsJson();
			res.status = 422;
			res.set_header("Cache-Control", "no-store");
			res.set_content(error.dump(2), "application/json");
			return;
		}

		json output = {
			{"ok", true},
			{"document", resolution.document}
		};
		res.status = 200;
		res.set_header("Cache-Control", "no-store");
		res.set_content(output.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;
		res.set_content(make_error("bad_json", std::string("Invalid PDF resolve request: ") + e.what()).dump(2), "application/json");
		return;
	}

	Session updated;
	if(!touch_session(token, &updated))
	{
		res.status = 401;
		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");
	}
}

void RuleServer::HandleAvailableRecordsRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	try
	{
		json requestJson = json::object();

		if(!TrimAscii(req.body).empty())
		{
			requestJson = json::parse(req.body);
		}

		if(!requestJson.is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_request_shape", "Available-records requests must be JSON objects when a body is supplied.").dump(2), "application/json");
			return;
		}

		const FormLaunchSelection formSelection = ParseFormLaunchSelection(requestJson);

		if(formSelection.requested)
		{
			SelectedFormSessionModelLoadResult selectedFormLoad = ApplySelectedFormSelectionToSession(s, formSelection);

			if(WriteSelectedFormLoadFailure(res, selectedFormLoad))
			{
				return;
			}
		}

		if(formSelection.requested || s.externalIdentity.enabled || s.ruleset.externalSecurity.enabled)
		{
			const SessionAuthenticationModeValidation modeValidation = ValidateSessionAuthenticationModeForCurrentRuntime(s);
			if(WriteSessionAuthenticationModeFailure(res, modeValidation))
			{
				return;
			}
		}

		const int maxRows = DataConnectionAvailableRecordsMaxRows(requestJson);
		json sessionJson = s.ruleset.ToJSON();
		json out = {
			{"ok", true},
			{"message", std::string()},
			{"userId", s.ruleset.sessionContext.userId},
			{"userName", s.ruleset.sessionContext.userName},
			{"maxRows", maxRows},
			{"hydrationAttempted", false},
			{"hydrated", false},
			{"records", json::array()},
			{"recordCount", 0},
			{"diagnostics", json::array()}
		};

		if(s.selectedForm.selected)
		{
			out["form"] = SelectedFormRuntimeToJson(s.selectedForm);
		}

		if(!sessionJson.contains("sessionModel") || !sessionJson.at("sessionModel").is_object())
		{
			out["message"] = "No session model is loaded, so no data-connection records are available.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"warning",
				"available_records_session_model_missing",
				"No session model is loaded, so no data-connection records are available.",
				"sessionModel");

			res.set_content(out.dump(2), "application/json");
			return;
		}

		const json loadPlan = sessionJson.at("sessionModel").value("dataConnectionLoadPlan", json::object());

		out["loadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(loadPlan));
		out["loadPlanFieldCount"] = static_cast<int>(DataConnectionLoadPlanFieldCount(loadPlan));
		out["loadPlanTargetCount"] = static_cast<int>(DataConnectionLoadPlanTargetCount(loadPlan));
		out["loadPlanDiagnosticCount"] = static_cast<int>(DataConnectionLoadPlanDiagnosticCount(loadPlan));

		if(!DataConnectionLoadPlanHasContent(loadPlan) || DataConnectionLoadPlanTableCount(loadPlan) == 0)
		{
			out["message"] = "No data-connection load-plan tables are available to list records.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"warning",
				"available_records_load_plan_empty",
				"No data-connection load-plan tables are available to list records.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.set_content(out.dump(2), "application/json");
			return;
		}

		if(DataConnectionLoadPlanHasErrorDiagnostics(loadPlan))
		{
			out["ok"] = false;
			out["message"] = "Data-connection records cannot be listed while the load plan has errors.";
			out["loadPlanDiagnostics"] = loadPlan.value("diagnostics", json::array());
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"available_records_load_plan_has_errors",
				"Data-connection records cannot be listed while the load plan has errors.",
				"sessionModel.dataConnectionLoadPlan.diagnostics");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		const json availableRecordsLoadPlan = BuildSessionUserScopedLoadPlan(loadPlan);
		out["availableRecordsLoadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(availableRecordsLoadPlan));

		if(DataConnectionLoadPlanTableCount(availableRecordsLoadPlan) == 0)
		{
			out["ok"] = false;
			out["message"] = "Data-connection records were not listed because every load table must be scoped by the signed-in user id.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"available_records_user_scope_missing",
				"Data-connection records were not listed because no selectable load table includes a SessionUserId load filter.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		json execution = BuildDataConnectionLoadPlanExecutionResult(availableRecordsLoadPlan, maxRows);
		json records = BuildAvailableRecordsFromLoadExecution(execution);

		out["execution"] = execution;
		out["records"] = records;
		out["recordCount"] = static_cast<int>(records.size());
		out["diagnostics"] = execution.value("diagnostics", json::array());
		out["ok"] = execution.value("succeeded", false);
		out["message"] = execution.value("message", std::string());

		if(!out.value("ok", false))
		{
			res.status = execution.value("attempted", false) ? 502 : 409;
		}

		res.set_content(out.dump(2), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");
	}
}

void RuleServer::HandleLoadRecordRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	bool selectedFormLoaded = false;

	try
	{
		json requestJson = json::object();

		if(!TrimAscii(req.body).empty())
		{
			requestJson = json::parse(req.body);
		}

		if(!requestJson.is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_request_shape", "Load record requests must be JSON objects.").dump(2), "application/json");
			return;
		}

		const json* requestedRecordIdentityPtr = DataConnectionRequestedRecordIdentity(requestJson);

		if(DataConnectionRecordIdHasInvalidShape(requestJson))
		{
			res.status = 400;
			res.set_content(make_error("invalid_record_id", "Load record requests require recordId to be a string when supplied.").dump(2), "application/json");
			return;
		}

		if(requestedRecordIdentityPtr != nullptr && !requestedRecordIdentityPtr->is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_record_identity", "Load record requests require recordIdentity to be an object when supplied.").dump(2), "application/json");
			return;
		}

		const std::string recordId = DataConnectionSelectedRecordId(requestJson);

		if(recordId.empty())
		{
			res.status = 400;
			res.set_content(make_error("missing_record_id", "Load record requests require a recordId.").dump(2), "application/json");
			return;
		}

		const FormLaunchSelection formSelection = ParseFormLaunchSelection(requestJson);

		if(formSelection.requested)
		{
			SelectedFormSessionModelLoadResult selectedFormLoad = ApplySelectedFormSelectionToSession(s, formSelection);

			if(WriteSelectedFormLoadFailure(res, selectedFormLoad))
			{
				return;
			}

			selectedFormLoaded = true;
		}

		if(formSelection.requested || s.externalIdentity.enabled || s.ruleset.externalSecurity.enabled)
		{
			const SessionAuthenticationModeValidation modeValidation = ValidateSessionAuthenticationModeForCurrentRuntime(s);
			if(WriteSessionAuthenticationModeFailure(res, modeValidation))
			{
				return;
			}
		}

		const int maxRows = DataConnectionAvailableRecordsMaxRows(requestJson);
		json sessionJson = s.ruleset.ToJSON();
		json out = {
			{"ok", true},
			{"message", std::string()},
			{"recordId", recordId},
			{"userId", s.ruleset.sessionContext.userId},
			{"userName", s.ruleset.sessionContext.userName},
			{"maxRows", maxRows},
			{"hydrationAttempted", false},
			{"hydrated", false},
			{"diagnostics", json::array()}
		};

		if(s.selectedForm.selected)
		{
			out["form"] = SelectedFormRuntimeToJson(s.selectedForm);
		}

		if(!sessionJson.contains("sessionModel") || !sessionJson.at("sessionModel").is_object())
		{
			out["ok"] = false;
			out["message"] = "No session model is loaded, so no data-connection records can be loaded.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"warning",
				"load_record_session_model_missing",
				"No session model is loaded, so no data-connection records can be loaded.",
				"sessionModel");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		const json loadPlan = sessionJson.at("sessionModel").value("dataConnectionLoadPlan", json::object());

		out["loadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(loadPlan));
		out["loadPlanFieldCount"] = static_cast<int>(DataConnectionLoadPlanFieldCount(loadPlan));
		out["loadPlanTargetCount"] = static_cast<int>(DataConnectionLoadPlanTargetCount(loadPlan));
		out["loadPlanDiagnosticCount"] = static_cast<int>(DataConnectionLoadPlanDiagnosticCount(loadPlan));

		if(!DataConnectionLoadPlanHasContent(loadPlan) || DataConnectionLoadPlanTableCount(loadPlan) == 0)
		{
			out["ok"] = false;
			out["message"] = "No data-connection load-plan tables are available to load a selected record.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"warning",
				"load_record_load_plan_empty",
				"No data-connection load-plan tables are available to load a selected record.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		if(DataConnectionLoadPlanHasErrorDiagnostics(loadPlan))
		{
			out["ok"] = false;
			out["message"] = "A selected record cannot be loaded while the load plan has errors.";
			out["loadPlanDiagnostics"] = loadPlan.value("diagnostics", json::array());
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"load_record_load_plan_has_errors",
				"A selected record cannot be loaded while the load plan has errors.",
				"sessionModel.dataConnectionLoadPlan.diagnostics");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		const json availableRecordsLoadPlan = BuildSessionUserScopedLoadPlan(loadPlan);
		out["availableRecordsLoadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(availableRecordsLoadPlan));

		if(DataConnectionLoadPlanTableCount(availableRecordsLoadPlan) == 0)
		{
			out["ok"] = false;
			out["message"] = "The selected record was not loaded because every load table must be scoped by the signed-in user id.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"load_record_user_scope_missing",
				"The selected record was not loaded because no selectable load table includes a SessionUserId load filter.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		json availableRecordsExecution = BuildDataConnectionLoadPlanExecutionResult(availableRecordsLoadPlan, maxRows);
		json records = BuildAvailableRecordsFromLoadExecution(availableRecordsExecution);
		const json requestedRecordIdentity = requestedRecordIdentityPtr != nullptr ? *requestedRecordIdentityPtr : json::object();

		out["recordCount"] = static_cast<int>(records.size());
		out["diagnostics"] = availableRecordsExecution.value("diagnostics", json::array());
		if(!requestedRecordIdentity.empty())
		{
			out["requestRecordIdentity"] = requestedRecordIdentity;
		}

		if(!availableRecordsExecution.value("succeeded", false))
		{
			out["ok"] = false;
			out["message"] = availableRecordsExecution.value("message", std::string("The data-connection load execution failed."));
			out["execution"] = availableRecordsExecution;
			res.status = availableRecordsExecution.value("attempted", false) ? 502 : 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		int selectedOperationIndex = -1;
		int selectedRowIndex = -1;
		json selectedRecord;
		bool ambiguousRecordId = false;

		if(!TryFindAvailableRecordLocation(records, recordId, selectedOperationIndex, selectedRowIndex, selectedRecord, &ambiguousRecordId))
		{
			out["ok"] = false;
			out["records"] = records;
			if(ambiguousRecordId)
			{
				out["message"] = "The selected data-connection record id matched more than one row for the signed-in user.";
				AppendDataConnectionDryRunDiagnostic(
					out,
					"error",
					"load_record_ambiguous",
					"The selected data-connection record id matched more than one row for the signed-in user.",
					"recordId");

				res.status = 409;
			}
			else
			{
				out["message"] = "The selected data-connection record was not found for the signed-in user.";
				AppendDataConnectionDryRunDiagnostic(
					out,
					"warning",
					"load_record_not_found",
					"The selected data-connection record was not found for the signed-in user.",
					"recordId");

				res.status = 404;
			}

			res.set_content(out.dump(2), "application/json");
			return;
		}

		json selectedLoadPlan = BuildSelectedRecordLoadPlan(loadPlan, selectedRecord, s.ruleset);
		out["selectedLoadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(selectedLoadPlan));

		if(DataConnectionLoadPlanTableCount(selectedLoadPlan) == 0)
		{
			out["ok"] = false;
			out["message"] = "The selected data-connection record could not be converted into a scoped load plan.";
			out["selectedRecord"] = selectedRecord;
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"load_record_selected_plan_empty",
				"The selected data-connection record could not be converted into a scoped load plan.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		json execution = BuildDataConnectionLoadPlanExecutionResult(selectedLoadPlan, maxRows);
		json selectedRow;

		if(!FilterLoadExecutionToSelectedRecordIdentity(execution, selectedRecord, selectedRow))
		{
			out["ok"] = false;
			out["message"] = "The selected data-connection record could not be isolated from the selected load execution response.";
			out["selectedRecord"] = selectedRecord;
			out["execution"] = execution;
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"load_record_isolation_failed",
				"The selected data-connection record could not be isolated from the selected load execution response.",
				"sessionModel.dataConnectionLoadExecution.operations");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		execution["selectedOperationIndex"] = selectedOperationIndex;
		execution["selectedRowIndex"] = selectedRowIndex;
		execution["selectedRecordId"] = recordId;

		ApplyDataConnectionLoadHydration(s.ruleset, execution);

		out = s.ruleset.ToJSON();
		out["ok"] = execution.value("hydrated", false);
		out["message"] = execution.value("message", std::string());
		out["recordId"] = recordId;
		out["selectedRecord"] = selectedRecord;
		out["selectedRow"] = selectedRow;
		out["recordIdentity"] = selectedRecord.value("recordIdentity", json::object());
		if(!requestedRecordIdentity.empty())
		{
			out["requestRecordIdentity"] = requestedRecordIdentity;
		}
		out["userId"] = s.ruleset.sessionContext.userId;
		out["userName"] = s.ruleset.sessionContext.userName;
		out["maxRows"] = maxRows;
		out["availableRecordsLoadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(availableRecordsLoadPlan));
		out["selectedLoadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(selectedLoadPlan));
		out["hydrationAttempted"] = execution.value("hydrationAttempted", false);
		out["hydrated"] = execution.value("hydrated", false);
		out["hydrationRefused"] = execution.value("hydrationRefused", false);
		out["hydratedAttributeCount"] = execution.value("hydratedAttributeCount", 0);
		out["diagnostics"] = execution.value("diagnostics", json::array());
		if(s.selectedForm.selected)
		{
			out["form"] = SelectedFormRuntimeToJson(s.selectedForm);
		}

		if(!out.contains("sessionModel") || !out.at("sessionModel").is_object())
		{
			out["sessionModel"] = json::object();
		}

		out["sessionModel"]["dataConnectionLoadExecuteRequested"] = true;
		out["sessionModel"]["dataConnectionLoadHydrateRequested"] = true;
		out["sessionModel"]["dataConnectionLoadExecution"] = execution;

		if(!execution.value("hydrated", false))
		{
			res.status = 409;
		}

		res.set_content(out.dump(4), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	const bool stored = store_session_runtime(token, s, s.message_count, &updated);

	if(!stored)
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}
}

void RuleServer::HandleDeleteRecordRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	try
	{
		json requestJson = json::object();

		if(!TrimAscii(req.body).empty())
		{
			requestJson = json::parse(req.body);
		}

		if(!requestJson.is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_request_shape", "Delete record requests must be JSON objects.").dump(2), "application/json");
			return;
		}

		const json* requestedRecordIdentityPtr = DataConnectionRequestedRecordIdentity(requestJson);

		if(DataConnectionRecordIdHasInvalidShape(requestJson))
		{
			res.status = 400;
			res.set_content(make_error("invalid_record_id", "Delete record requests require recordId to be a string when supplied.").dump(2), "application/json");
			return;
		}

		if(requestedRecordIdentityPtr != nullptr && !requestedRecordIdentityPtr->is_object())
		{
			res.status = 400;
			res.set_content(make_error("invalid_record_identity", "Delete record requests require recordIdentity to be an object when supplied.").dump(2), "application/json");
			return;
		}

		const std::string recordId = DataConnectionSelectedRecordId(requestJson);

		if(recordId.empty())
		{
			res.status = 400;
			res.set_content(make_error("missing_record_id", "Delete record requests require a recordId.").dump(2), "application/json");
			return;
		}

		const FormLaunchSelection formSelection = ParseFormLaunchSelection(requestJson);

		if(formSelection.requested)
		{
			SelectedFormSessionModelLoadResult selectedFormLoad = ApplySelectedFormSelectionToSession(s, formSelection);

			if(WriteSelectedFormLoadFailure(res, selectedFormLoad))
			{
				return;
			}
		}

		if(formSelection.requested || s.externalIdentity.enabled || s.ruleset.externalSecurity.enabled)
		{
			const SessionAuthenticationModeValidation modeValidation = ValidateSessionAuthenticationModeForCurrentRuntime(s);
			if(WriteSessionAuthenticationModeFailure(res, modeValidation))
			{
				return;
			}
		}

		const int maxRows = DataConnectionAvailableRecordsMaxRows(requestJson);
		json sessionJson = s.ruleset.ToJSON();
		json out = {
			{"ok", true},
			{"message", std::string()},
			{"recordId", recordId},
			{"userId", s.ruleset.sessionContext.userId},
			{"userName", s.ruleset.sessionContext.userName},
			{"maxRows", maxRows},
			{"deleted", false},
			{"deletedCount", 0},
			{"diagnostics", json::array()}
		};

		if(s.selectedForm.selected)
		{
			out["form"] = SelectedFormRuntimeToJson(s.selectedForm);
		}

		if(!sessionJson.contains("sessionModel") || !sessionJson.at("sessionModel").is_object())
		{
			out["ok"] = false;
			out["message"] = "No session model is loaded, so no data-connection records can be deleted.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"warning",
				"delete_record_session_model_missing",
				"No session model is loaded, so no data-connection records can be deleted.",
				"sessionModel");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		const json loadPlan = sessionJson.at("sessionModel").value("dataConnectionLoadPlan", json::object());

		out["loadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(loadPlan));
		out["loadPlanFieldCount"] = static_cast<int>(DataConnectionLoadPlanFieldCount(loadPlan));
		out["loadPlanTargetCount"] = static_cast<int>(DataConnectionLoadPlanTargetCount(loadPlan));
		out["loadPlanDiagnosticCount"] = static_cast<int>(DataConnectionLoadPlanDiagnosticCount(loadPlan));

		if(!DataConnectionLoadPlanHasContent(loadPlan) || DataConnectionLoadPlanTableCount(loadPlan) == 0)
		{
			out["ok"] = false;
			out["message"] = "No data-connection load-plan tables are available to delete a selected record.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"warning",
				"delete_record_load_plan_empty",
				"No data-connection load-plan tables are available to delete a selected record.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		if(DataConnectionLoadPlanHasErrorDiagnostics(loadPlan))
		{
			out["ok"] = false;
			out["message"] = "A selected record cannot be deleted while the load plan has errors.";
			out["loadPlanDiagnostics"] = loadPlan.value("diagnostics", json::array());
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"delete_record_load_plan_has_errors",
				"A selected record cannot be deleted while the load plan has errors.",
				"sessionModel.dataConnectionLoadPlan.diagnostics");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		const json availableRecordsLoadPlan = BuildSessionUserScopedLoadPlan(loadPlan);
		out["availableRecordsLoadPlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(availableRecordsLoadPlan));

		if(DataConnectionLoadPlanTableCount(availableRecordsLoadPlan) == 0)
		{
			out["ok"] = false;
			out["message"] = "The selected record was not deleted because every load table must be scoped by the signed-in user id.";
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"delete_record_user_scope_missing",
				"The selected record was not deleted because no selectable load table includes a SessionUserId load filter.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		json availableRecordsExecution = BuildDataConnectionLoadPlanExecutionResult(availableRecordsLoadPlan, maxRows);
		json records = BuildAvailableRecordsFromLoadExecution(availableRecordsExecution);
		const json requestedRecordIdentity = requestedRecordIdentityPtr != nullptr ? *requestedRecordIdentityPtr : json::object();

		out["recordCount"] = static_cast<int>(records.size());
		out["diagnostics"] = availableRecordsExecution.value("diagnostics", json::array());
		if(!requestedRecordIdentity.empty())
		{
			out["requestRecordIdentity"] = requestedRecordIdentity;
		}

		if(!availableRecordsExecution.value("succeeded", false))
		{
			out["ok"] = false;
			out["message"] = availableRecordsExecution.value("message", std::string("The data-connection load execution failed."));
			out["execution"] = availableRecordsExecution;
			res.status = availableRecordsExecution.value("attempted", false) ? 502 : 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		int selectedOperationIndex = -1;
		int selectedRowIndex = -1;
		json selectedRecord;
		bool ambiguousRecordId = false;

		if(!TryFindAvailableRecordLocation(records, recordId, selectedOperationIndex, selectedRowIndex, selectedRecord, &ambiguousRecordId))
		{
			out["ok"] = false;
			out["records"] = records;
			if(ambiguousRecordId)
			{
				out["message"] = "The selected data-connection record id matched more than one row for the signed-in user.";
				AppendDataConnectionDryRunDiagnostic(
					out,
					"error",
					"delete_record_ambiguous",
					"The selected data-connection record id matched more than one row for the signed-in user.",
					"recordId");

				res.status = 409;
			}
			else
			{
				out["message"] = "The selected data-connection record was not found for the signed-in user.";
				AppendDataConnectionDryRunDiagnostic(
					out,
					"warning",
					"delete_record_not_found",
					"The selected data-connection record was not found for the signed-in user.",
					"recordId");

				res.status = 404;
			}

			res.set_content(out.dump(2), "application/json");
			return;
		}

		json deletePlan = BuildSelectedRecordDeletePlan(loadPlan, selectedRecord, s.ruleset);
		out["selectedDeletePlanTableCount"] = static_cast<int>(DataConnectionLoadPlanTableCount(deletePlan));

		if(DataConnectionLoadPlanTableCount(deletePlan) == 0)
		{
			out["ok"] = false;
			out["message"] = "The selected data-connection record could not be converted into a scoped delete plan.";
			out["selectedRecord"] = selectedRecord;
			AppendDataConnectionDryRunDiagnostic(
				out,
				"error",
				"delete_record_selected_plan_empty",
				"The selected data-connection record could not be converted into a scoped delete plan.",
				"sessionModel.dataConnectionLoadPlan.tables");

			res.status = 409;
			res.set_content(out.dump(2), "application/json");
			return;
		}

		json deleteExecution = BuildDataConnectionDeletePlanExecutionResult(deletePlan);

		out["ok"] = deleteExecution.value("succeeded", false);
		out["message"] = deleteExecution.value("message", std::string());
		out["recordId"] = recordId;
		out["selectedRecord"] = selectedRecord;
		out["recordIdentity"] = selectedRecord.value("recordIdentity", json::object());
		out["selectedOperationIndex"] = selectedOperationIndex;
		out["selectedRowIndex"] = selectedRowIndex;
		out["deleted"] = deleteExecution.value("succeeded", false)
			&& deleteExecution.value("deleteExecuted", false)
			&& deleteExecution.value("committed", false);
		out["deletedCount"] = deleteExecution.value("deletedCount", 0);
		out["dataConnectionDeletePlan"] = deletePlan;
		out["dataConnectionDeleteExecution"] = deleteExecution;
		out["diagnostics"] = deleteExecution.value("diagnostics", json::array());
		if(s.selectedForm.selected)
		{
			out["form"] = SelectedFormRuntimeToJson(s.selectedForm);
		}

		if(!out.value("ok", false))
		{
			res.status = deleteExecution.value("attempted", false) ? 502 : 409;
		}

		res.set_content(out.dump(4), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}
}

void RuleServer::HandleSaveRequest(const httplib::Request& req, httplib::Response& res)
{
	const std::string token = extract_token(req);

	if(token.empty())
	{
		res.status = 401;

		res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

		return;
	}

	Session s;

	if(!try_get_session(token, s))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}

	try
	{
		json requestJson = json::object();

		if(!TrimAscii(req.body).empty())
		{
			requestJson = json::parse(req.body);
		}

		const bool dataConnectionCommitRequested = DataConnectionCommitRequested(requestJson);
		const bool dataConnectionLoadExecuteRequested = DataConnectionLoadExecuteRequested(requestJson);
		const bool dataConnectionLoadHydrateRequested = DataConnectionLoadHydrateRequested(requestJson);
		const std::optional<int> dataConnectionLoadMaxRows = DataConnectionLoadExecuteMaxRows(requestJson);
		json out = s.ruleset.ToJSON();

		if(out.contains("sessionModel") && out.at("sessionModel").is_object())
		{
			const json loadPlan = out.at("sessionModel").value("dataConnectionLoadPlan", json::object());

			if(DataConnectionLoadPlanHasContent(loadPlan))
			{
				out["sessionModel"]["dataConnectionLoadExecuteRequested"] = dataConnectionLoadExecuteRequested;
				out["sessionModel"]["dataConnectionLoadHydrateRequested"] = dataConnectionLoadHydrateRequested;

				if(dataConnectionLoadExecuteRequested)
				{
					json loadExecution = BuildDataConnectionLoadPlanExecutionResult(loadPlan, dataConnectionLoadMaxRows);

					if(dataConnectionLoadHydrateRequested)
					{
						ApplyDataConnectionLoadHydration(s.ruleset, loadExecution);
						out = s.ruleset.ToJSON();
					}
					else
					{
						loadExecution["hydrateRequested"] = false;
						loadExecution["hydrationAttempted"] = false;
						loadExecution["hydrated"] = false;
						loadExecution["hydrationRefused"] = false;
						loadExecution["hydratedAttributeCount"] = 0;
					}

					out["sessionModel"]["dataConnectionLoadExecuteRequested"] = dataConnectionLoadExecuteRequested;
					out["sessionModel"]["dataConnectionLoadHydrateRequested"] = dataConnectionLoadHydrateRequested;
					out["sessionModel"]["dataConnectionLoadExecution"] = loadExecution;
				}
				else
				{
					out["sessionModel"]["dataConnectionLoadDryRun"] = BuildDataConnectionLoadPlanDryRunResult(loadPlan);
				}
			}

			const json savePlan = out.at("sessionModel").value("dataConnectionSavePlan", json::object());
			const json deletedInstanceDeletePlan = out.at("sessionModel").value("dataConnectionDeletedInstanceDeletePlan", json::object());
			const bool hasSavePlan = DataConnectionSavePlanHasContent(savePlan);
			const bool hasDeletedInstanceDeletePlan = DataConnectionLoadPlanHasContent(deletedInstanceDeletePlan);

			if(hasSavePlan || hasDeletedInstanceDeletePlan)
			{
				out["sessionModel"]["dataConnectionSaveCommitRequested"] = dataConnectionCommitRequested;
				out["sessionModel"]["dataConnectionDeletedInstanceDeleteRequested"] = dataConnectionCommitRequested && hasDeletedInstanceDeletePlan;

				if(dataConnectionCommitRequested)
				{
					const json preservedLoadExecution = out["sessionModel"].value("dataConnectionLoadExecution", json());
					const json preservedLoadDryRun = out["sessionModel"].value("dataConnectionLoadDryRun", json());
					const bool hadLoadExecution = out["sessionModel"].contains("dataConnectionLoadExecution");
					const bool hadLoadDryRun = out["sessionModel"].contains("dataConnectionLoadDryRun");

					json savePreview = BuildDataConnectionSavePreviewJson(&s.ruleset, savePlan, true);

					out = s.ruleset.ToJSON();

					out["sessionModel"]["dataConnectionLoadExecuteRequested"] = dataConnectionLoadExecuteRequested;
					out["sessionModel"]["dataConnectionLoadHydrateRequested"] = dataConnectionLoadHydrateRequested;
					if(hadLoadExecution)
					{
						out["sessionModel"]["dataConnectionLoadExecution"] = preservedLoadExecution;
					}
					if(hadLoadDryRun)
					{
						out["sessionModel"]["dataConnectionLoadDryRun"] = preservedLoadDryRun;
					}
					out["sessionModel"]["dataConnectionSaveCommitRequested"] = dataConnectionCommitRequested;
					out["sessionModel"]["dataConnectionDeletedInstanceDeleteRequested"] = hasDeletedInstanceDeletePlan;
					if(savePreview.contains("execution"))
					{
						out["sessionModel"]["dataConnectionSaveExecution"] = savePreview["execution"];
					}
					if(savePreview.contains("deletePlan"))
					{
						out["sessionModel"]["dataConnectionDeletedInstanceDeletePlan"] = savePreview["deletePlan"];
					}
					if(savePreview.contains("deleteExecution"))
					{
						out["sessionModel"]["dataConnectionDeletedInstanceDeleteExecution"] = savePreview["deleteExecution"];
					}
				}
				else
				{
					json savePreview = BuildDataConnectionSavePreviewJson(&s.ruleset, savePlan, false);
					if(savePreview.contains("dryRun"))
					{
						out["sessionModel"]["dataConnectionSaveDryRun"] = savePreview["dryRun"];
					}
					if(savePreview.contains("deletePlan"))
					{
						out["sessionModel"]["dataConnectionDeletedInstanceDeletePlan"] = savePreview["deletePlan"];
					}
				}
			}
		}

		res.set_content(out.dump(4), "application/json");
	}
	catch(const std::exception& e)
	{
		res.status = 400;

		res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

		return;
	}

	Session updated;

	if(!touch_session(token, &updated))
	{
		res.status = 401;

		res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

		return;
	}
}

int RuleServer::StartServer()
{
	httplib::Server server;

	const int threadCount = get_env_int_or_default("CCP_THREAD_COUNT", 8, 1, 64);

	server.new_task_queue = [threadCount] {
		return new httplib::ThreadPool(threadCount);
	};

	server.Options(R"(.*)", [](const httplib::Request& req, httplib::Response& res) {
		static std::atomic<std::uint64_t> counter{0};

		RuleServer::ApplyCorsHeaders(req, res);

		//if((++counter % 25) == 0)
		//{
			remove_expired_sessions();
			remove_expired_debug_sessions();
		//}

		res.status = 200; // No Content
	});

	server.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
		RuleServer::ApplyCorsHeaders(req, res);
	});
	
	server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
		static std::atomic<std::uint64_t> counter{0};

		//if((++counter % 25) == 0)
		//{
			remove_expired_sessions();
			remove_expired_debug_sessions();
		//}

		return httplib::Server::HandlerResponse::Unhandled;
	});
	
	server.Get("/health", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(json{{"ok", true}, {"time_utc", now_iso8601_utc()}, {"buildConfiguration", build_configuration_name()}}.dump(2), "application/json");
	});

	server.Get("/debug/capabilities", RuleServer::HandleDebugCapabilitiesRequest);
	server.Post("/debug/session-model/validate", RuleServer::HandleDebugSessionModelValidateRequest);
	server.Post("/debug/session/start", RuleServer::HandleDebugSessionStartRequest);
	server.Post("/debug/session/evaluate", RuleServer::HandleDebugSessionEvaluateRequest);
	server.Post("/debug/session/end", RuleServer::HandleDebugSessionEndRequest);

	server.Post("/login", RuleServer::HandleLoginRequest);
	server.Post("/external-login", RuleServer::HandleExternalLoginRequest);
	server.Get("/forms", RuleServer::HandleFormsRequest);
	server.Post("/forms", RuleServer::HandleFormsRequest);
	server.Get("/available-forms", RuleServer::HandleFormsRequest);
	server.Post("/available-forms", RuleServer::HandleFormsRequest);
	server.Post("/start-inputs", RuleServer::HandleStartInputsRequest);
	server.Post("/reload-session-model", RuleServer::HandleReloadSessionModelRequest);
	server.Post("/reloadsessionmodel", RuleServer::HandleReloadSessionModelRequest);
	server.Post("/sessioncontext", RuleServer::HandleSessionContextRequest);
	server.Post("/sessionhistory", RuleServer::HandleSessionHistoryRequest);

	// Who am I: GET /me (requires token)
	server.Get("/me", RuleServer::HandleMeRequest);

	server.Post("/rulestatus", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		try
		{
			if(!req.body.empty())
			{
				auto j = json::parse(req.body);

				auto arr = j["display-rules"];

				auto entityId = j.value("entityid", std::string());
				auto instanceId = j.value("instanceid", std::string());

				for(auto& item : arr)
				{
					const string ruleId = item.value("ruleId", std::string());
					const string controlId = item.value("controlId", std::string());
					bool currentState = item.value("currentState", false);
					bool hasChanged = item.value("hasChanged", false);

					for(const auto& entity : *s.ruleset.entities)
					{
						if(entity->entityid == entityId)
						{
							for(const auto& instance : entity->instances)
							{
								if(instance->instanceId == instanceId)
								{
									for(const auto& rule : instance->rules)
									{
										if(rule->GetID() == ruleId)
										{
											bool newState = rule->GetState();

											if(currentState != newState)
											{
												item["currentState"] = newState;

												item["hasChanged"] = true;
											}
											else
											{
												int h = 10;
											}

											break;
										}
									}

									break;
								}
							}

							break;
						}
					}
				}
				
				string str = "{ \"display-rules\": " + arr.dump(4) + " }";

				res.set_content(str, "application/json");
			}
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}
	});

	server.Post("/element", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		try
		{
			if(!req.body.empty())
			{
				auto j = json::parse(req.body);

				string screenId = j["screenId"];
				string elementId = j["elementId"];
				string entityId = j["entityId"];
				string instanceId = j["instanceId"];

				ScreenLocation loc = ScreenLocation();

				loc.screenId = screenId;
				loc.entityId = entityId;
				loc.instanceId = instanceId;

				DataManager dm = DataManager();
				ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
				shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);

				shared_ptr<ScreenFile::Screen> screen = dm.GetScreen(s.ruleset.entities, sessionScreenDataView, loc, BorrowRuleSet(s.ruleset));

				shared_ptr<ScreenFile::Element> element = dm.GetElementById(screen, elementId);

				if(screen == nullptr || element == nullptr)
				{
					res.status = 404;

					res.set_content(make_error("not_found", "Screen or element not found.").dump(2), "application/json");

					return;
				}

				string str = SerializeElementJson(element);

				res.set_content(str, "application/json");

				//string str = "{ \"display-rules\": " + arr.dump(4) + " }";

				//res.set_content(str, "application/json");
			}
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}
	});

	server.Post("/update", RuleServer::HandleUpdateRequest);
	server.Post("/submitscreen", RuleServer::HandleSubmitScreenRequest);
	server.Post("/startsession", RuleServer::HandleStartSessionRequest);
	server.Post("/currentscreen", RuleServer::HandleCurrentScreenRequest);

	server.Post("/message", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		shared_ptr<ScreenFile::Screen> screen;

		try
		{
			DataManager dm = DataManager();
			ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
			shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);

			auto j = json::parse(req.body);

			string entityId = j.value("entityId", "");
			string nextEntityId = j.value("nextEntityId", "");

			string instanceId = j.value("instanceId", "");
			string nextInstanceId = j.value("nextInstanceId", "");

			string nextScreenId = j.value("nextScreenId", "");

			bool isInitial = j.value("isInitial", false);
			string directTo = j.value("directTo", "");

			if(isInitial)
			{
				if(sessionScreenData.screens.empty())
				{
					res.status = 500;

					res.set_content(make_error("screen_not_loaded", "No screens are available.").dump(2), "application/json");

					return;
				}

				screen = make_shared<ScreenFile::Screen>(sessionScreenData.screens[0]);

				screen->instanceId = dm.GetFirstInstanceId(s.ruleset.entities, screen->entityId);

				instanceId = screen->instanceId.has_value() ? screen->instanceId.value() : std::string();

				auto rulesetPtr = BorrowRuleSet(s.ruleset);

				screen = dm.GetSessionData(s.ruleset.entities, screen, screen->entityId, instanceId, rulesetPtr, &sessionScreenData);

				ScreenLocation loc = ScreenLocation();

				loc.screenId = screen->id;
				loc.entityId = screen->entityId;
				loc.instanceId = instanceId;

				s.ruleset.AddScreenLocation(loc);
			}
			else
			{
				ScreenData::DataRoot rd = ParseScreenDataJson(req.body);

				screen = sessionScreenData.GetScreenByID(rd.screenid);

				if(screen == nullptr)
				{
					res.status = 404;

					res.set_content(make_error("screen_not_found", "Screen not found.").dump(2), "application/json");

					return;
				}

				screen->instanceId = instanceId;
				
				auto rulesetPtr = BorrowRuleSet(s.ruleset);

				screen = dm.HandleData(s.ruleset.entities,
					make_shared<ScreenData::DataRoot>(rd),
					screen,
					sessionScreenDataView,
					directTo,
					rulesetPtr);

				if(screen != nullptr)
				{
					screen = dm.GetSessionData(s.ruleset.entities, screen, screen->entityId, screen->instanceId.has_value() ? screen->instanceId.value() : std::string(), rulesetPtr, &sessionScreenData);

					ScreenLocation loc = ScreenLocation();

					loc.screenId = screen->id;
					loc.entityId = screen->entityId;
					loc.instanceId = screen->instanceId.has_value() ? screen->instanceId.value() : std::string();

					s.ruleset.AddScreenLocation(loc);
				}
			}

			dm.SetDisplayElement(s.ruleset.entities, screen);
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		if(screen != nullptr)
		{
			string j = SerializeScreenJson(screen);

			res.set_content(j, "application/json");
		}
	});

	server.Post("/upload", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		shared_ptr<ScreenFile::Screen> screen;

		try
		{
			string uploadControlId = req.form.get_field("id");
			string instanceId = req.form.get_field("instanceid");

			if(req.is_multipart_form_data() && req.form.has_file("file"))
			{
				RuleSet::UploadInfo uploadInfo;

				uploadInfo.fileId = Util::GenerateGUID();

				uploadInfo.uploadControlId = uploadControlId;

				const auto& file = req.form.get_file("file");

				uploadInfo.fileName = file.filename;
				uploadInfo.fileExtension = file.filename.substr(file.filename.find_last_of('.') + 1);

				uploadInfo.instanceId = instanceId;

				size_t fileSize = file.content.size();

				string fileSizeStr = "";

				if(fileSize < 1024)
				{
					fileSizeStr = std::to_string(fileSize);

					uploadInfo.fileSize = fileSizeStr + "b";
				}
				else if(fileSize < 1024000)
				{
					fileSize = fileSize / 1024;

					fileSizeStr = std::to_string(fileSize);	

					uploadInfo.fileSize = fileSizeStr + "Kb";
				}
				else
				{
					fileSize = fileSize / 1024000;

					fileSizeStr = std::to_string(fileSize);

					uploadInfo.fileSize = fileSizeStr + "Mb";
				}

				s.ruleset.uploads->push_back(uploadInfo);

				json out = {
					{"fileId", uploadInfo.fileId},
					{"uploadControlId", uploadControlId},
					{"fileName", uploadInfo.fileName},
					{"fileExtension", uploadInfo.fileExtension},
					{"fileSize", uploadInfo.fileSize}
				};

				res.set_content(out.dump(4), "application/json");
			}
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		if(screen != nullptr)
		{
			string j = "{}";

			res.set_content(j, "application/json");
		}
	});

	server.Post("/removefile", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		shared_ptr<ScreenFile::Screen> screen;

		try
		{
			auto j = json::parse(req.body);

			string uploadControlId = j.value("uploadid", "");
			string fileId = j.value("fileid", "");
			string instanceId = j.value("instanceid", "");

			if(!uploadControlId.empty() && !fileId.empty())
			{
				s.ruleset.uploads->erase(std::remove_if(s.ruleset.uploads->begin(), s.ruleset.uploads->end(), [&](const RuleSet::UploadInfo& info) {
					return info.uploadControlId == uploadControlId && info.instanceId == instanceId && info.fileId == fileId;
				}), s.ruleset.uploads->end());
			}
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		if(screen != nullptr)
		{
			string j = "{}";

			res.set_content(j, "application/json");
		}
	});

	server.Post("/pdf/resolve", RuleServer::HandlePdfResolveRequest);

	server.Post("/previousscreen", RuleServer::HandlePreviousScreenRequest);

	server.Post("/load", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		try
		{
			for(shared_ptr<Entity> ent : *s.ruleset.entities)
			{
				ent->instances.clear();
			}

			s.ruleset.uploads->clear();

			auto j = json::parse(req.body);

			const string contents = j.value("content", std::string());

			j = json::parse(contents);

			auto arr = j["entities"];

			std::unordered_map<std::string, vector<string>> linkedIdentifiers;

			for(auto& item : arr)
			{
				auto instances = item["instances"];

				string entityId = item.value("entityid", std::string());

				// Loop through s.ruleSet.entities
				for(const auto& entity : *s.ruleset.entities)
				{
					if(entity->entityid == entityId)
					{
						for(auto& instanceItem : instances)
						{
							shared_ptr<Instance> instance;

							instance = entity->GetCleanInstance();

							entity->instances.push_back(instance);

							string instanceId = instanceItem.value("instanceId", std::string());

							auto linkedIds = instanceItem.value("linkedInstances", json::array());

							vector<string> linkedInstanceIds;

							for(const auto& linked : linkedIds)
							{
								string li = linked.get<std::string>();

								linkedInstanceIds.push_back(li);
							}

							if(!linkedInstanceIds.empty())
							{
								linkedIdentifiers[instanceId] = linkedInstanceIds;
							}

							instance->SetInstanceId(instanceId);

							//instance->GetInstanceFromJSON(instanceItem);

							for(const json& attJson : instanceItem.at("attributes"))
							{
								string attId = attJson.value("id", std::string());
								string value = attJson.value("value", std::string());
								bool isInferred = attJson.value("isInferred", false);

								for(const auto& att : instance->attributes)
								{
									if(att->GetID() == attId)
									{
										if(isInferred == false)
										{
											att->SetValueFromString(value, true);
										}
										else
										{
											att->SetRawValue(value);
										}

										break;
									}
								}
							}
						}
						
						break;
					}
				}
			}

			for(const auto& pair : linkedIdentifiers)
			{
				const std::string& instanceId = pair.first;
				
				const std::vector<std::string>& linkedIds = pair.second;

				for(const auto& entity : *s.ruleset.entities)
				{
					for(const shared_ptr<Instance> instance : entity->instances)
					{
						if(instance->instanceId == instanceId)
						{
							for(const auto& entity2 : *s.ruleset.entities)
							{
								shared_ptr<Instance> target = entity2->GetInstanceById(instanceId);
							
								if(target != nullptr)
								{
									for(const string& linkedId : linkedIds)
									{
										for(const auto& entity3 : *s.ruleset.entities)
										{
											shared_ptr<Instance> childInstance = entity3->GetInstanceById(linkedId);

											if(childInstance != nullptr)
											{
												instance->AddLinkedInstance(childInstance);
												childInstance->AddLinkedInstance(instance);

												shared_ptr<Relationship> relationship = FindRuntimeRelationshipBetweenEntities(s.ruleset, instance->entityId, childInstance->entityId);

												if(relationship != nullptr)
												{
													shared_ptr<Instance> sourceInstance = relationship->sourceEntity == instance->entityId ? instance : childInstance;
													shared_ptr<Instance> targetInstance = sourceInstance == instance ? childInstance : instance;

													sourceInstance->AddRelationshipLink(targetInstance, relationship, true);
													targetInstance->AddRelationshipLink(sourceInstance, relationship, false);
												}

												instance->EvaluateFunctions();
												childInstance->EvaluateFunctions();

												break;
											}
										}
									}

									break;
								}
							}

							break;
						}
					}
				}
			}
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		if(s.ruleset.entities->empty() || s.ruleset.entities->begin()->get()->instances.empty())
		{
			res.status = 404;

			res.set_content(make_error("instance_not_found", "No root instance is available after load.").dump(2), "application/json");

			return;
		}

		string iid = s.ruleset.entities->begin()->get()->instances[0]->instanceId;

		json out = {
			{"instanceId", iid}
		};

		res.set_content(out.dump(4), "application/json");
	});

	server.Post("/available-records", RuleServer::HandleAvailableRecordsRequest);
	server.Post("/availablerecords", RuleServer::HandleAvailableRecordsRequest);
	server.Post("/load-record", RuleServer::HandleLoadRecordRequest);
	server.Post("/loadrecord", RuleServer::HandleLoadRecordRequest);
	server.Post("/delete-record", RuleServer::HandleDeleteRecordRequest);
	server.Post("/deleterecord", RuleServer::HandleDeleteRecordRequest);

	server.Post("/save", RuleServer::HandleSaveRequest);

	server.Post("/createinstance", RuleServer::HandleCreateInstanceRequest);
	server.Post("/collect-file/upload", RuleServer::HandleCollectFileUploadRequest);
	server.Post("/collect-file/delete", RuleServer::HandleCollectFileDeleteRequest);

	server.Post("/copy", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		string instanceId = "";

		shared_ptr<ScreenFile::Screen> screen;

		try
		{
			auto j = json::parse(req.body);

			const string sourceEntityId = j.value("sourceEntityId", std::string());
			const string sourceInstanceId = j.value("sourceInstanceId", std::string());
			const string targetEntityId = j.value("targetEntityId", std::string());
			const string targetInstanceId = j.value("targetInstanceId", std::string());

			DataManager dm = DataManager();

			shared_ptr<Instance> sourceInstance = dm.GetEntityInstanceById(s.ruleset.entities, sourceEntityId, sourceInstanceId);
			shared_ptr<Instance> targetInstance = dm.GetEntityInstanceById(s.ruleset.entities, targetEntityId, targetInstanceId);

			dm.Copy(sourceInstance, targetInstance);
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		if(screen != nullptr)
		{
			screen->instanceId = instanceId;

			string j = SerializeScreenJson(screen);

			res.set_content(j, "application/json");
		}
		else
		{
			string j = "{ }";

			res.set_content(j, "application/json");
		}
	});

	server.Post("/logout", RuleServer::HandleLogoutRequest);

	server.Post("/edit", RuleServer::HandleEditInstanceRequest);

	server.Post("/delete", RuleServer::HandleDeleteInstanceRequest);

	server.Post("/summary", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		try
		{
			DataManager dm = DataManager();

			Summary::SummaryData summary = dm.GetSummaryData(s.ruleset.entities);

			json s = summary;

			res.set_content(s.dump(4), "application/json");
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}
	});

	server.Post("/screen", [](const httplib::Request& req, httplib::Response& res) {
		const std::string token = extract_token(req);

		if(token.empty())
		{
			res.status = 401;

			res.set_content(make_error("missing_token", "Send token in Authorization: Bearer <token> or X-Auth-Token.").dump(2), "application/json");

			return;
		}

		Session s;

		if(!try_get_session(token, s))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}

		shared_ptr<ScreenFile::Screen> screen;

		try
		{
			auto j = json::parse(req.body);

			const string screenId = j.value("screenid", std::string());
			const string entityId = j.value("entityid", std::string());
			const string instanceId = j.value("instanceid", std::string());

			ScreenLocation loc = ScreenLocation();

			loc.screenId = screenId;
			loc.entityId = entityId;
			loc.instanceId = instanceId;

			DataManager dm = DataManager();
			ScreenFile::ScreenData& sessionScreenData = EnsureSessionScreenDataLoaded(s);
			shared_ptr<ScreenFile::ScreenData> sessionScreenDataView = RuleServerScreenData(sessionScreenData);

			screen = dm.GetScreen(s.ruleset.entities, sessionScreenDataView, loc, BorrowRuleSet(s.ruleset));

			if(screen == nullptr)
			{
				res.status = 404;

				res.set_content(make_error("screen_not_found", "Screen not found.").dump(2), "application/json");

				return;
			}

			dm.SetDisplayElement(s.ruleset.entities, screen);
		
			s.ruleset.AddScreenLocation(loc);

			string screenJson = SerializeScreenJson(screen);

			res.set_content(screenJson, "application/json");
		}
		catch(const std::exception& e)
		{
			res.status = 400;

			res.set_content(make_error("bad_json", std::string("Invalid JSON: ") + e.what()).dump(2), "application/json");

			return;
		}

		Session updated;

		if(!touch_session(token, &updated))
		{
			res.status = 401;

			res.set_content(make_error("invalid_token", "Token not found or expired.").dump(2), "application/json");

			return;
		}
	});

	server.Post("/page", RuleServer::HandlePageRequest);
	server.Post("/collect-view/membership", RuleServer::HandleCollectViewMembershipRequest);

	// Nice error for unknown routes
	server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
		if(res.status == 404)
		{
			res.set_content(make_error("not_found", "Route not found.").dump(2), "application/json");
		}
	});

	const std::string host = get_env_or_default("CCP_HOST", "0.0.0.0");
	const int port = get_env_int_or_default("CCP_PORT", 8080, 1, 65535);

	printf("Listening on http://%s:%d\n", host.c_str(), port);
	printf("Endpoints:\n");
	printf("- GET  /health\n");
	printf("- GET  /debug/capabilities\n");
	printf("- POST /debug/session-model/validate\n");
	printf("- POST /debug/session/start\n");
	printf("- POST /debug/session/evaluate\n");
	printf("- POST /debug/session/end\n");
	printf("- POST /login\n");
	printf("- POST /external-login\n");
	printf("- GET  /forms\n");
	printf("- POST /reload-session-model\n");
	printf("- GET  /me\n");
	printf("- POST /message\n");
	printf("- POST /logout\n");

	server.listen(host.c_str(), port);

	return 0;
}

string RuleServer::AddInstance(string entityId,
	                           shared_ptr<RuleSet> ruleset,
	                           shared_ptr<Instance> parentInstance,
	                           const string& relationshipId)
{
	if(ruleset == nullptr || ruleset->entities == nullptr)
	{
		return "";
	}

	for(const shared_ptr<Entity>& ent : *ruleset->entities)
	{
		if(ent->entityid == entityId)
		{
			if(parentInstance != nullptr && !parentInstance->instanceId.empty())
			{
				string ret = ent->AddInstance(parentInstance, ruleset->entities, relationshipId);

				parentInstance->EvaluateFunctions();
				
				return ret;
			}
		
			return ent->AddInstance();
		}
	}

	return "";
}

void RuleServer::ConvertValue(const string& strValue, Attribute::AttributeType type, shared_ptr<ValueVariant> outValue)
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

