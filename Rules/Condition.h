#pragma once

#include "Attribute.h"
#include <memory>
#include <variant>

using namespace std;

// Forward declaration of Rule class
class Rule;

// Class representing a condition within a conditional rule
class Condition 
{
public:
	// Enum to represent the operator used in the condition
    enum Operator
    {
        EQUALS,
        NOT_EQUALS,
        GREATER_THAN,
        LESS_THAN,
        GREATER_EQUAL,
        LESS_EQUAL
    };

	// Enum to represent the type of condition
    enum ConditionType
    {
        ATTRIBUTE,
        RULE
	};

	// Constructor for attribute-based condition
    Condition(shared_ptr<Rule> parentRule, shared_ptr<Attribute> att, shared_ptr<ValueVariant> vv, Operator op);
    
	// Constructor for rule-based condition
    Condition(shared_ptr<Rule> parentRule, shared_ptr<Rule> rule, bool b);
    
	struct Evaluation
	{
		bool value = false;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Unknown;
	};

	// Method to evaluate the condition
    bool Evaluate();
	Evaluation EvaluateKnowledge();
	
	// Destructor
    ~Condition();
	
	// Method to get the associated attribute
    shared_ptr<Attribute> GetAttribute() { return attribute; }

private:
	// Methods to evaluate conditions based on attribute type
    void EvaluateIntCondition();
    
	// Method to evaluate conditions for double attributes
    void EvaluateDoubleCondition();
	
	// Method to evaluate conditions for string attributes
    void EvaluateStringCondition();
	
	// Method to evaluate conditions for boolean attributes
    void EvaluateBoolCondition();

	// Method to evaluate conditions for date attributes
    void EvaluateDateCondition();

	// Pointer to the attribute being evaluated
    shared_ptr<Attribute> attribute;
    
	// Pointer to the expected value for the condition
    shared_ptr<ValueVariant> expectedValue;
    
	// Current state of the condition (true/false)
    bool state = false;
	
	// Flag indicating if this is a rule-based condition
    bool isRuleCondition = false;
	
	// Weak pointer to the conditional rule if this is a rule-based condition. weak_ptr avoids ownership cycles.
	weak_ptr<Rule> ruleCondition;
	
	// The operator used in the condition
    Operator op;
};

