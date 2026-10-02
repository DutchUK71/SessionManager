#include "json.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cctype>
#include "ScreenFile.h"

using json = nlohmann::json;
using namespace std;

ScreenFile::ScreenFile()
{
}

ScreenFile::ScreenFile(string filePath)
{
	f = filePath;

	LoadFile();
}

ScreenFile::QuestionType questionTypeFromString(const std::string& s)
{
	if(s == "text")
	{
		return ScreenFile::QuestionType::Text;
	}

	if(s == "date")
	{
		return ScreenFile::QuestionType::Date;
	}

	if(s == "double")
	{
		return ScreenFile::QuestionType::Double;
	}

	if(s == "checkbox")
	{
		return ScreenFile::QuestionType::Checkbox;
	}

	if(s == "radio")
	{
		return ScreenFile::QuestionType::Radio;
	}

	std::cout << "Unknown question type" << s << std::endl;

	return ScreenFile::QuestionType::Text;
}

string normalize_json_name(const string& value);

string first_string_value(const json& j, const vector<string>& keys)
{
	for(const string& key : keys)
	{
		if(!j.contains(key) || j.at(key).is_null())
		{
			continue;
		}

		if(j.at(key).is_string())
		{
			return j.at(key).get<string>();
		}

		if(j.at(key).is_boolean())
		{
			return j.at(key).get<bool>() ? "true" : "false";
		}

		if(j.at(key).is_number_integer())
		{
			return to_string(j.at(key).get<int>());
		}

		if(j.at(key).is_number_float())
		{
			return to_string(j.at(key).get<double>());
		}
	}

	return "";
}

optional<bool> optional_bool_value(const json& j, const string& key)
{
	if(!j.contains(key) || !j.at(key).is_boolean())
	{
		return nullopt;
	}

	return j.at(key).get<bool>();
}

optional<bool> bool_from_json_value(const json& value)
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
		string normalized = normalize_json_name(value.get<string>());

		if(normalized == "true" || normalized == "yes" || normalized == "y" || normalized == "1")
		{
			return true;
		}

		if(normalized == "false" || normalized == "no" || normalized == "n" || normalized == "0")
		{
			return false;
		}
	}

	return nullopt;
}

optional<bool> first_bool_value(const json& j, const vector<string>& keys)
{
	for(const string& key : keys)
	{
		if(!j.contains(key) || j.at(key).is_null())
		{
			continue;
		}

		optional<bool> value = bool_from_json_value(j.at(key));

		if(value.has_value())
		{
			return value;
		}
	}

	return nullopt;
}

optional<int> optional_int_value(const json& j, const string& key)
{
	if(!j.contains(key) || !j.at(key).is_number_integer())
	{
		return nullopt;
	}

	return j.at(key).get<int>();
}

