#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
#include "JSONFile.h"
#include "json.hpp"

using namespace std;

class ScreenFile
{
	public:
	struct Constraint
	{
		optional<int> maxCharacters;
		optional<int> maxRows;
		optional<int> decimals;
		optional<string> pattern;
	};

	enum class QuestionType
	{
		Text,
		Date,
		Double,
		Checkbox,
		Radio
	};

	struct Error
	{
		string errorMessage;
	};

	struct InstanceAttribute
	{
		string attributeId;
		string value;
	};

	struct CollectInstance
	{
		string instanceId;
		vector<InstanceAttribute> attributes;
		optional<bool> selected = false;
	};

	struct Column
	{
		optional<int> order;
		string text;
		string attributeId;
		string source;
		optional<nlohmann::json> tag;
	};

	struct CollectFileAttribute
	{
		string attributeId;
		string attributeName;
		string attributeCoreName;
		string attributeDataType;
	};

	struct CollectFile
	{
		optional<int> maximumSizeMb;
		optional<int> maximumCount;
		bool restrictTypes = false;
		vector<string> allowedExtensions;
		optional<CollectFileAttribute> fileNameAttribute;
		optional<CollectFileAttribute> extensionAttribute;
		optional<CollectFileAttribute> sizeAttribute;
	};

	struct FileTransfer
	{
		string uploadUrl;
		string deleteUrl;
		int maximumSizeMb = 25;
	};

	struct StatusData
	{
		optional<vector<Column>> columns;
		optional<vector<InstanceAttribute>> attributes;
	};

	struct Options
	{
		string caption;
		string value;
		optional<string> valueListItemId;
		optional<string> hint;
		optional<string> conditionEntityId;
		optional<string> conditionAttributeId;
		optional<string> conditionOperator;
		optional<string> conditionComparisonValue;
		optional<bool> conditionValue;
	};

	struct ValueListItem
	{
		string itemId;
		string value;
		string displayText;
	};

	struct ValueList
	{
		string valueListId;
		string valueListName;
		string valueType;
		vector<ValueListItem> items;
	};

	struct Pagination
	{
		optional<int> currentPage = 1;
		optional<int> optionsPerPage = 10;
		optional<int> totalPages = 0;
		optional<string> filter = make_optional<string>("");
		optional<string> previousLabel;
		optional<string> nextLabel;
	};

	struct TableColumn
	{
		optional<int> order;
		string header;
	};

	struct TableData
	{
		optional<vector<TableColumn>> columns;
	};

	struct CollectData
	{
		struct SelectionError
		{
			string attributeId;
			string operatorName;
			string value;
			string message;
		};

		optional<vector<Column>> columns;
		optional<vector<CollectInstance>> instances;
		optional<Pagination> pagination;
		optional<bool> select = false;
		optional<bool> multiSelect = false;
		optional<bool> showNoneOption = true;
		optional<bool> selectionRequired = false;
		optional<string> membershipRelationshipId;
		optional<vector<SelectionError>> selectionErrors = make_optional<vector<SelectionError>>();
		optional<int> selectedCount = 0;
		optional<int> instanceCount = 0;
	};

	struct FileUploadData
	{
		string uploadId;
		string fileId;
		string fileName;
		string fileExtension;
		string fileSize;
	};

	struct FileUpload
	{
		string id;
		optional<int> maxAttachments;
		optional<bool> showFileSize = true;
		optional<bool> showFileExtension = true;
		optional<string> caption;
		optional<vector<FileUploadData>> files;
	};

	struct VisibilityPredicate
	{
		string entityId;
		string attributeId;
		string operatorName;
		string value;
	};

	struct Element
	{
		optional<string> id;
		optional<string> questionText;
		optional<string> hintText;
		optional<QuestionType> type;
		optional<vector<Constraint>> constraints;
		optional<bool> required;
		optional<string> validationMessage;
		optional<vector<CollectData::SelectionError>> ruleErrors = make_optional<vector<CollectData::SelectionError>>();
		optional<vector<Error>> errors = make_optional<vector<ScreenFile::Error>>();
		optional<string> value;
		optional<string> labelText;
		optional<string> className;
		optional<string> displayRuleId;
		optional<bool> display;
		// Runtime-only certainty for logical visibility. Unknown visibility must never drive destructive cleanup.
		optional<bool> displayKnown;
		optional<int> layoutWidthPercent;
		optional<string> containerLayout;
		optional<VisibilityPredicate> visibility;
		optional<string> boundEntityId;
		optional<string> boundAttributeId;
		optional<string> valueListId;
		string controlType;
		optional<string> pdfTemplateId;
		optional<string> pdfDownloadFileName;
		optional<string> collectEntity;
		optional<string> collectRelationshipId;
		optional<string> identifiedBy;
		optional<string> caption;
		optional<string> addButton;
		optional<string> deleteButton;
		optional<bool>canEdit;
		optional<bool> canDelete;
		optional<vector<Options>> options;
		optional<bool> radioOptionVisibility;
		optional<TableData> tableData;
		optional<Pagination> pagination;
		optional<StatusData> statusData;
		optional<CollectData> collectData;
		optional<CollectFile> collectFile;
		optional<string> bannerHeader;
		optional<string> bannerContent;
		optional<string> detailsHeader;
		optional<string> detailsContent;
		optional<string> insetText;
		optional<string> direction;
		optional<FileUpload> fileUpload;
		optional<nlohmann::json> rendering;
		vector<Element> children;
	};

	struct Screen
	{
		using VisibilityPredicate = ScreenFile::VisibilityPredicate;

