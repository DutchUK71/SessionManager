#include "Attribute.h"
#include "Rule.h"
#include "Instance.h"
#include <algorithm>
#include <cctype>

namespace
{
    bool HasOnlyTrailingWhitespace(const string& value, size_t pos)
    {
        while(pos < value.size())
        {
            if(!isspace(static_cast<unsigned char>(value[pos])))
            {
                return false;
            }

            ++pos;
        }

        return true;
    }

    bool IsAllDigits(const string& value)
    {
        return all_of(value.begin(), value.end(), [](unsigned char c)
        {
            return isdigit(c);
        });
    }

    bool TryParseFixedInt(const string& value, size_t pos, size_t length, int& outValue)
    {
        if(pos + length > value.size())
        {
            return false;
        }

        const string segment = value.substr(pos, length);

        if(!IsAllDigits(segment))
        {
            return false;
        }

        outValue = stoi(segment);

        return true;
    }

    string FormatIsoDate(int year, int month, int day)
    {
        ostringstream out;

        out << setfill('0') << setw(4) << year << "-"
            << setw(2) << month << "-"
            << setw(2) << day;

        return out.str();
    }

    string FormatCompactLegacyDate(int year, int month, int day)
    {
        ostringstream out;

        out << setfill('0') << setw(2) << day
            << setw(2) << month
            << setw(4) << year;

        return out.str();
    }

    bool TryNormalizeDateValue(const string& value, string& isoDate, string& compactLegacyDate)
    {
        string dateText = value;

        if(dateText.size() > 10
            && (dateText[10] == 'T' || dateText[10] == ' '))
        {
            dateText = dateText.substr(0, 10);
        }

        int year = 0;
        int month = 0;
        int day = 0;

        if(dateText.size() == 10 && dateText[4] == '-' && dateText[7] == '-')
        {
            if(!TryParseFixedInt(dateText, 0, 4, year)
                || !TryParseFixedInt(dateText, 5, 2, month)
                || !TryParseFixedInt(dateText, 8, 2, day))
            {
                return false;
            }
        }
        else if(dateText.size() == 10
            && (dateText[2] == '-' || dateText[2] == '/')
            && (dateText[5] == '-' || dateText[5] == '/'))
        {
            if(!TryParseFixedInt(dateText, 0, 2, day)
                || !TryParseFixedInt(dateText, 3, 2, month)
                || !TryParseFixedInt(dateText, 6, 4, year))
            {
                return false;
            }
        }
        else if(dateText.size() == 8 && IsAllDigits(dateText))
        {
            if(!TryParseFixedInt(dateText, 0, 2, day)
                || !TryParseFixedInt(dateText, 2, 2, month)
                || !TryParseFixedInt(dateText, 4, 4, year))
            {
                return false;
            }
        }
        else
        {
            return false;
        }

        compactLegacyDate = FormatCompactLegacyDate(year, month, day);

        if(!Date::IsValidDate(compactLegacyDate))
        {
            return false;
        }

        isoDate = FormatIsoDate(year, month, day);

        return true;
    }

    bool IsValidTimeValue(int hour, int minute, int second)
    {
        return hour >= 0 && hour <= 23
            && minute >= 0 && minute <= 59
            && second >= 0 && second <= 59;
    }

    string FormatTimeValue(int hour, int minute, int second)
    {
        ostringstream out;

        out << setfill('0') << setw(2) << hour << ":"
            << setw(2) << minute << ":"
            << setw(2) << second;

        return out.str();
    }

    bool TryNormalizeTimeValue(const string& value, string& normalizedTime)
    {
        if(value.size() < 5 || value[2] != ':')
        {
            return false;
        }

        int hour = 0;
        int minute = 0;
        int second = 0;

        if(!TryParseFixedInt(value, 0, 2, hour)
            || !TryParseFixedInt(value, 3, 2, minute))
        {
            return false;
        }

        if(value.size() > 5
            && (value.size() < 8
                || value[5] != ':'
                || !TryParseFixedInt(value, 6, 2, second)))
        {
            return false;
        }

        if(!IsValidTimeValue(hour, minute, second))
        {
            return false;
        }

        normalizedTime = FormatTimeValue(hour, minute, second);

        return true;
    }

    bool TryNormalizeDateTimeValue(const string& value, string& normalizedDateTime)
    {
        if(value.size() < 16 || (value[10] != 'T' && value[10] != ' '))
        {
            return false;
        }

        string isoDate;
        string compactLegacyDate;
        string normalizedTime;

        if(!TryNormalizeDateValue(value.substr(0, 10), isoDate, compactLegacyDate)
            || !TryNormalizeTimeValue(value.substr(11), normalizedTime))
        {
            return false;
        }

        normalizedDateTime = isoDate + "T" + normalizedTime;

        return true;
    }

