#pragma once

#define CPPHTTPLIB_USE_MULTIPART_FORMDATA

#include "ScreenFile.h"
#include "json.hpp"
#include "Rules/RuleSet.h"
#include "ScreenData.h"
#include "Functions.h"
#include "SessionModelImporter.h"
#include "SessionModelRuleEvaluator.h"

namespace httplib
{
	struct Request;
	struct Response;
}

class RuleServer
{
	public:
	RuleServer();
	~RuleServer();
	int StartServer();

	static void Load();
	static void HandleLoginRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleExternalLoginRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleReloadSessionModelRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleMeRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleLogoutRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleFormsRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleStartInputsRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleSessionContextRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleSessionHistoryRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleUpdateRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleSubmitScreenRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleStartSessionRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleCurrentScreenRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleStartOrCurrentScreenRequest(const httplib::Request& req, httplib::Response& res, const char* endpointName, bool currentScreenEndpoint);
	static void HandleAvailableRecordsRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleLoadRecordRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleDeleteRecordRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleSaveRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleCreateInstanceRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleCollectFileUploadRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleCollectFileDeleteRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleEditInstanceRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleDeleteInstanceRequest(const httplib::Request& req, httplib::Response& res);
	static void HandlePageRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleCollectViewMembershipRequest(const httplib::Request& req, httplib::Response& res);
	static void HandlePdfResolveRequest(const httplib::Request& req, httplib::Response& res);
	static void HandlePreviousScreenRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleDebugCapabilitiesRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleDebugSessionModelValidateRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleDebugSessionStartRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleDebugSessionEvaluateRequest(const httplib::Request& req, httplib::Response& res);
	static void HandleDebugSessionEndRequest(const httplib::Request& req, httplib::Response& res);
	static bool IsAllowedCorsOrigin(const std::string& origin);
	static void ApplyCorsHeaders(const httplib::Request& req, httplib::Response& res);

	static ScreenFile ruleServerScreenFile;
	static ScreenData::DataRoot ParseScreenDataJson(const std::string jsonString);
	static RuleSet GetCleanRuleSet();
	static bool IsSessionModelLoaded();
	static const std::vector<SessionModelImporter::Diagnostic>& GetSessionModelDiagnostics();
	static json SessionModelEvaluationStatsToJSON(const SessionModelRuleEvaluator::EvaluationStats& stats);
	static string AddInstance(string entityId, shared_ptr<RuleSet> ruleset, shared_ptr<Instance> parentInstance, const string& relationshipId = "");
	static shared_ptr<Instance> GetCleanInstance(string entityId);
	static JSONFile::DataModel model;
	static void ConvertValue(const string& strValue, Attribute::AttributeType type, shared_ptr<ValueVariant> outValue);
	
	static bool loaded;
	static bool loadedFromSessionModel;
	static RuleSet sessionModelRuntimeTemplate;
	static std::vector<SessionModelImporter::Diagnostic> sessionModelDiagnostics;
};
