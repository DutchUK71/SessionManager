#pragma once

#include "Attribute.h"
#include "Condition.h"
#include <memory>
#include <vector>
#include <unordered_set>
#include "JSONFile.h"
#include "FunctionRule.h"

// Enum to represent the type of rule
enum RuleType
{
	AND,
	OR,
	FUNCTION
};

// Class representing a conditional rule. The enable_shared_from_this is used to create shared_ptr instances from 'this' pointer.
// This is needed when returning a pointer to itself.
class Rule : public std::enable_shared_from_this<Rule>
{
public:
	static RuleType StringToRuleType(std::string typeStr)
	{
		typeStr = Util::ToUpper(typeStr);

		if(typeStr == "AND")
		{
			return AND;
		}
		else if(typeStr == "OR")
		{
			return OR;
		}
		else if(typeStr == "FUNCTION")
		{
			return FUNCTION;
		}

		return AND; // Default to AND if unrecognized
	};

	// Factory method to create a Rule instance from a JSON rule and attributes.
	static std::shared_ptr<Rule> Create(JSONFile::JSONRule rule, shared_ptr<vector<shared_ptr<Attribute>>> attributes);

	// Constructor
	Rule();

	// Destructor
	~Rule();

	// Method to get an attribute by its ID from a list of attributes.
	std::shared_ptr<Attribute> GetAttributeByID(std::vector<std::shared_ptr<Attribute>> attributes, std::string id);

	// Method to add a condition to the rule.
	void AddCondition(std::shared_ptr<Condition> condition);
	
	// Method to evaluate the rule based on its conditions and type.
	void Evaluate();
	
	// Method to get the current state of the rule.
	bool GetState();
	
	// Method to convert a string representation of an operator to the corresponding enum value.
	Condition::Operator StringToOperator(std::string opStr);
	
	// Method to get the attribute associated with the rule.
	std::shared_ptr<Attribute> GetAttribute();
	
	// Method to add a linked rule.
	void AddRule(std::shared_ptr<Rule> rule);
	
	// Public member to store conditions associated with the rule.
	std::vector<std::shared_ptr<Condition>> conditions;

	std::shared_ptr<FunctionRule> function;

	string GetID()
	{
		return id;
	}

	RuleType GetRuleType()
	{
		return ruleType;
	}

private:
	// Unique identifier for the rule.
	std::string id;

	// The attribute that this rule infers or modifies.
	std::shared_ptr<Attribute> attribute;

	// The type of the rule (AND/OR).
	RuleType ruleType;
	
	// List of linked rules that depend on this rule.
	std::vector<std::weak_ptr<Rule>> linkedRules;
};