    string FormatIsoDate(const Date& date)
    {
        return FormatIsoDate(date.getYear(), date.getMonth(), date.getDay());
    }

    bool IsStringBackedAttributeType(Attribute::AttributeType type)
    {
        return type == Attribute::STRING
            || type == Attribute::DATETIME
            || type == Attribute::TIMEOFDAY;
    }
}

// Constructor
Attribute::Attribute(JSONFile::JSONAttribute att)
{
    // Initialize ID
    this->id = att.attributeid;
    
	// Initialize attribute text
	this->attributeText = att.text;

	this->coreName = att.coreName;

	this->mainIdentifier = att.mainIdentifier;

	// Initialize type
    this->type = GetAttributeType(att.type);

    if(att.copyfrom.has_value())
    {
        this->copyFrom = CopyFrom(att.copyfrom->sourceEntityId, att.copyfrom->sourceAttributeId);
	}

	if(att.inferredbyrule != "")
    {
        this->isInferred = true;
    }

	// Initialize default value based on type
	this->isInferred = !att.inferredbyrule.empty();

    if(att.defaultvalue != "")
    {
        SetDefaultValue(att.defaultvalue);
    }
}

// Destructor
Attribute::~Attribute()
{
}

string Attribute::KnowledgeStateToString(KnowledgeState state)
{
    switch(state)
    {
        case KnowledgeState::Certain:
            return "Certain";
        case KnowledgeState::Uncertain:
            return "Uncertain";
        case KnowledgeState::Unknown:
        default:
            return "Unknown";
    }
}