string normalize_json_name(const string& value)
{
	string normalized;

	for(char c : value)
	{
		if(std::isalnum(static_cast<unsigned char>(c)))
		{
			normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
	}

	return normalized;
}

string normalize_transition_forget_policy_mode(const string& value)
{
	const string normalized = normalize_json_name(value);

	if(normalized.empty() || normalized == "inherit")
	{
		return "Inherit";
	}

	if(normalized == "apply")
	{
		return "Apply";
	}

	if(normalized == "skip")
	{
		return "Skip";
	}

	return value;
}

string trim_json_text(const string& value)
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

vector<string> split_delimited_options(const string& value)
{
	vector<string> result;
	string current;

	for(char c : value)
	{
		if(c == ',' || c == '\n' || c == '\r')
		{
			string text = trim_json_text(current);
			if(!text.empty())
			{
				result.push_back(text);
			}
			current.clear();
			continue;
		}

		current.push_back(c);
	}

	string text = trim_json_text(current);
	if(!text.empty())
	{
		result.push_back(text);
	}

	return result;
}

bool is_required_rule_name(const string& name)
{
	string normalized = normalize_json_name(name);

	return normalized == "required"
		|| normalized == "mandatory"
		|| normalized == "isrequired";
}

optional<int> first_int_value(const json& j, const vector<string>& keys)
{
	for(const string& key : keys)
	{
		if(!j.contains(key) || j.at(key).is_null())
		{
			continue;
		}

		const json& value = j.at(key);

		if(value.is_number_integer())
		{
			return value.get<int>();
		}

		if(value.is_string())
		{
			try
			{
				return stoi(value.get<string>());
			}
			catch(const std::exception&)
			{
			}
		}
	}

	return nullopt;
}

optional<ScreenFile::VisibilityPredicate> parse_visibility_predicate(const json& j)
{
	if(!j.contains("visibility") || !j.at("visibility").is_object())
	{
		return nullopt;
	}

	const json& visibility = j.at("visibility");
	const json* predicate = &visibility;

	if(visibility.contains("ast") && visibility.at("ast").is_object())
	{
		predicate = &visibility.at("ast");
	}

	if(!predicate->contains("nodeType") || predicate->at("nodeType").get<string>() != "condition")
	{
		return nullopt;
	}

	ScreenFile::VisibilityPredicate result;
	result.entityId = first_string_value(*predicate, {"entityId", "entityid"});
	result.attributeId = first_string_value(*predicate, {"attributeId", "attributeid", "id"});
	result.operatorName = first_string_value(*predicate, {"operator", "operatorName"});
	result.value = first_string_value(*predicate, {"value"});

	if(result.attributeId.empty() || result.operatorName.empty())
	{
		return nullopt;
	}

	return result;
}

// CONSTRAINT //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::Constraint& c)
{
	optional<int> maxCharacters = first_int_value(j, {
		"maxcharacters",
		"maxCharacters",
		"maxCharacter",
		"maxCharacterCount",
		"maximumCharacters",
		"maximumCharacterCount",
		"characterLimit",
		"maxChars",
		"maxLength",
		"maxlength",
		"maximumLength"
	});

	if(maxCharacters.has_value())
	{
		c.maxCharacters = maxCharacters.value();
	}

	optional<int> decimals = first_int_value(j, {
		"decimals",
		"decimalPlaces",
		"decimalplaces"
	});

	if(decimals.has_value())
	{
		c.decimals = decimals.value();
	}

	optional<int> maxRows = first_int_value(j, {
		"maxrows",
		"maxRows",
		"maximumRows",
		"maxRowCount"
	});

	if(maxRows.has_value())
	{
		c.maxRows = maxRows.value();
	}

	string pattern = first_string_value(j, {
		"pattern",
		"regex",
		"regularExpression"
	});

	if(!pattern.empty())
	{
		c.pattern = pattern;
	}

	string ruleName = first_string_value(j, {
		"type",
		"name",
		"rule",
		"ruleName",
		"constraint",
		"constraintType",
		"validation",
		"validationName",
		"validationType",
		"validator",
		"validatorType"
	});

	if(!ruleName.empty())
	{
		string normalizedRuleName = normalize_json_name(ruleName);

		if(normalizedRuleName == "maxcharacters"
			|| normalizedRuleName == "maxcharacter"
			|| normalizedRuleName == "maxcharactercount"
			|| normalizedRuleName == "maximumcharacters"
			|| normalizedRuleName == "maximumcharactercount"
			|| normalizedRuleName == "characterlimit"
			|| normalizedRuleName == "maxchars"
			|| normalizedRuleName == "maxlength"
			|| normalizedRuleName == "maximumlength")
		{
			optional<int> namedMaxCharacters = first_int_value(j, {
				"value",
				"validationValue",
				"constraintValue",
				"ruleValue",
				"parameterValue",
				"maxCharacter",
				"maxCharacters",
				"maximumCharacters",
				"maxLength",
				"maximumLength",
				"max",
				"limit",
				"characters",
				"count",
				"length",
				"size"
			});

			if(namedMaxCharacters.has_value())
			{
				c.maxCharacters = namedMaxCharacters.value();
			}
		}
		else if(normalizedRuleName == "pattern"
			|| normalizedRuleName == "regex"
			|| normalizedRuleName == "regularexpression")
		{
			string namedPattern = first_string_value(j, {
				"value",
				"validationValue",
				"constraintValue",
				"ruleValue",
				"parameterValue",
				"pattern",
				"regex",
				"regularExpression"
			});

			if(!namedPattern.empty())
			{
				c.pattern = namedPattern;
			}
		}
	}
}

void to_json(json& j, const ScreenFile::Constraint& c)
{
	if(c.maxCharacters.has_value())
	{
		j["maxcharacters"] = c.maxCharacters.value();
	}

	if(c.decimals.has_value())
	{
		j["decimals"] = c.decimals.value();
	}

	if(c.maxRows.has_value())
	{
		j["maxrows"] = c.maxRows.value();
	}

	if(c.pattern.has_value())
	{
		j["pattern"] = c.pattern.value();
	}
}

bool has_constraint_values(const ScreenFile::Constraint& constraint)
{
	return constraint.maxCharacters.has_value()
		|| constraint.maxRows.has_value()
		|| constraint.decimals.has_value()
		|| constraint.pattern.has_value();
}

optional<vector<ScreenFile::Constraint>> parse_constraints_value(const json& value)
{
	vector<ScreenFile::Constraint> constraints;

	if(value.is_array())
	{
		for(const json& item : value)
		{
			ScreenFile::Constraint constraint = item.get<ScreenFile::Constraint>();

			if(has_constraint_values(constraint))
			{
				constraints.push_back(constraint);
			}
		}
	}
	else if(value.is_object())
	{
		ScreenFile::Constraint constraint = value.get<ScreenFile::Constraint>();

		if(has_constraint_values(constraint))
		{
			constraints.push_back(constraint);
		}
	}

	if(constraints.empty())
	{
		return nullopt;
	}

	return constraints;
}

optional<bool> parse_required_value(const json& value)
{
	if(value.is_array())
	{
		for(const json& item : value)
		{
			optional<bool> required = parse_required_value(item);

			if(required.has_value())
			{
				return required;
			}
		}
	}
	else if(value.is_object())
	{
		optional<bool> direct = first_bool_value(value, {
			"required",
			"isRequired",
			"mandatory",
			"mustAnswer",
			"mustanswer"
		});

		if(direct.has_value())
		{
			return direct;
		}

		string ruleName = first_string_value(value, {
			"type",
			"name",
			"rule",
			"ruleName",
			"constraint",
			"constraintType",
			"validation",
			"validationName",
			"validationType",
			"validator",
			"validatorType"
		});

		if(is_required_rule_name(ruleName))
		{
			optional<bool> enabled = first_bool_value(value, {
				"value",
				"validationValue",
				"constraintValue",
				"ruleValue",
				"parameterValue",
				"enabled",
				"required",
				"isRequired"
			});

			return enabled.value_or(true);
		}

		for(auto it = value.begin(); it != value.end(); ++it)
		{
			if(is_required_rule_name(it.key()))
			{
				return bool_from_json_value(it.value()).value_or(true);
			}
		}
	}
	else
	{
		return bool_from_json_value(value);
	}

	return nullopt;
}

// ERROR //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::Error& e)
{
	e.errorMessage = j.at("errormessage").get<std::string>();
}

void to_json(json& j, const ScreenFile::Error& e)
{
	j["errormessage"] = e.errorMessage;
}

// INSTANCEATTRIBUTE //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::InstanceAttribute& a)
{
	a.attributeId = j.at("attributeid").get<std::string>();
	a.value = j.at("value").get<std::string>();
}
/*
void to_json(json& j, const ScreenFile::InstanceAttribute& a)
{
	j["attributeid"] = a.attributeId;
	j["value"] = a.value;
}
*/

// COLLECTINSTANCE //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::CollectInstance& i)
{
	i.instanceId = j.at("instanceid").get<std::string>();
	i.attributes = j.at("attributes").get<std::vector<ScreenFile::InstanceAttribute>>();
	optional<bool> selected = first_bool_value(j, {"selected"});
	if(selected.has_value())
	{
		i.selected = selected.value();
	}
}
/*
void to_json(json& j, const ScreenFile::CollectInstance& i)
{
	j["instanceid"] = i.instanceId;
	j["attributes"] = i.attributes;
}
*/
// COLLECTHEADING //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::Column& h)
{
	h.order = first_int_value(j, {"order"});
	h.text = first_string_value(j, {"text", "header", "label", "caption", "name"});
	h.attributeId = first_string_value(j, {"attributeid", "attributeId", "attribute", "id", "key"});
	const string source = normalize_json_name(first_string_value(j, {"source", "sourceKind", "sourcekind"}));
	if(source == "filename")
	{
		h.source = "FileName";
		if(h.attributeId.empty()) h.attributeId = "__file_name";
	}
	else if(source == "extension")
	{
		h.source = "Extension";
		if(h.attributeId.empty()) h.attributeId = "__file_extension";
	}
	else if(source == "size")
	{
		h.source = "Size";
		if(h.attributeId.empty()) h.attributeId = "__file_size";
	}
	else if(source == "other")
	{
		h.source = "Other";
	}
	if(j.contains("tag") && j.at("tag").is_object())
	{
		h.tag = j.at("tag");
	}
}

/*
void to_json(json& j, const ScreenFile::Column& h)
{
	j["text"] = h.text;
	j["attributeid"] = h.attributeId;
}
*/


void from_json(const json& j, ScreenFile::Pagination& p)
{
	optional<int> currentPage = first_int_value(j, {"currentpage", "currentPage"});
	if(currentPage.has_value())
	{
		p.currentPage = currentPage.value();
	}

	optional<int> optionsPerPage = first_int_value(j, {"optionsperpage", "optionsPerPage", "pagesize", "pageSize"});
	if(optionsPerPage.has_value())
	{
		p.optionsPerPage = optionsPerPage.value();
	}

	optional<int> totalPages = first_int_value(j, {"totalpages", "totalPages"});
	if(totalPages.has_value())
	{
		p.totalPages = totalPages.value();
	}

	string filter = first_string_value(j, {"filter"});
	if(!filter.empty())
	{
		p.filter = filter;
	}

	string previousLabel = first_string_value(j, {"previouslabel", "previousLabel", "previous", "prev"});
	if(!previousLabel.empty())
	{
		p.previousLabel = previousLabel;
	}

	string nextLabel = first_string_value(j, {"nextlabel", "nextLabel", "next"});
	if(!nextLabel.empty())
	{
		p.nextLabel = nextLabel;
	}
}

/*
void to_json(json& j, const ScreenFile::Pagination& p)
{
	if(p.currentPage.has_value())
	{
		j["currentpage"] = p.currentPage.value();
	}
	if(p.optionsPerPage.has_value())
	{
		j["optionsperpage"] = p.optionsPerPage.value();
	}
	if(p.totalPages.has_value())
	{
		j["totalpages"] = p.totalPages.value();
	}
	if(p.filter.has_value())
	{
		j["filter"] = p.filter.value();
	}
}
*/

// COLLECTDATA //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::CollectData::SelectionError& e)
{
	e.attributeId = first_string_value(j, {"attributeId", "attributeid"});
	e.operatorName = first_string_value(j, {"operator", "operatorName", "operatorname"});
	e.value = first_string_value(j, {"value"});
	e.message = first_string_value(j, {"message", "errorMessage", "errormessage"});
}

void from_json(const json& j, ScreenFile::CollectData& c)
{
	if(j.contains("displayFields") && j.at("displayFields").is_array())
	{
		c.columns = j.at("displayFields").get<std::vector<ScreenFile::Column>>();
	}
	else if(j.contains("displayfields") && j.at("displayfields").is_array())
	{
		c.columns = j.at("displayfields").get<std::vector<ScreenFile::Column>>();
	}
	else if(j.contains("columns"))
	{
		c.columns = j.at("columns").get<std::vector<ScreenFile::Column>>();
	}

	if(j.contains("instances"))
	{
		c.instances = j.at("instances").get<std::vector<ScreenFile::CollectInstance>>();
	}

	if(j.contains("pagination"))
	{
		c.pagination = j.at("pagination").get<ScreenFile::Pagination>();
	}

	if(j.contains("select"))
	{
		c.select = j.at("select").get<bool>();
	}

	optional<bool> multiSelect = first_bool_value(j, {"multiSelect", "multiselect"});
	if(multiSelect.has_value())
	{
		c.multiSelect = multiSelect.value();
	}

	optional<bool> showNoneOption = first_bool_value(j, {"showNoneOption", "shownoneoption"});
	if(showNoneOption.has_value())
	{
		c.showNoneOption = showNoneOption.value();
	}

	optional<bool> selectionRequired = first_bool_value(j, {"selectionRequired", "selectionrequired"});
	if(selectionRequired.has_value())
	{
		c.selectionRequired = selectionRequired.value();
	}

	if(j.contains("selectionErrors") && j.at("selectionErrors").is_array())
	{
		c.selectionErrors = j.at("selectionErrors").get<vector<ScreenFile::CollectData::SelectionError>>();
	}
	else if(j.contains("selectionerrors") && j.at("selectionerrors").is_array())
	{
		c.selectionErrors = j.at("selectionerrors").get<vector<ScreenFile::CollectData::SelectionError>>();
	}

	string membershipRelationshipId = first_string_value(j, {"membershipRelationshipId", "membershiprelationshipid"});
	if(!membershipRelationshipId.empty())
	{
		c.membershipRelationshipId = membershipRelationshipId;
	}

	optional<int> selectedCount = first_int_value(j, {"selectedCount", "selectedcount"});
	if(selectedCount.has_value())
	{
		c.selectedCount = selectedCount.value();
	}
}

/*
void to_json(json& j, const ScreenFile::CollectData& c)
{
	if(c.columns.has_value())
	{
		j["columns"] = c.columns.value();
	}

	if(c.instances.has_value())
	{
		j["instances"] = c.instances.value();
	}
	
	if(c.pagination.has_value())
	{
		j["pagination"] = c.pagination.value();
	}

	if(c.select.has_value())
	{
		j["select"] = c.select.value();
	}
}
*/

void from_json(const json& j, ScreenFile::TableColumn& c)
{
	c.order = first_int_value(j, {"order"});
	c.header = first_string_value(j, {"header", "text", "label", "caption", "name"});
}

void from_json(const json& j, ScreenFile::TableData& t)
{
	if(j.contains("columns") && j.at("columns").is_array())
	{
		t.columns = j.at("columns").get<std::vector<ScreenFile::TableColumn>>();
	}
}

void from_json(const json& j, ScreenFile::Options& o)
{
	o.caption = j.at("caption").get<std::string>();
	o.value = j.at("value").get<std::string>();
	const string valueListItemId = first_string_value(j, {"valueListItemId", "valuelistitemid", "itemId", "itemid"});
	if(!valueListItemId.empty())
	{
		o.valueListItemId = valueListItemId;
	}

	if(j.contains("hint"))
	{
		o.hint = j.at("hint").get<std::string>();
	}

	if(j.contains("conditionEntityId") && j.at("conditionEntityId").is_string())
	{
		o.conditionEntityId = j.at("conditionEntityId").get<std::string>();
	}

	if(j.contains("conditionAttributeId") && j.at("conditionAttributeId").is_string())
	{
		o.conditionAttributeId = j.at("conditionAttributeId").get<std::string>();
	}

	const string conditionOperator = first_string_value(j, {"conditionOperator", "conditionoperator"});
	if(!conditionOperator.empty())
	{
		o.conditionOperator = conditionOperator;
	}

	if(j.contains("conditionComparisonValue") && j.at("conditionComparisonValue").is_string())
	{
		o.conditionComparisonValue = j.at("conditionComparisonValue").get<std::string>();
	}
	else if(j.contains("conditioncomparisonvalue") && j.at("conditioncomparisonvalue").is_string())
	{
		o.conditionComparisonValue = j.at("conditioncomparisonvalue").get<std::string>();
	}

	if(j.contains("conditionValue") && j.at("conditionValue").is_boolean())
	{
		o.conditionValue = j.at("conditionValue").get<bool>();
	}
}

void to_json(json& j, const ScreenFile::Options& o)
{
	j["caption"] = o.caption;
	j["value"] = o.value;
	if(o.valueListItemId.has_value())
	{
		j["valueListItemId"] = o.valueListItemId.value();
	}

	if(o.hint.has_value())
	{
		j["hint"] = o.hint.value();
	}

	if(o.conditionEntityId.has_value())
	{
		j["conditionEntityId"] = o.conditionEntityId.value();
	}

	if(o.conditionAttributeId.has_value())
	{
		j["conditionAttributeId"] = o.conditionAttributeId.value();
	}

	if(o.conditionOperator.has_value())
	{
		j["conditionOperator"] = o.conditionOperator.value();
	}

	if(o.conditionComparisonValue.has_value())
	{
		j["conditionComparisonValue"] = o.conditionComparisonValue.value();
	}

	if(o.conditionValue.has_value())
	{
		j["conditionValue"] = o.conditionValue.value();
	}
}

void from_json(const json& j, ScreenFile::StatusData& s)
{
	if(j.contains("columns"))
	{
		s.columns = j.at("columns").get<std::vector<ScreenFile::Column>>();
	}

	if(j.contains("attributes"))
	{
		s.attributes = j.at("attributes").get<std::vector<ScreenFile::InstanceAttribute>>();
	}
}

void to_json(json& j, const ScreenFile::StatusData& s)
{
	if(s.columns.has_value())
	{
		j["columns"] = s.columns.value();
	}
	if(s.attributes.has_value())
	{
		j["attributes"] = s.attributes.value();
	}
}

void from_json(const json& j, ScreenFile::FileUploadData& f)
{
	f.uploadId = j.at("uploadid").get<std::string>();
	f.fileName = j.at("filename").get<std::string>();
	f.fileExtension = j.at("fileextension").get<std::string>();
	f.fileSize = j.at("filesize").get<std::string>();
	f.fileId = j.at("fileid").get<std::string>();
}

void from_json(const json& j, ScreenFile::ValueListItem& item)
{
	item.itemId = first_string_value(j, {"itemId", "itemid", "id"});
	item.value = first_string_value(j, {"value"});
	item.displayText = first_string_value(j, {"displayText", "displaytext", "caption", "label"});
	if(item.displayText.empty())
	{
		item.displayText = item.value;
	}
}

void to_json(json& j, const ScreenFile::ValueListItem& item)
{
	j = json{
		{"itemId", item.itemId},
		{"value", item.value},
		{"displayText", item.displayText}
	};
}

void from_json(const json& j, ScreenFile::ValueList& valueList)
{
	valueList.valueListId = first_string_value(j, {"valueListId", "valuelistid", "id"});
	valueList.valueListName = first_string_value(j, {"valueListName", "valuelistname", "name"});
	valueList.valueType = first_string_value(j, {"valueType", "valuetype", "type"});
	if(j.contains("items") && j.at("items").is_array())
	{
		valueList.items = j.at("items").get<vector<ScreenFile::ValueListItem>>();
	}
}

void to_json(json& j, const ScreenFile::ValueList& valueList)
{
	j = json{
		{"valueListId", valueList.valueListId},
		{"valueListName", valueList.valueListName},
		{"valueType", valueList.valueType},
		{"items", valueList.items}
	};
}

void to_json(json& j, const ScreenFile::FileUploadData& f)
{
	j["uploadid"] = f.uploadId;
	j["filename"] = f.fileName;
	j["fileextension"] = f.fileExtension;
	j["filesize"] = f.fileSize;
	j["fileid"] = f.fileId;
}

void from_json(const json& j, ScreenFile::FileUpload& f)
{
	f.id = j.at("id").get<std::string>();

	if(j.contains("maxattachments"))
	{
		f.maxAttachments = j.at("maxattachments").get<int>();
	}

	if(j.contains("showfilesize"))
	{
		f.showFileSize = j.at("showfilesize").get<bool>();
	}

	if(j.contains("showfileextension"))
	{
		f.showFileExtension = j.at("showfileextension").get<bool>();
	}

	if(j.contains("caption"))
	{
		f.caption = j.at("caption").get<std::string>();
	}

	if(j.contains("files"))
	{
		f.files = j.at("files").get<std::vector<ScreenFile::FileUploadData>>();
	}
}

void to_json(json& j, const ScreenFile::FileUpload& f)
{
	j["id"] = f.id;

	if(f.maxAttachments.has_value())
	{
		j["maxattachments"] = f.maxAttachments.value();
	}

	if(f.showFileSize.has_value())
	{
		j["showfilesize"] = f.showFileSize.value();
	}

	if(f.showFileExtension.has_value())
	{
		j["showfileextension"] = f.showFileExtension.value();
	}

	if(f.caption.has_value())
	{
		j["caption"] = f.caption.value();
	}

	if(f.files.has_value())
	{
		j["files"] = f.files.value();
	}
}

// QUESTION //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::Element& q)
{
	if(j.contains("id"))
	{
		q.id = j.at("id").get<std::string>();
	}
	else if(j.contains("controlId"))
	{
		q.id = j.at("controlId").get<std::string>();
	}

	if(j.contains("control-type"))
	{
		q.controlType = j.at("control-type").get<std::string>();
	}
	else if(j.contains("controlType"))
	{
		q.controlType = j.at("controlType").get<std::string>();
	}

	const string normalizedControlType = normalize_json_name(q.controlType);
	const bool isLabelControl = normalizedControlType == "label";

	if(j.contains("question"))
	{
		q.questionText = j.at("question").get<std::string>();
	}
	else if(j.contains("questionText"))
	{
		q.questionText = j.at("questionText").get<std::string>();
	}
	else if(!isLabelControl && j.contains("label") && j.at("label").is_string())
	{
		q.questionText = j.at("label").get<std::string>();
	}

	if(j.contains("hinttext"))
	{
		q.hintText = j.at("hinttext").get<std::string>();
	}
	else if(j.contains("hintText"))
	{
		q.hintText = j.at("hintText").get<std::string>();
	}
	else if(j.contains("helpText"))
	{
		q.hintText = j.at("helpText").get<std::string>();
	}
	else if(j.contains("helptext"))
	{
		q.hintText = j.at("helptext").get<std::string>();
	}

	if(j.contains("type"))
	{
		q.type = questionTypeFromString(j.at("type").get<std::string>());
	}

	if(j.contains("constraints"))
	{
		q.constraints = parse_constraints_value(j.at("constraints"));
	}
	else if(j.contains("constraint"))
	{
		q.constraints = parse_constraints_value(j.at("constraint"));
	}
	else if(j.contains("validation"))
	{
		q.constraints = parse_constraints_value(j.at("validation"));
	}
	else if(j.contains("validations"))
	{
		q.constraints = parse_constraints_value(j.at("validations"));
	}
	else if(j.contains("validationElements"))
	{
		q.constraints = parse_constraints_value(j.at("validationElements"));
	}
	else if(j.contains("validationRules"))
	{
		q.constraints = parse_constraints_value(j.at("validationRules"));
	}
	else if(j.contains("validators"))
	{
		q.constraints = parse_constraints_value(j.at("validators"));
	}

	optional<bool> required = first_bool_value(j, {
		"required",
		"isRequired",
		"mandatory"
	});

	if(!required.has_value())
	{
		for(const string& key : {
			"constraints",
			"constraint",
			"validation",
			"validations",
			"validationElements",
			"validationRules",
			"validators"
		})
		{
			if(j.contains(key))
			{
				required = parse_required_value(j.at(key));

				if(required.has_value())
				{
					break;
				}
			}
		}
	}

	q.required = required.value_or(false);

	string validationMessage = first_string_value(j, {"validationMessage", "validationmessage"});
	if(validationMessage.empty() && j.contains("validation") && j.at("validation").is_object())
	{
		validationMessage = first_string_value(j.at("validation"), {"validationMessage", "validationmessage"});
	}
	if(!validationMessage.empty())
	{
		q.validationMessage = validationMessage;
	}

	if(j.contains("ruleErrors") && j.at("ruleErrors").is_array())
	{
		q.ruleErrors = j.at("ruleErrors").get<vector<ScreenFile::CollectData::SelectionError>>();
	}
	else if(j.contains("ruleerrors") && j.at("ruleerrors").is_array())
	{
		q.ruleErrors = j.at("ruleerrors").get<vector<ScreenFile::CollectData::SelectionError>>();
	}
	else if(j.contains("validation") && j.at("validation").is_object())
	{
		const json& validation = j.at("validation");
		if(validation.contains("ruleErrors") && validation.at("ruleErrors").is_array())
		{
			q.ruleErrors = validation.at("ruleErrors").get<vector<ScreenFile::CollectData::SelectionError>>();
		}
		else if(validation.contains("ruleerrors") && validation.at("ruleerrors").is_array())
		{
			q.ruleErrors = validation.at("ruleerrors").get<vector<ScreenFile::CollectData::SelectionError>>();
		}
	}

	if(j.contains("errors"))
	{
		q.errors = j.at("errors").get<std::vector<ScreenFile::Error>>();
	}

	if(j.contains("value"))
	{
		q.value = j.at("value").get<std::string>();
	}

	if(j.contains("label-text"))
	{
		q.labelText = j.at("label-text").get<std::string>();
	}
	else if(j.contains("labelText"))
	{
		q.labelText = j.at("labelText").get<std::string>();
	}
	else if(j.contains("label") && j.at("label").is_string())
	{
		q.labelText = j.at("label").get<std::string>();
	}

	if(j.contains("layout") && j.at("layout").is_object())
	{
		q.layoutWidthPercent = first_int_value(j.at("layout"), {"widthPercent", "widthpercent", "width"});
	}

	if(!q.layoutWidthPercent.has_value())
	{
		q.layoutWidthPercent = first_int_value(j, {"widthPercent", "widthpercent"});
	}

	string containerLayout = first_string_value(j, {"containerLayout", "containerlayout"});

	if(!containerLayout.empty())
	{
		q.containerLayout = containerLayout;
	}

	if(j.contains("class-name"))
	{
		q.className = j.at("class-name").get<std::string>();
	}

	if(j.contains("display-rule-id"))
	{
		q.displayRuleId = j.at("display-rule-id").get<std::string>();
	}
	else if(j.contains("displayRuleId"))
	{
		q.displayRuleId = j.at("displayRuleId").get<std::string>();
	}

	if(j.contains("display"))
	{
		q.display = j.at("display").get<bool>();
	}

	q.visibility = parse_visibility_predicate(j);

	if(j.contains("binding") && j.at("binding").is_object())
	{
		const json& binding = j.at("binding");
		string boundEntityId = first_string_value(binding, {"entityId", "entityid"});
		string boundAttributeId = first_string_value(binding, {"attributeId", "attributeid"});

		if(!boundEntityId.empty())
		{
			q.boundEntityId = boundEntityId;
		}

		if(!boundAttributeId.empty())
		{
			q.boundAttributeId = boundAttributeId;
		}
	}

	const string valueListId = first_string_value(j, {"valueListId", "valuelistid"});
	if(!valueListId.empty())
	{
		q.valueListId = valueListId;
	}

	if(j.contains("pdfForm") && j.at("pdfForm").is_object())
	{
		const json& pdfForm = j.at("pdfForm");
		string templateId = first_string_value(pdfForm, {"templateId", "templateid"});
		string downloadFileName = first_string_value(pdfForm, {"downloadFileName", "downloadfilename"});
		if(!templateId.empty())
		{
			q.pdfTemplateId = templateId;
		}
		if(!downloadFileName.empty())
		{
			q.pdfDownloadFileName = downloadFileName;
		}
	}

	if(j.contains("collect-entity"))
	{
		q.collectEntity = j.at("collect-entity").get<std::string>();
	}
	else
	{
		string collectEntity = first_string_value(j, {"collectEntity"});

		if(!collectEntity.empty() && q.controlType == "entity-instance-list")
		{
			q.collectEntity = collectEntity;
		}
	}

	if(j.contains("collection") && j.at("collection").is_object())
	{
		const json& collection = j.at("collection");
		const string collectionKind = normalize_json_name(first_string_value(collection, {"kind"}));
		string childEntityId = first_string_value(collection, {"childEntityId", "entityId", "collectEntity", "collect-entity"});
		string relationshipId = first_string_value(collection, {"relationshipId", "relationshipid"});
		string membershipRelationshipId = first_string_value(collection, {"membershipRelationshipId", "membershiprelationshipid"});
		string createButtonLabel = first_string_value(collection, {"createButtonLabel", "addButton", "addbutton"});
		string deleteButtonLabel = first_string_value(collection, {"deleteButtonLabel", "deleteButton", "deletebutton"});
		optional<bool> selectRows = first_bool_value(collection, {"selectRows", "selectrows", "select"});
		optional<bool> multiSelect = first_bool_value(collection, {"multiSelect", "multiselect"});
		optional<bool> showNoneOption = first_bool_value(collection, {"showNoneOption", "shownoneoption"});
		optional<bool> selectionRequired = first_bool_value(collection, {"selectionRequired", "selectionrequired"});

		if(!childEntityId.empty())
		{
			q.collectEntity = childEntityId;
		}

		if(!relationshipId.empty())
		{
			q.collectRelationshipId = relationshipId;
		}

		if(selectRows.has_value() || multiSelect.has_value() || showNoneOption.has_value() || selectionRequired.has_value() || !membershipRelationshipId.empty())
		{
			if(!q.collectData.has_value())
			{
				q.collectData = ScreenFile::CollectData();
			}

			q.collectData->select = selectRows.value_or(false);
			q.collectData->multiSelect = q.collectData->select.value_or(false) && multiSelect.value_or(false);
			q.collectData->showNoneOption = showNoneOption.value_or(true);
			q.collectData->selectionRequired = q.collectData->select.value_or(false) && selectionRequired.value_or(false);
			if(collection.contains("selectionErrors") && collection.at("selectionErrors").is_array())
			{
				q.collectData->selectionErrors = collection.at("selectionErrors").get<vector<ScreenFile::CollectData::SelectionError>>();
			}
			if(!membershipRelationshipId.empty())
			{
				q.collectData->membershipRelationshipId = membershipRelationshipId;
			}
		}

		if(!createButtonLabel.empty())
		{
			q.addButton = createButtonLabel;
		}

		if(!deleteButtonLabel.empty())
		{
			q.deleteButton = deleteButtonLabel;
		}

		if((collectionKind == "file" || normalizedControlType == "collectfile") &&
			collection.contains("file") && collection.at("file").is_object())
		{
			const json& file = collection.at("file");
			ScreenFile::CollectFile collectFile;
			collectFile.maximumSizeMb = first_int_value(file, {"maximumSizeMb", "maximumsizemb"});
			collectFile.maximumCount = first_int_value(file, {"maximumCount", "maximumcount", "maxFileCount", "maxfilecount"});
			if(collectFile.maximumCount.has_value() && collectFile.maximumCount.value() <= 0)
			{
				collectFile.maximumCount.reset();
			}
			collectFile.restrictTypes = first_bool_value(file, {"restrictTypes", "restricttypes"}).value_or(false);
			if(file.contains("allowedExtensions") && file.at("allowedExtensions").is_array())
			{
				for(const json& extensionValue : file.at("allowedExtensions"))
				{
					if(extensionValue.is_string())
					{
						string extension = extensionValue.get<string>();
						while(!extension.empty() && extension.front() == '.')
						{
							extension.erase(extension.begin());
						}
						for(char& character : extension)
						{
							character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
						}
						if(!extension.empty())
						{
							collectFile.allowedExtensions.push_back(extension);
						}
					}
				}
			}

			const auto parseAttribute = [](const json& owner, const string& key) -> optional<ScreenFile::CollectFileAttribute> {
				if(!owner.contains(key) || !owner.at(key).is_object())
				{
					return nullopt;
				}
				const json& value = owner.at(key);
				ScreenFile::CollectFileAttribute attribute;
				attribute.attributeId = first_string_value(value, {"attributeId", "attributeid"});
				attribute.attributeName = first_string_value(value, {"attributeName", "attributename"});
				attribute.attributeCoreName = first_string_value(value, {"attributeCoreName", "attributecorename"});
				attribute.attributeDataType = first_string_value(value, {"attributeDataType", "attributedatatype"});
				return attribute.attributeId.empty() ? nullopt : make_optional(attribute);
			};

			collectFile.fileNameAttribute = parseAttribute(file, "fileNameAttribute");
			collectFile.extensionAttribute = parseAttribute(file, "extensionAttribute");
			collectFile.sizeAttribute = parseAttribute(file, "sizeAttribute");
			q.collectFile = collectFile;
			q.canEdit = false;
			q.canDelete = true;
		}

		if(collection.contains("displayFields") && collection.at("displayFields").is_array())
		{
			if(!q.collectData.has_value())
			{
				q.collectData = ScreenFile::CollectData();
			}

			q.collectData->columns = collection.at("displayFields").get<std::vector<ScreenFile::Column>>();
		}
		else if(collection.contains("displayfields") && collection.at("displayfields").is_array())
		{
			if(!q.collectData.has_value())
			{
				q.collectData = ScreenFile::CollectData();
			}

			q.collectData->columns = collection.at("displayfields").get<std::vector<ScreenFile::Column>>();
		}
	}

	if(j.contains("identified-by"))
	{
		q.identifiedBy = j.at("identified-by").get<std::string>();
	}

	if(j.contains("caption"))
	{
		q.caption = j.at("caption").get<std::string>();
	}

	if(j.contains("addbutton"))
	{
		q.addButton = j.at("addbutton").get<std::string>();
	}

	if(j.contains("deletebutton"))
	{
		q.deleteButton = j.at("deletebutton").get<std::string>();
	}

	if(j.contains("edit"))
	{
		q.canEdit = j.at("edit").get<bool>();
	}

	if(j.contains("delete"))
	{
		q.canDelete = j.at("delete").get<bool>();
	}

	if(j.contains("collectdata"))
	{
		q.collectData = j.at("collectdata").get<ScreenFile::CollectData>();
	}

	if(j.contains("displayFields") && j.at("displayFields").is_array())
	{
		if(!q.collectData.has_value())
		{
			q.collectData = ScreenFile::CollectData();
		}

		q.collectData->columns = j.at("displayFields").get<std::vector<ScreenFile::Column>>();
	}
	else if(j.contains("displayfields") && j.at("displayfields").is_array())
	{
		if(!q.collectData.has_value())
		{
			q.collectData = ScreenFile::CollectData();
		}

		q.collectData->columns = j.at("displayfields").get<std::vector<ScreenFile::Column>>();
	}

	if(j.contains("options"))
	{
		if(j.at("options").is_array())
		{
			q.options = j.at("options").get<std::vector<ScreenFile::Options>>();
		}
		else if(j.at("options").is_string())
		{
			vector<ScreenFile::Options> parsedOptions;
			for(const string& optionText : split_delimited_options(j.at("options").get<string>()))
			{
				ScreenFile::Options option;
				option.caption = optionText;
				option.value = optionText;
				parsedOptions.push_back(option);
			}

			q.options = parsedOptions;
		}
	}

	if(j.contains("table") && j.at("table").is_object())
	{
		q.tableData = j.at("table").get<ScreenFile::TableData>();
	}

	if(j.contains("pagination") && j.at("pagination").is_object())
	{
		q.pagination = j.at("pagination").get<ScreenFile::Pagination>();

		if((q.controlType == "entity-instance-list" || q.controlType == "collect" || q.controlType == "collectview" || q.controlType == "collect-view" || q.controlType == "collect-file")
			&& !q.collectData.has_value())
		{
			q.collectData = ScreenFile::CollectData();
		}

		if(q.collectData.has_value() && !q.collectData->pagination.has_value())
		{
			q.collectData->pagination = q.pagination.value();
		}
	}

	if(j.contains("statusdata"))
	{
		q.statusData = j.at("statusdata").get<ScreenFile::StatusData>();
	}

	string bannerHeader = first_string_value(j, {"bannerHeader", "bannerheader"});
	string bannerContent = first_string_value(j, {"bannerContent", "bannercontent"});

	if(normalizedControlType == "notificationbanner" || normalizedControlType == "banner")
	{
		if(bannerHeader.empty())
		{
			bannerHeader = first_string_value(j, {"labelText", "label", "questionText", "question"});
		}

		if(bannerContent.empty())
		{
			bannerContent = first_string_value(j, {"placeholder", "caption"});
		}
	}

	if(!bannerHeader.empty())
	{
		q.bannerHeader = bannerHeader;
	}

	if(!bannerContent.empty())
	{
		q.bannerContent = bannerContent;
	}

	if(j.contains("detailsheader"))
	{
		q.detailsHeader = j.at("detailsheader").get<std::string>();
	}

	if(j.contains("detailscontent"))
	{
		q.detailsContent = j.at("detailscontent").get<std::string>();
	}

	if(j.contains("insettext"))
	{
		q.insetText = j.at("insettext").get<std::string>();
	}

	if(j.contains("direction"))
	{
		q.direction = j.at("direction").get<std::string>();
	}

	if(j.contains("fileupload"))
	{
		q.fileUpload = j.at("fileupload").get<ScreenFile::FileUpload>();
	}

	if(j.contains("rendering") && j.at("rendering").is_object())
	{
		q.rendering = j.at("rendering");

		const json& rendering = q.rendering.value();
		const auto useBooleanOptions = rendering.find("useBooleanOptions");
		if(normalizedControlType == "radio"
			&& useBooleanOptions != rendering.end()
			&& useBooleanOptions->is_boolean()
			&& useBooleanOptions->get<bool>())
		{
			ScreenFile::Options yesOption;
			yesOption.caption = "Yes";
			yesOption.value = "true";

			ScreenFile::Options noOption;
			noOption.caption = "No";
			noOption.value = "false";

			q.options = vector<ScreenFile::Options>{yesOption, noOption};
		}

		const auto useRadioOptionVisibility = rendering.find("useRadioOptionVisibility");
		const auto radioOptions = rendering.find("radioOptions");
		if((normalizedControlType == "radio" || normalizedControlType == "dropdown")
			&& radioOptions != rendering.end()
			&& radioOptions->is_array())
		{
			const bool optionVisibilityEnabled = useRadioOptionVisibility != rendering.end()
				&& useRadioOptionVisibility->is_boolean()
				&& useRadioOptionVisibility->get<bool>();
			vector<ScreenFile::Options> parsedOptions;
			parsedOptions.reserve(radioOptions->size());
			for(const json& optionJson : *radioOptions)
			{
				if(!optionJson.is_object())
				{
					continue;
				}

				ScreenFile::Options option;
				option.caption = first_string_value(optionJson, {"caption", "name", "label"});
				option.value = first_string_value(optionJson, {"value"});
				if(option.caption.empty() || option.value.empty())
				{
					continue;
				}
				const string valueListItemId = first_string_value(optionJson, {"valueListItemId", "valuelistitemid", "itemId", "itemid"});
				if(!valueListItemId.empty())
				{
					option.valueListItemId = valueListItemId;
				}
				const string hint = first_string_value(optionJson, {"hint"});
				if(!hint.empty())
				{
					option.hint = hint;
				}

				const string conditionEntityId = first_string_value(optionJson, {"conditionEntityId", "conditionentityid"});
				const string conditionAttributeId = first_string_value(optionJson, {"conditionAttributeId", "conditionattributeid"});
				const string conditionOperator = first_string_value(optionJson, {"conditionOperator", "conditionoperator"});
				const optional<bool> conditionValue = first_bool_value(optionJson, {"conditionValue", "conditionvalue"});
				if(!conditionOperator.empty())
				{
					option.conditionOperator = conditionOperator;
				}
				if(!conditionEntityId.empty() && !conditionAttributeId.empty())
				{
					option.conditionEntityId = conditionEntityId;
					option.conditionAttributeId = conditionAttributeId;
				}
				if(optionJson.contains("conditionComparisonValue") && optionJson.at("conditionComparisonValue").is_string())
				{
					option.conditionComparisonValue = optionJson.at("conditionComparisonValue").get<std::string>();
				}
				else if(optionJson.contains("conditioncomparisonvalue") && optionJson.at("conditioncomparisonvalue").is_string())
				{
					option.conditionComparisonValue = optionJson.at("conditioncomparisonvalue").get<std::string>();
				}
				if(conditionValue.has_value())
				{
					option.conditionValue = conditionValue.value();
				}

				parsedOptions.push_back(option);
			}

			q.options = parsedOptions;
			q.radioOptionVisibility = optionVisibilityEnabled;
		}
	}

	if(j.contains("children") && j.at("children").is_array())
	{
		q.children = j.at("children").get<std::vector<ScreenFile::Element>>();
	}
	else if(j.contains("controls") && j.at("controls").is_array())
	{
		q.children = j.at("controls").get<std::vector<ScreenFile::Element>>();
	}
	else if(j.contains("elements") && j.at("elements").is_array())
	{
		q.children = j.at("elements").get<std::vector<ScreenFile::Element>>();
	}
}

void to_json(json& j, const ScreenFile::Element& q)
{
	if(q.id.has_value())
	{
		j["id"] = q.id;
	}

	if(q.questionText.has_value())
	{
		j["question"] = q.questionText;
	}
	
	if(q.hintText.has_value())
	{
		j["hinttext"] = q.hintText.value();
	}

	if(q.type.has_value())
	{
		switch(q.type.value())
		{
			case ScreenFile::QuestionType::Text:
				j["type"] = "text";
				break;
			case ScreenFile::QuestionType::Date:
				j["type"] = "date";
				break;
			case ScreenFile::QuestionType::Double:
				j["type"] = "double";
				break;
			case ScreenFile::QuestionType::Checkbox:
				j["type"] = "checkbox";
				break;
			case ScreenFile::QuestionType::Radio:
				j["type"] = "radio";
				break;
		}
	}

	if(q.constraints.has_value())
	{
		j["constraints"] = q.constraints.value();
	}

	if(q.required.has_value())
	{
		j["required"] = q.required.value();
	}

	if(q.validationMessage.has_value())
	{
		j["validationMessage"] = q.validationMessage.value();
	}

	if(q.errors.has_value())
	{
		j["errors"] = q.errors.value();
	}

	if(q.value.has_value())
	{
		j["value"] = q.value.value();
	}

	if(q.labelText.has_value())
	{
		j["label-text"] = q.labelText.value();
		j["labelText"] = q.labelText.value();
	}
	
	j["control-type"] = q.controlType;

	if(q.className.has_value())
	{
		j["class-name"] = q.className;
	}

	if(q.displayRuleId.has_value())
	{
		j["display-rule-id"] = q.displayRuleId.value();
	}

	if(q.display.has_value())
	{
		j["display"] = q.display.value();
	}

	if(q.layoutWidthPercent.has_value())
	{
		j["layout"] = {
			{"widthPercent", q.layoutWidthPercent.value()}
		};
	}

	if(q.containerLayout.has_value())
	{
		j["containerLayout"] = q.containerLayout.value();
	}

	if(q.visibility.has_value())
	{
		j["visibility"] = {
			{"entityId", q.visibility->entityId},
			{"attributeId", q.visibility->attributeId},
			{"operator", q.visibility->operatorName},
			{"value", q.visibility->value}
		};
	}

	if(q.boundEntityId.has_value() || q.boundAttributeId.has_value())
	{
		json binding = json::object();

		if(q.boundEntityId.has_value())
		{
			binding["entityId"] = q.boundEntityId.value();
		}

		if(q.boundAttributeId.has_value())
		{
			binding["attributeId"] = q.boundAttributeId.value();
		}

		j["binding"] = binding;
	}

	if(q.pdfTemplateId.has_value())
	{
		j["pdfForm"] = {
			{"templateId", q.pdfTemplateId.value()},
			{"downloadFileName", q.pdfDownloadFileName.value_or("document.pdf")}
		};
	}

	if(q.collectEntity.has_value())
	{
		j["collect-entity"] = q.collectEntity.value();
	}

	if(q.collectRelationshipId.has_value())
	{
		j["relationshipId"] = q.collectRelationshipId.value();
	}

	if(q.identifiedBy.has_value())
	{
		j["identified-by"] = q.identifiedBy.value();
	}

	if(q.caption.has_value())
	{
		j["caption"] = q.caption.value();
	}

	if(q.addButton.has_value())
	{
		j["addbutton"] = q.addButton.value();
	}

	if(q.deleteButton.has_value())
	{
		j["deletebutton"] = q.deleteButton.value();
	}

	if(q.canEdit.has_value())
	{
		j["edit"] = q.canEdit.value();
	}

	if(q.canDelete.has_value())
	{
		j["delete"] = q.canDelete.value();
	}

	if(q.collectData.has_value())
	{
		j["collectdata"] = q.collectData.value();
	}

	if(q.options.has_value())
	{
		j["options"] = q.options.value();
	}

	if(q.valueListId.has_value())
	{
		j["valueListId"] = q.valueListId.value();
	}

	if(q.tableData.has_value())
	{
		j["table"] = q.tableData.value();
	}

	if(q.pagination.has_value())
	{
		j["pagination"] = q.pagination.value();
	}

	if(q.statusData.has_value())
	{
		j["statusdata"] = q.statusData.value();
	}

	if(q.bannerHeader.has_value())
	{
		j["bannerheader"] = q.bannerHeader.value();
	}

	if(q.bannerContent.has_value())
	{
		j["bannercontent"] = q.bannerContent.value();
	}

	if(q.detailsHeader.has_value())
	{
		j["detailsheader"] = q.detailsHeader.value();
	}

	if(q.detailsContent.has_value())
	{
		j["detailscontent"] = q.detailsContent.value();
	}

	if(q.insetText.has_value())
	{
		j["insettext"] = q.insetText.value();
	}

	if(q.direction.has_value())
	{
		j["direction"] = q.direction.value();
	}

	if(q.fileUpload.has_value())
	{
		j["fileupload"] = q.fileUpload.value();
	}

	if(q.rendering.has_value() && q.rendering->is_object() && !q.rendering->empty())
	{
		j["rendering"] = q.rendering.value();
	}

	if(!q.children.empty())
	{
		j["children"] = q.children;
	}
}

// SCREEN //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::Screen& s)
{
	s.id = first_string_value(j, {"id", "screenId"});

	s.entityId = first_string_value(j, {"entityid", "entityId"});

	if(j.contains("instanceid"))
	{
		s.instanceId = j.at("instanceid").get<std::string>();
	}
	else if(j.contains("instanceId"))
	{
		s.instanceId = j.at("instanceId").get<std::string>();
	}
	
	if(j.contains("elements"))
	{
		s.elements = j.at("elements").get<std::vector<ScreenFile::Element>>();
	}
	else if(j.contains("controls"))
	{
		s.elements = j.at("controls").get<std::vector<ScreenFile::Element>>();
	}

	if(j.contains("relevant"))
	{
		s.relevant = j.at("relevant").get<bool>();
	}

	if(j.contains("always"))
	{
		s.always = j.at("always").get<bool>();
	}

	if(j.contains("back"))
	{
		s.back = j.at("back").get<std::string>();
	}

	if(j.contains("return-to"))
	{
		s.returnTo = j.at("return-to").get<std::string>();
	}
	
	if(j.contains("clearnextonreturn"))
	{
		s.clearNextOnReturn = j.at("clearnextonreturn").get<bool>();
	}

	if(j.contains("title"))
	{
		s.title = j.at("title").get<std::string>();
	}
	else if(j.contains("screenTitle"))
	{
		s.title = j.at("screenTitle").get<std::string>();
	}

	s.order = optional_int_value(j, "order");
	s.isStartScreen = optional_bool_value(j, "isStartScreen");
	s.isEndScreen = optional_bool_value(j, "isEndScreen");
	s.visibility = parse_visibility_predicate(j);
}

void to_json(json& j, const ScreenFile::Screen& s)
{
	j["id"] = s.id;
	j["entityid"] = s.entityId;

	if(s.instanceId.has_value())
	{
		j["instanceid"] = s.instanceId.value();
	}

	j["elements"] = s.elements;
	
	if(s.relevant.has_value())
	{
		j["relevant"] = s.relevant.value();
	}

	if(s.always.has_value())
	{
		j["always"] = s.always.value();
	}

	if(s.back.has_value())
	{
		j["back"] = s.back.value();
	}

	if(s.returnTo.has_value())
	{
		j["return-to"] = s.returnTo.value();
	}

	if(s.clearNextOnReturn.has_value())
	{
		j["clearnextonreturn"] = s.clearNextOnReturn.value();
	}

	if(s.title.has_value())
	{
		j["title"] = s.title.value();
	}

	if(s.order.has_value())
	{
		j["order"] = s.order.value();
	}

	if(s.isStartScreen.has_value())
	{
		j["isStartScreen"] = s.isStartScreen.value();
	}

	if(s.isEndScreen.has_value())
	{
		j["isEndScreen"] = s.isEndScreen.value();
	}

	if(s.visibility.has_value())
	{
		j["visibility"] = {
			{"entityId", s.visibility->entityId},
			{"attributeId", s.visibility->attributeId},
			{"operator", s.visibility->operatorName},
			{"value", s.visibility->value}
		};
	}
}

// SCREENDATA //////////////////////////////////////////////////////////////////////////////
void from_json(const json& j, ScreenFile::FlowTransition& t)
{
	t.transitionId = first_string_value(j, {"transitionId", "id"});
	t.order = optional_int_value(j, "order").value_or(0);
	t.fromScreenId = first_string_value(j, {"fromScreenId", "from"});
	t.toScreenId = first_string_value(j, {"toScreenId", "to"});
	t.label = first_string_value(j, {"label"});
	t.action = first_string_value(j, {"action", "transitionAction"});
	t.emailTemplateId = first_string_value(j, {"emailTemplateId"});
	t.transitionRole = first_string_value(j, {"transitionRole", "role"});
	t.collectControlId = first_string_value(j, {"collectControlId", "collectElementId", "elementId"});
	t.forgetPolicyMode = normalize_transition_forget_policy_mode(first_string_value(j, {"forgetPolicyMode"}));
	t.condition = j.contains("condition") ? j.at("condition") : json(nullptr);
}

void to_json(json& j, const ScreenFile::FlowTransition& t)
{
	j["transitionId"] = t.transitionId;
	j["order"] = t.order;
	j["fromScreenId"] = t.fromScreenId;
	j["toScreenId"] = t.toScreenId;
	j["label"] = t.label;

	if(!t.action.empty())
	{
		j["action"] = t.action;
	}

	if(!t.emailTemplateId.empty())
	{
		j["emailTemplateId"] = t.emailTemplateId;
	}

	if(!t.transitionRole.empty())
	{
		j["transitionRole"] = t.transitionRole;
	}

	if(!t.collectControlId.empty())
	{
		j["collectControlId"] = t.collectControlId;
	}

	j["forgetPolicyMode"] = t.forgetPolicyMode.empty() ? "Inherit" : t.forgetPolicyMode;

	if(!t.condition.is_null())
	{
		j["condition"] = t.condition;
	}
}

void from_json(const json& j, ScreenFile::FlowScreen& s)
{
	s.screenId = first_string_value(j, {"screenId", "id"});
	s.order = optional_int_value(j, "order").value_or(0);
	s.isStartScreen = optional_bool_value(j, "isStartScreen");
	s.isEndScreen = optional_bool_value(j, "isEndScreen");
	s.isCollectDashboard = optional_bool_value(j, "isCollectDashboard");
	s.maximumOutgoingTransitions = optional_int_value(j, "maximumOutgoingTransitions");

	if(j.contains("transitions") && j.at("transitions").is_array())
	{
		s.transitions = j.at("transitions").get<vector<ScreenFile::FlowTransition>>();
	}
}

void to_json(json& j, const ScreenFile::FlowScreen& s)
{
	j["screenId"] = s.screenId;
	j["order"] = s.order;

	if(s.isStartScreen.has_value())
	{
		j["isStartScreen"] = s.isStartScreen.value();
	}

	if(s.isEndScreen.has_value())
	{
		j["isEndScreen"] = s.isEndScreen.value();
	}

	if(s.isCollectDashboard.has_value())
	{
		j["isCollectDashboard"] = s.isCollectDashboard.value();
	}

	if(s.maximumOutgoingTransitions.has_value())
	{
		j["maximumOutgoingTransitions"] = s.maximumOutgoingTransitions.value();
	}

	if(!s.transitions.empty())
	{
		j["transitions"] = s.transitions;
	}
}

void from_json(const json& j, ScreenFile::Flow& f)
{
	f.startScreenId = first_string_value(j, {"startScreenId", "start"});

	if(j.contains("screens") && j.at("screens").is_array())
	{
		f.screens = j.at("screens").get<vector<ScreenFile::FlowScreen>>();
	}
}

void to_json(json& j, const ScreenFile::Flow& f)
{
	j["startScreenId"] = f.startScreenId;
	j["screens"] = f.screens;
}

void from_json(const json& j, ScreenFile::ScreenData& s)
{
	if(j.contains("fileTransfer") && j.at("fileTransfer").is_object())
	{
		const json& transfer = j.at("fileTransfer");
		s.fileTransfer.uploadUrl = first_string_value(transfer, {"uploadUrl", "uploadurl"});
		s.fileTransfer.deleteUrl = first_string_value(transfer, {"deleteUrl", "deleteurl"});
		s.fileTransfer.maximumSizeMb = first_int_value(transfer, {"maximumSizeMb", "maximumsizemb"}).value_or(25);
		if(s.fileTransfer.maximumSizeMb <= 0)
		{
			s.fileTransfer.maximumSizeMb = 25;
		}
	}
	if(j.contains("valueLists") && j.at("valueLists").is_array())
	{
		s.valueLists = j.at("valueLists").get<vector<ScreenFile::ValueList>>();
	}
	else if(j.contains("valuelists") && j.at("valuelists").is_array())
	{
		s.valueLists = j.at("valuelists").get<vector<ScreenFile::ValueList>>();
	}

	if(j.contains("screens") && j.at("screens").is_array())
	{
		s.screens = j.at("screens").get<vector<ScreenFile::Screen>>();
	}

	unordered_map<string, const ScreenFile::ValueList*> valueListsById;
	for(const ScreenFile::ValueList& valueList : s.valueLists)
	{
		if(!valueList.valueListId.empty())
		{
			valueListsById[valueList.valueListId] = &valueList;
		}
	}

	const auto hydrateElementOptions = [&](const auto& self, ScreenFile::Element& element) -> void {
		if(element.collectFile.has_value() && !element.collectFile->maximumSizeMb.has_value())
		{
			element.collectFile->maximumSizeMb = s.fileTransfer.maximumSizeMb;
		}
		if(element.valueListId.has_value())
		{
			const auto valueListIt = valueListsById.find(element.valueListId.value());
			if(valueListIt != valueListsById.end())
			{
				unordered_map<string, ScreenFile::Options> authoredByItemId;
				for(const ScreenFile::Options& authored : element.options.value_or(vector<ScreenFile::Options>()))
				{
					if(authored.valueListItemId.has_value())
					{
						authoredByItemId[authored.valueListItemId.value()] = authored;
					}
				}

				vector<ScreenFile::Options> hydratedOptions;
				hydratedOptions.reserve(valueListIt->second->items.size());
				for(const ScreenFile::ValueListItem& item : valueListIt->second->items)
				{
					if(item.value.empty())
					{
						continue;
					}

					ScreenFile::Options option;
					const auto authoredIt = authoredByItemId.find(item.itemId);
					if(authoredIt != authoredByItemId.end())
					{
						option = authoredIt->second;
					}
					option.valueListItemId = item.itemId.empty() ? nullopt : make_optional(item.itemId);
					option.caption = item.displayText.empty() ? item.value : item.displayText;
					option.value = item.value;
					hydratedOptions.push_back(option);
				}
				element.options = std::move(hydratedOptions);
			}
		}

		for(ScreenFile::Element& child : element.children)
		{
			self(self, child);
		}
	};
	for(ScreenFile::Screen& screen : s.screens)
	{
		for(ScreenFile::Element& element : screen.elements)
		{
			hydrateElementOptions(hydrateElementOptions, element);
		}
	}

	if(j.contains("flow") && j.at("flow").is_object())
	{
		s.flow = j.at("flow").get<ScreenFile::Flow>();
	}
}

void to_json(json& j, const ScreenFile::ScreenData& s)
{
	j["fileTransfer"] = {
		{"uploadUrl", s.fileTransfer.uploadUrl},
		{"deleteUrl", s.fileTransfer.deleteUrl},
		{"maximumSizeMb", s.fileTransfer.maximumSizeMb}
	};
	if(!s.valueLists.empty())
	{
		j["valueLists"] = s.valueLists;
	}
	j["screens"] = s.screens;

	if(s.flow.has_value())
	{
		j["flow"] = s.flow.value();
	}
}

void ScreenFile::ScreenData::RebuildScreenIndex() const
{
	screenIndexById.clear();
	screenIndexById.reserve(screens.size());

	for(size_t index = 0; index < screens.size(); ++index)
	{
		if(!screens[index].id.empty())
		{
			screenIndexById[screens[index].id] = index;
		}
	}

	screenIndexSize = screens.size();
}

shared_ptr<ScreenFile::Screen> ScreenFile::ScreenData::GetScreenByID(const string& screenId) const
{
	if(screenId.empty())
	{
		return nullptr;
	}

	lock_guard<mutex> lock(*screenIndexMutex);

	if(screenIndexSize != screens.size())
	{
		RebuildScreenIndex();
	}

	auto it = screenIndexById.find(screenId);

	if(it == screenIndexById.end())
	{
		RebuildScreenIndex();
		it = screenIndexById.find(screenId);
	}

	if(it != screenIndexById.end() && it->second < screens.size() && screens[it->second].id == screenId)
	{
		return make_shared<ScreenFile::Screen>(screens[it->second]);
	}

	RebuildScreenIndex();
	it = screenIndexById.find(screenId);

	if(it != screenIndexById.end() && it->second < screens.size() && screens[it->second].id == screenId)
	{
		return make_shared<ScreenFile::Screen>(screens[it->second]);
	}

	return nullptr;
}

string ScreenFile::GetScreenJSON(shared_ptr<ScreenFile::Screen> screen)
{
	json j = *screen;

	return j.dump(4);
}

string ScreenFile::GetElementJSON(shared_ptr<ScreenFile::Element> element)
{
	json j = *element;

	return j.dump(4);
}

void ScreenFile::LoadFile()
{
	std::ifstream file(f);

	if(!file.is_open())
	{
		std::cout << "Failed to open file: " << f << std::endl;

		screenData = ScreenData();

		return;
	}

	json j;

	file >> j;

	screenData = j.get<ScreenData>();
}

void ScreenFile::SaveFile(const ScreenFile::ScreenData& data, const string& filePath)
{
	json j = data;
	
	std::ofstream file(filePath);
	
	if(!file.is_open())
	{
		std::cout << "Failed to open file for writing: " << filePath << std::endl;
	
		return;
	}
	
	file << j.dump(4);
}

shared_ptr<ScreenFile::Screen> ScreenFile::GetScreenByID(const string& screenID)
{
	return screenData.GetScreenByID(screenID);
}