		string id;
		string entityId;
		optional<string> instanceId;
		optional<bool> relevant;
		optional<bool> always;
		optional<string> back;
		vector<Element> elements;
		optional<string> returnTo;
		optional<bool> clearNextOnReturn;
		optional<string> title;
		optional<int> order;
		optional<bool> isStartScreen;
		optional<bool> isEndScreen;
		optional<VisibilityPredicate> visibility;
	};

	struct FlowTransition
	{
		string transitionId;
		int order = 0;
		string fromScreenId;
		string toScreenId;
		string label;
		string action;
		string emailTemplateId;
		string transitionRole;
		string collectControlId;
		string forgetPolicyMode = "Inherit";
		nlohmann::json condition = nullptr;
	};

	struct FlowScreen
	{
		string screenId;
		int order = 0;
		optional<bool> isStartScreen;
		optional<bool> isEndScreen;
		optional<bool> isCollectDashboard;
		optional<int> maximumOutgoingTransitions;
		vector<FlowTransition> transitions;
	};

	struct Flow
	{
		string startScreenId;
		vector<FlowScreen> screens;
	};

	struct ScreenData
	{
		vector<ValueList> valueLists;
		vector<Screen> screens;
		optional<Flow> flow;
		FileTransfer fileTransfer;

		mutable unordered_map<string, size_t> screenIndexById;
		mutable size_t screenIndexSize = static_cast<size_t>(-1);
		mutable shared_ptr<mutex> screenIndexMutex = make_shared<mutex>();

		void RebuildScreenIndex() const;
		shared_ptr<Screen> GetScreenByID(const string& screenId) const;
	};

	ScreenFile();

	ScreenFile(string filePath);

	~ScreenFile()
	{
	};

	void LoadFile();

	void SaveFile(const ScreenFile::ScreenData& data, const string& filePath);

	string GetScreenJSON(shared_ptr<ScreenFile::Screen> screen);

	string GetElementJSON(shared_ptr<ScreenFile::Element> element);

	shared_ptr<Screen> GetScreenByID(const string& screenID);

	string f;

	ScreenData screenData;
};

void from_json(const nlohmann::json& j, ScreenFile::ScreenData& s);
void to_json(nlohmann::json& j, const ScreenFile::ScreenData& s);

inline void to_json(nlohmann::json& j, const ScreenFile::InstanceAttribute& attr)
{
	j = nlohmann::json{
		{"attributeid", attr.attributeId},
		{"value", attr.value}
	};
}

inline void to_json(nlohmann::json& j, const ScreenFile::CollectInstance& ci)
{
	j = nlohmann::json{
		{"instanceid", ci.instanceId},
		{"attributes", ci.attributes},
		{"selected", ci.selected.value_or(false)}
	};
}

inline void to_json(nlohmann::json& j, const ScreenFile::CollectData::SelectionError& error)
{
	j = nlohmann::json{
		{"attributeId", error.attributeId},
		{"operator", error.operatorName},
		{"value", error.value},
		{"message", error.message}
	};
}

inline void to_json(nlohmann::json& j, const ScreenFile::Column& col)
{
	j = nlohmann::json{
		{"text", col.text},
		{"attributeid", col.attributeId}
	};

	if (col.order.has_value()) {
		j["order"] = col.order.value();
	}
}

inline void to_json(nlohmann::json& j, const ScreenFile::Pagination& pg)
{
	j = nlohmann::json{
		{"currentpage", pg.currentPage.value_or(1)},
		{"optionsperpage", pg.optionsPerPage.value_or(10)},
		{"totalpages", pg.totalPages.value_or(0)},
		{"filter", pg.filter.value_or("")}
	};

	if (pg.previousLabel.has_value()) {
		j["previouslabel"] = pg.previousLabel.value();
	}

	if (pg.nextLabel.has_value()) {
		j["nextlabel"] = pg.nextLabel.value();
	}
}

inline void to_json(nlohmann::json& j, const ScreenFile::TableColumn& column)
{
	j = nlohmann::json{};

	if (column.order.has_value()) {
		j["order"] = column.order.value();
	}

	j["header"] = column.header;
}

inline void to_json(nlohmann::json& j, const ScreenFile::TableData& table)
{
	j = nlohmann::json{};

	if (table.columns.has_value()) {
		j["columns"] = table.columns.value();
	}
}

inline void to_json(nlohmann::json& j, const ScreenFile::CollectData& cd)
{
	j = nlohmann::json{};
	
	if (cd.columns.has_value()) {
		j["columns"] = cd.columns.value();
	}
	
	if (cd.instances.has_value()) {
		j["instances"] = cd.instances.value();
	}
	
	if (cd.pagination.has_value()) {
		j["pagination"] = cd.pagination.value();
	}
	
	if (cd.select.has_value()) {
		j["select"] = cd.select.value();
	}

	if (cd.multiSelect.has_value()) {
		j["multiselect"] = cd.multiSelect.value();
	}

	if (cd.showNoneOption.has_value()) {
		j["shownoneoption"] = cd.showNoneOption.value();
	}

	if (cd.selectionRequired.has_value()) {
		j["selectionrequired"] = cd.selectionRequired.value();
	}

	if (cd.selectionErrors.has_value()) {
		j["selectionerrors"] = cd.selectionErrors.value();
	}

	if (cd.membershipRelationshipId.has_value()) {
		j["membershiprelationshipid"] = cd.membershipRelationshipId.value();
	}

	if (cd.selectedCount.has_value()) {
		j["selectedcount"] = cd.selectedCount.value();
	}
}