void Attribute::SetDefaultValue(std::string defaultValue)
{
    if(type == INTEGER)
    {
        SetIntValue(stoi(defaultValue));
    }
    else if(type == DOUBLE)
    {
        SetDoubleValue(stod(defaultValue));
    }
    else if(type == STRING)
    {
        SetStringValue(defaultValue);
    }
    else if(type == BOOLEAN)
    {
        SetBoolValue(defaultValue == "true");
    }
	else if (type == DATE)
    {
        SetDateValue(defaultValue);
	}
    else if(type == DATETIME || type == TIMEOFDAY)
    {
        SetValueFromString(defaultValue, false);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: GetType
//
// Description: Returns the type of the attribute.
//
// Returns:
//     int: The type of the attribute as an integer.
//
int Attribute::GetType()
{
	// Return the attribute type
    return type;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: GetAttributeType
//
// Description: Maps a string representation of an attribute type to its corresponding enum value.
//
// Parameters:
// 
// Returns:
//     Attribute::AttributeType: The corresponding enum value for the attribute type.
//
Attribute::AttributeType Attribute::GetAttributeType(string typeStr)
{
    if(typeStr == "integer")
    {
        return INTEGER;
    }
    else if(typeStr == "double")
    {
        return DOUBLE;
    }
    else if(typeStr == "text")
    {
        return STRING;
    }
    else if(typeStr == "boolean")
    {
        return BOOLEAN;
	}
	else if (typeStr == "date")
    {
        return DATE;
    }
    else if(typeStr == "datetime")
    {
        return DATETIME;
    }
    else if(typeStr == "timeofday" || typeStr == "time")
    {
        return TIMEOFDAY;
    }

	return UNKNOWN;
}

bool Attribute::TryConvertValue(const string& strValue, AttributeType type, ValueVariant& outValue)
{
    if(strValue.empty() || type == UNKNOWN)
    {
        return false;
    }

    try
    {
        size_t parsedChars = 0;

        if(type == INTEGER)
        {
            int intValue = stoi(strValue, &parsedChars);

            if(!HasOnlyTrailingWhitespace(strValue, parsedChars))
            {
                return false;
            }

            outValue = ValueVariant(intValue);

            return true;
        }
        else if(type == DOUBLE)
        {
            double doubleValue = stod(strValue, &parsedChars);

            if(!HasOnlyTrailingWhitespace(strValue, parsedChars))
            {
                return false;
            }

            outValue = ValueVariant(doubleValue);

            return true;
        }
        else if(type == STRING)
        {
            outValue = ValueVariant(strValue);

            return true;
        }
        else if(type == BOOLEAN)
        {
            string normalized = strValue;

            normalized = Util::ToUpper(normalized);

            if(normalized == "TRUE" || normalized == "1" || normalized == "YES" || normalized == "Y")
            {
                outValue = ValueVariant(true);

                return true;
            }

            if(normalized == "FALSE" || normalized == "0" || normalized == "NO" || normalized == "N")
            {
                outValue = ValueVariant(false);

                return true;
            }

            return false;
        }
        else if(type == DATE)
        {
            string isoDate;
            string compactLegacyDate;

            if(!TryNormalizeDateValue(strValue, isoDate, compactLegacyDate))
            {
                return false;
            }

            outValue = ValueVariant(isoDate);

            return true;
        }
        else if(type == DATETIME)
        {
            string dateTimeValue;

            if(!TryNormalizeDateTimeValue(strValue, dateTimeValue))
            {
                return false;
            }

            outValue = ValueVariant(dateTimeValue);

            return true;
        }
        else if(type == TIMEOFDAY)
        {
            string timeValue;

            if(!TryNormalizeTimeValue(strValue, timeValue))
            {
                return false;
            }

            outValue = ValueVariant(timeValue);

            return true;
        }
    }
    catch(const std::exception&)
    {
        return false;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: GetValue
//
// Description: Retrieves the current value of the attribute as a ValueVariant.
//
// Returns:
//     ValueVariant: The current value of the attribute based on the type.
//
ValueVariant Attribute::GetValue()
{
    if(type == INTEGER)
    {
		ValueVariant vv = ValueVariant(intValue);

        return vv;
    }
    else if(type == DOUBLE)
    {
        ValueVariant vv = ValueVariant(doubleValue);

        return vv;
    }
    else if(type == STRING)
    {
        ValueVariant vv = ValueVariant(strValue);

        return vv;
    }
    else if(type == BOOLEAN)
    {
        ValueVariant vv = ValueVariant(boolValue);

        return vv;
    }
	else if (type == DATE)
    {
        ValueVariant vv = ValueVariant(dateValue);
    
        return vv;
	}
    else if(type == DATETIME || type == TIMEOFDAY)
    {
        ValueVariant vv = ValueVariant(strValue);

        return vv;
    }

    return ValueVariant();
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: AddRule
//
// Description: Adds a conditional rule to the list of linked rules for this attribute.
//
// Parameters:
//    shared_ptr<Rule> rule: The conditional rule to be added.
//
// Returns: None.
//
void Attribute::AddRule(shared_ptr<Rule> rule)
{
    for (const weak_ptr<Rule>& linkedRuleWeak : linkedRules)
    {
        // Lock the weak pointer to get a shared pointer
        if (auto linkedRule = linkedRuleWeak.lock())
        {
            // Check if the rule ID matches
            if (linkedRule == rule)
            {
                return;
            }
        }
    }
    
	// The rule is not yet likned, so add the rule to the list of linked rules
    linkedRules.push_back(rule);
}


///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: EvaluateAll
//
// Description: Evaluates all linked conditional rules for this attribute.
//
// Returns: None.
//
void Attribute::EvaluateAll()
{
	// Evaluate each linked rule
    for (const weak_ptr<Rule>& linkedRuleWeak : linkedRules)
    {
		// Lock the weak pointer to get a shared pointer
        if (auto linkedRule = linkedRuleWeak.lock())
        {
			// Evaluate the linked rule
            linkedRule->Evaluate();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: SetValue
//
// Description: Sets the value of the attribute based on the provided ValueVariant.
//
// Parameters:
//    ValueVariant vv: The new value to be set.
//    bool runEval: Flag indicating whether to evaluate linked rules after setting the value.
//
// Returns: None.
//
void Attribute::SetValue(ValueVariant vv, bool runEval)
{
    bool changed = false;
    bool typeMatched = false;

	// Set the value based on the attribute type
    if(type == INTEGER && holds_alternative<int>(vv))
    {
        typeMatched = true;
        changed = SetIntValue(get<int>(vv));
    }
    else if(type == DOUBLE && holds_alternative<double>(vv))
    {
        typeMatched = true;
        changed = SetDoubleValue(get<double>(vv));
    }
    else if(IsStringBackedAttributeType(type) && holds_alternative<string>(vv))
    {
        typeMatched = true;
        changed = SetStringValue(get<string>(vv));
    }
    else if(type == BOOLEAN && holds_alternative<bool>(vv))
    {
        typeMatched = true;
        changed = SetBoolValue(get<bool>(vv));
    }
    else if (type == DATE && holds_alternative<string>(vv))
    {
        typeMatched = true;
		changed = SetDateValue(get<string>(vv));
    }
    else
    {
		cout << "Type mismatch for \"" << attributeText << "\". Cannot set value." << endl;
    }

    if(typeMatched && !changed && !isUnknown && knowledgeState != KnowledgeState::Certain)
    {
        changed = SetKnowledgeState(KnowledgeState::Certain, false);
    }

	// Evaluate linked rules if the value changed and runEval is true
    if (runEval && changed)
    {
        if(instance != nullptr)
        {
            instance->EvaluateFunctions();
        }

        EvaluateAll();
    }
}

bool Attribute::SetValueFromString(const string& strValue, bool runEval)
{
    SetRawValue(strValue);

    if(strValue.empty())
    {
        MakeUnknown(runEval);

        return true;
    }

    ValueVariant convertedValue;

    if(!TryConvertValue(strValue, static_cast<AttributeType>(GetType()), convertedValue))
    {
        SetRawValue("");
        MakeUnknown(runEval);

        return false;
    }

    SetValue(convertedValue, runEval);

    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: SetIntValue
//
// Description: Sets the integer value of the attribute.
//
// Parameters:
//    int newValue: The new integer value to be set.
//
// Returns:
//    bool: True if the value was changed, false otherwise.
//
bool Attribute::SetIntValue(int newValue)
{
	// Check for type mismatch
    if (type != INTEGER)
    {
        cout << "Type mismatch. Cannot set integer value to non-integer attribute." << endl;
    }

	// Update the value if it has changed
    if (isUnknown || intValue != newValue)
    {
        if(isUnknown)
        {
            cout << this->attributeText + ": Unknown to " << newValue << endl;
        }
        else
        {
            cout << this->attributeText + ": " << intValue << " to " << newValue << endl;
        }

        intValue = newValue;
    
        isUnknown = false;
        knowledgeState = KnowledgeState::Certain;

		return true;
    }

	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: SetDoubleValue
//
// Description: Sets the double value of the attribute.
//
// Parameters:
//    double newValue: The new double value to be set.
//
// Returns:
//    bool: True if the value was changed, false otherwise.
//
bool Attribute::SetDoubleValue(double newValue)
{
	// Check for type mismatch
    if (type != DOUBLE)
    {
        cout << "Type mismatch. Cannot set double value to non-double attribute." << endl;
    }

	// Update the value if it has changed
    if (isUnknown || doubleValue != newValue)
    {
        if(isUnknown)
        {
            cout << this->attributeText + ": Unknown to " << newValue << endl;
        }
        else
        {
            cout << this->attributeText + ": " << doubleValue << " to " << newValue << endl;
        }

        doubleValue = newValue;
        
        isUnknown = false;
        knowledgeState = KnowledgeState::Certain;

        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: SetStringValue
//
// Description: Sets the string value of the attribute.
//
// Parameters:
//    string newValue: The new string value to be set.
//
// Returns:
//    bool: True if the value was changed, false otherwise.
//
bool Attribute::SetStringValue(string newValue)
{
	// Check for type mismatch
    if (!IsStringBackedAttributeType(type))
    {
        cout << "Type mismatch. Cannot set text-backed value to non-text-backed attribute." << endl;
    }

	// Update the value if it has changed
    if (isUnknown || strValue != newValue)
    {
		if(isUnknown)
        {
            cout << this->attributeText + ": Unknown to " << newValue << endl;
        }
        else
        {
            cout << this->attributeText + ": " << strValue << " to " << newValue << endl;
        }
       
        strValue = newValue;
        
        isUnknown = false;
        knowledgeState = KnowledgeState::Certain;

        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: SetBoolValue
//
// Description: Sets the boolean value of the attribute.
//
// Parameters:
//    bool newValue: The new boolean value to be set.
//
// Returns:
//    bool: True if the value was changed, false otherwise.
//
bool Attribute::SetBoolValue(bool newValue)
{
	// Check for type mismatch
    if (type != BOOLEAN)
    {
        cout << "Type mismatch. Cannot set boolean value to non-boolean attribute." << endl;
    }
    
	// Update the value if it has changed
    if (isUnknown || boolValue != newValue)
    {
        if(isUnknown)
        {
            cout << this->attributeText + ": Unknown to " << (newValue ? "true" : "false") << endl;
        }
        else
        {
            cout << this->attributeText + ": " << (boolValue ? "true" : "false") << " to " << (newValue ? "true" : "false") << endl;
        }

        boolValue = newValue;
    
        isUnknown = false;
        knowledgeState = KnowledgeState::Certain;

		return true;
    }

	return false;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: SetDateValue
//
// Description: Sets the date value of the attribute.
//
// Parameters:
//    bool newValue: The new date value to be set.
//
// Returns:
//    bool: True if the value was changed, false otherwise.
//
bool Attribute::SetDateValue(string newValue)
{
    string isoDate;
    string compactLegacyDate;

    // Check for type mismatch
    if (type != DATE)
    {
        cout << "Type mismatch. Cannot set date value to non-date attribute." << endl;
        
        return false;
    }

	if (!TryNormalizeDateValue(newValue, isoDate, compactLegacyDate))
    {
        cout << "Invalid date format for \"" << attributeText << "\": " << newValue << endl;
        
        return false;
    }

	Date d = Date(compactLegacyDate);
        
    // Update the value if it has changed
    if (isUnknown || dateValue != d)
    {
		if(isUnknown)
        {
            cout << this->attributeText + ": Unknown to " << isoDate << endl;
        }
        else
        {
            cout << this->attributeText + ": " << FormatIsoDate(dateValue) << " to " << isoDate << endl;
        }
        
        dateValue = d;

        isUnknown = false;
        knowledgeState = KnowledgeState::Certain;

        return true;
    }

    return false;
}

void Attribute::MakeUnknown()
{
    MakeUnknown(true);
}

void Attribute::MakeUnknown(bool runEval)
{
    if(!isUnknown)
    {
        isUnknown = true;
        knowledgeState = KnowledgeState::Unknown;

        switch(type)
        {
            case INTEGER:
                cout << this->attributeText + ": " << intValue << " to Unknown" << endl;
                intValue = 0;
                break;
            case DOUBLE:
				cout << this->attributeText + ": " << doubleValue << " to Unknown" << endl;
                doubleValue = 0.0;
                break;
            case STRING:
                cout << this->attributeText + ": " << strValue << " to Unknown" << endl;
                strValue = "";
                break;
            case DATETIME:
            case TIMEOFDAY:
                cout << this->attributeText + ": " << strValue << " to Unknown" << endl;
                strValue = "";
                break;
            case BOOLEAN:
				cout << this->attributeText + ": " << (boolValue ? "true" : "false") << " to Unknown" << endl;
                boolValue = false;
                break;
            case DATE:
                cout << this->attributeText + ": " << FormatIsoDate(dateValue) << " to Unknown" << endl;
                dateValue = Date();
                break;
            default:
                break;
        }

        if(runEval && instance != nullptr)
        {
            instance->EvaluateFunctions();
        }

        if(runEval)
        {
            EvaluateAll();
        }
    }
    else
    {
        knowledgeState = KnowledgeState::Unknown;
    }
}

bool Attribute::IsUnknown()
{
    return knowledgeState == KnowledgeState::Unknown;
}

bool Attribute::IsKnown()
{
    return !IsUnknown();
}

bool Attribute::IsUncertain()
{
    return knowledgeState == KnowledgeState::Uncertain;
}

bool Attribute::IsCertain()
{
    return knowledgeState == KnowledgeState::Certain;
}

Attribute::KnowledgeState Attribute::GetKnowledgeState()
{
    return knowledgeState;
}

bool Attribute::SetKnowledgeState(KnowledgeState state, bool runEval)
{
    if(state == KnowledgeState::Unknown)
    {
        const bool wasUnknown = IsUnknown();

        MakeUnknown(runEval);

        return !wasUnknown;
    }

    const bool changed = isUnknown || knowledgeState != state;

    isUnknown = false;
    knowledgeState = state;

    if(runEval && changed)
    {
        if(instance != nullptr)
        {
            instance->EvaluateFunctions();
        }

        EvaluateAll();
    }

    return changed;
}

string Attribute::GetStringValue()
{
    if(type == STRING || type == DATETIME || type == TIMEOFDAY)
    {
        return strValue;
    }
    else if(type == INTEGER)
    {
        return to_string(intValue);
    }
    else if(type == DOUBLE)
    {
        return to_string(doubleValue);
    }
    else if(type == BOOLEAN)
    {
        return boolValue ? "true" : "false";
    }
    else if(type == DATE)
    {
        return FormatIsoDate(dateValue);
    }

    return "";
}

json Attribute::CreateJSON()
{
    json j;

    j["id"] = id;
    j["type"] = type;
    j["value"] = GetStringValue();
	j["attributeText"] = attributeText;
	j["isInferred"] = isInferred;
	if(mainIdentifier)
	{
		j["mainIdentifier"] = true;
	}
    j["knowledgeState"] = KnowledgeStateToString(knowledgeState);
    j["isKnown"] = IsKnown();
    j["isCertain"] = IsCertain();
    j["isUncertain"] = IsUncertain();

	if(!coreName.empty())
	{
		j["coreName"] = coreName;
	}
    
    return j;
}
