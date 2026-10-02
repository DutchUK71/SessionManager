#include "Condition.h"
#include "Rule.h"

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: Condition (Constructor)
//
// Description: Initializes a new instance of the Condition class.
//
// Parameters:#
//   shared_ptr<Rule> parentRule: The parent conditional rule that this condition belongs to.
//   shared_ptr<Attribute> att: The attribute to be evaluated in this condition.
//   shared_ptr<ValueVariant> vv: The expected value for the condition.
//   Operator op: The operator to be used for the condition.
//
// Returns: None.
//
///////////////////////////////////////////////////////////////////////////////////////////////
Condition::Condition(shared_ptr<Rule> parentRule, shared_ptr<Attribute> att, shared_ptr<ValueVariant> vv, Operator op)
{
	// store input values.
	this->op = op;

	attribute = att;

	expectedValue = vv;

	att->AddRule(parentRule);
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: Condition (Constructor)
//
// Description: Initializes a new instance of the Condition class for rule-based conditions.
//
// Parameters:
//   shared_ptr<Rule> parentRule: The parent conditional rule that this condition belongs to.
//   shared_ptr<Rule> rule: The conditional rule to be evaluated in this condition.
//   bool b: The expected boolean value for the condition.
//
// Returns: None.
//
///////////////////////////////////////////////////////////////////////////////////////////////
Condition::Condition(shared_ptr<Rule> parentRule, shared_ptr<Rule> rule, bool b)
{
	// get attribute from rule
	shared_ptr<Attribute> att = rule->GetAttribute();

	// store attribute
	expectedValue = make_shared<ValueVariant>(b);

	// Link the attribute to the rule
	att->AddRule(rule);
	
	// Store the parent rule
	rule->AddRule(parentRule);

	isRuleCondition = true;

	// store weak reference
	ruleCondition = rule;
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: ~Condition (Destructor)
//
// Description: Destructor for the Condition class.
//
///////////////////////////////////////////////////////////////////////////////////////////////
Condition::~Condition()
{
};

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: Evaluate
//
// Description: Evaluates the condition based on the attribute's current value and the expected value.
//
// Returns:
//	   bool: The result of the condition evaluation (true if the condition is met, false otherwise).
//
///////////////////////////////////////////////////////////////////////////////////////////////
bool Condition::Evaluate()
{
	Evaluation evaluation = EvaluateKnowledge();

	state = evaluation.value;

	return state;
}

Condition::Evaluation Condition::EvaluateKnowledge()
{
	// Handle rule-based conditions
	if (isRuleCondition)
	{
		// Check for null expected value
		if(expectedValue == nullptr)
		{
			state = false;
		
			return Evaluation{state, Attribute::KnowledgeState::Unknown};
		}

		// Evaluate the state based on the linked rule's state
		if (auto rc = ruleCondition.lock())
		{
			shared_ptr<Attribute> ruleAttribute = rc->GetAttribute();

			if(ruleAttribute == nullptr || ruleAttribute->IsUnknown())
			{
				state = false;

				return Evaluation{state, Attribute::KnowledgeState::Unknown};
			}

			// Compare the rule's state with the expected value
			if (rc->GetState() == get<bool>(*expectedValue))
			{
				state = true;
			}
			else
			{
				state = false;
			}

			return Evaluation{
				state,
				ruleAttribute->IsCertain() ? Attribute::KnowledgeState::Certain : Attribute::KnowledgeState::Uncertain
			};
		}
		else
		{
			state = false;
		}
		
		return Evaluation{state, Attribute::KnowledgeState::Unknown};
	}

	// Not a rule condition, continue with attribute evaluation
	if(attribute == nullptr || expectedValue == nullptr || attribute->IsUnknown())
	{
		state = false;

		return Evaluation{state, Attribute::KnowledgeState::Unknown};
	}

	// Get the current value of the attribute
	ValueVariant currentValue = attribute->GetValue();

	// Evaluate based on the type of the current value and expected value
	if(holds_alternative<int>(currentValue) && holds_alternative<int>(*expectedValue))
	{
		EvaluateIntCondition();
	}
	else if (holds_alternative<double>(currentValue) && holds_alternative<double>(*expectedValue))
	{
		EvaluateDoubleCondition();
	}
	else if (holds_alternative<string>(currentValue) && holds_alternative<string>(*expectedValue))
	{
		EvaluateStringCondition();
	}
	else if (holds_alternative<bool>(currentValue) && holds_alternative<bool>(*expectedValue))
	{
		EvaluateBoolCondition();
	}
	else if (holds_alternative<Date>(currentValue) && holds_alternative<Date>(*expectedValue))
	{
		EvaluateDateCondition();
	}

	return Evaluation{
		state,
		attribute->IsCertain() ? Attribute::KnowledgeState::Certain : Attribute::KnowledgeState::Uncertain
	};
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: EvaluateIntCondition
//
// Description: Evaluates the condition for integer attributes based on the specified operator.
//
// Returns: None.
//
void Condition::EvaluateIntCondition()
{
	const int current = get<int>(attribute->GetValue());
	const int expected = get<int>(*expectedValue);

	switch (op)
	{
		case EQUALS: state = current == expected; break;
		case NOT_EQUALS: state = current != expected; break;
		case GREATER_THAN: state = current > expected; break;
		case LESS_THAN: state = current < expected; break;
		case GREATER_EQUAL: state = current >= expected; break;
		case LESS_EQUAL: state = current <= expected; break;
		default: state = false; break;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: EvaluateDoubleCondition
//
// Description: Evaluates the condition for double attributes based on the specified operator.
//
// Returns: None.
//
///////////////////////////////////////////////////////////////////////////////////////////////
void Condition::EvaluateDoubleCondition()
{
	const double current = get<double>(attribute->GetValue());
	const double expected = get<double>(*expectedValue);

	switch (op)
	{
		case EQUALS: state = current == expected; break;
		case NOT_EQUALS: state = current != expected; break;
		case GREATER_THAN: state = current > expected; break;
		case LESS_THAN: state = current < expected; break;
		case GREATER_EQUAL: state = current >= expected; break;
		case LESS_EQUAL: state = current <= expected; break;
		default: state = false; break;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: EvaluateStringCondition
//
// Description: Evaluates the condition for string attributes based on the specified operator.
//
// Returns: None.
//
///////////////////////////////////////////////////////////////////////////////////////////////
void Condition::EvaluateStringCondition()
{
	const string current = get<string>(attribute->GetValue());
	const string expected = get<string>(*expectedValue);

	switch (op)
	{
		case EQUALS: state = current == expected; break;
		case NOT_EQUALS: state = current != expected; break;
		default: state = false; break;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: EvaluateBoolCondition
//
// Description: Evaluates the condition for boolean attributes based on the specified operator.
//
// Returns: None.
//
///////////////////////////////////////////////////////////////////////////////////////////////
void Condition::EvaluateBoolCondition()
{
	const bool current = get<bool>(attribute->GetValue());
	const bool expected = get<bool>(*expectedValue);

	switch (op)
	{
		case EQUALS: state = current == expected; break;
		case NOT_EQUALS: state = current != expected; break;
		default: state = false; break;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: EvaluateDateCondition
//
// Description: Evaluates the condition for date attributes based on the specified operator.
//
// Returns: None.
//
///////////////////////////////////////////////////////////////////////////////////////////////
void Condition::EvaluateDateCondition()
{
	const Date current = get<Date>(attribute->GetValue());
	const Date expected = get<Date>(*expectedValue);

	switch (op)
	{
		case EQUALS: state = current == expected; break;
		case NOT_EQUALS: state = current != expected; break;
		case GREATER_THAN: state = current > expected; break;
		case LESS_THAN: state = current < expected; break;
		case GREATER_EQUAL: state = current >= expected; break;
		case LESS_EQUAL: state = current <= expected; break;
		default: state = false; break;
	}
}
