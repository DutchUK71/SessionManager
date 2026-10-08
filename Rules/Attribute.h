#pragma once

#include <iostream>
#include <functional>
#include <vector>
#include <variant>
#include <string>
#include <memory>
#ifdef WIN32
#   include <objbase.h>
#endif
#include <iomanip>
#include <sstream> 
#include <random>
#include "JSONFile.h"
#include "Util.h"
#include "Date.h"
#include "json.hpp"

using json = nlohmann::json;

#pragma warning(disable:26495)

using namespace std;

// Define a variant type to hold different attribute values
using ValueVariant = variant<int, double, string, bool, Date>;

// Forward declaration of Rule class
class Rule;

class Instance;

class CopyFrom
{
	public:
	string sourceEntityId;
	string sourceAttributeId;
};

// Class representing an attribute with various types and linked conditional rules
class Attribute : public std::enable_shared_from_this<Attribute>
{
public:
	// Enum to represent the type of the attribute
	enum AttributeType
	{
		UNKNOWN,
		INTEGER,
		STRING,
		DOUBLE,
		BOOLEAN,
		DATE,
		DATETIME,
		TIMEOFDAY
	};

	enum class KnowledgeState
	{
		Unknown,
		Uncertain,
		Certain
	};

	static string KnowledgeStateToString(KnowledgeState state);

	CopyFrom GetCopyFrom()
	{
		return copyFrom;
	}

	void SetRawValue(string str)
	{
		rawValue = str;
	}

	string GetRawValue()
	{
		return rawValue;
	}

private:
	CopyFrom copyFrom;

	string rawValue;

	// The type of the attribute
	AttributeType type = UNKNOWN;
	
	// The value of the attribute in different types
	int intValue = 0;

	// The string value of the attribute
	string strValue;
	
	// The double value of the attribute
	double doubleValue = 0.0;
	
	// The boolean value of the attribute
	bool boolValue = false;
	
	// The date value of the attribute
	Date dateValue;

	// The text description of the attribute
	string attributeText;

	string coreName;

	// List of linked conditional rules. These are weak pointer as opposed to shared_ptr to avoid ownership cycles.
	vector<weak_ptr<Rule>> linkedRules;
	
	// Unique identifier for the attribute
	string id;
	
	// Flag indicating if the attribute is inferred by a rule
	bool isInferred = false;

	bool mainIdentifier = false;

	bool isUnknown = true;

	KnowledgeState knowledgeState = KnowledgeState::Unknown;

public:
	// Constructor
	Attribute() = default;
	
	// Constructor to initialize an attribute from a JSON attribute
	Attribute(JSONFile::JSONAttribute);
	
	// Destructor
	~Attribute();
	
	// Static method to convert a string to an AttributeType enum value
	static AttributeType GetAttributeType(string typeStr);

	static bool TryConvertValue(const string& strValue, AttributeType type, ValueVariant& outValue);
	
	// Method to get the current value of the attribute
	ValueVariant GetValue();
	
	// Method to set the value of the attribute
	void SetValue(ValueVariant vv, bool runEval =1);

	bool SetValueFromString(const string& strValue, bool runEval = true);
	
	void SetDefaultValue(std::string defaultValue);

	// Method to get the type of the attribute
	int GetType();
	
	// Method to set the integer value of the attribute
	bool SetIntValue(int newValue);
	
	// Method to set the double value of the attribute
	bool SetDoubleValue(double newValue);
	
	// Method to set the string value of the attribute
	bool SetStringValue(string newValue);
	
	// Method to set the boolean value of the attribute
	bool SetBoolValue(bool newValue);
	
	// Method to set the date value of the attribute
	bool SetDateValue(string newValue);

	// Method to add a linked conditional rule
	void AddRule(shared_ptr<Rule> rule);
	
	// Method to evaluate all linked conditional rules
	void EvaluateAll();
	
	// Method to check if the attribute is inferred by a rule
	bool IsInferred() { return isInferred; }

	bool IsMainIdentifier() { return mainIdentifier; }
	
	// Method to get the text description of the attribute
	string GetAttributeText() { return attributeText; }

	string GetCoreName() { return coreName; }
	
	// Method to get the unique identifier of the attribute
	string GetID() { return id; }

	void SetID(string newId)
	{
		id = newId;
	}

	void MakeUnknown();
	void MakeUnknown(bool runEval);

	bool IsUnknown();
	bool IsKnown();
	bool IsUncertain();
	bool IsCertain();
	KnowledgeState GetKnowledgeState();
	bool SetKnowledgeState(KnowledgeState state, bool runEval = false);

	string screenId;

	string GetStringValue();

	shared_ptr<Instance> instance;

	json CreateJSON();
};
