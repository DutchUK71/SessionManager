#include "Rule.h"
#include <algorithm>
#include <exception>

namespace
{
	struct RuleEvaluation
	{
		bool value = false;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Unknown;
	};
}

////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: Rule (Constructor)
//
// Description: Initializes a new instance of the Rule class.
//
// Parameters:
//    RuleType type: The type of the conditional rule (AND/OR).
//
//////////////////////////////////////////////////////////////////////////////////////////////
Rule::Rule()
{
}

////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: ~Rule (Destructor)
//
// Description: Destructor for the Rule class.
//
////////////////////////////////////////////////////////////////////////////////////////////////
Rule::~Rule()
{
}

/////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: Create (Factory Method)
//
// Description: Factory method to create a Rule instance from a JSON rule and attributes.
//
// Parameters:
//    RuleType type: The type of the conditional rule (AND/OR).
//    JSONFile::JSONRule rule: The JSON representation of the rule.
//    std::vector<std::shared_ptr<Attribute>> attributes: The list of attributes to be used.
//
// Returns:
//    std::shared_ptr<Rule>: A shared pointer to the created Rule instance.
//
///////////////////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<Rule> Rule::Create(JSONFile::JSONRule rule, shared_ptr<vector<shared_ptr<Attribute>>> attributes)
{
	RuleType type = StringToRuleType(rule.ruletype);

	// Create a conditional rule as a shared_ptr
	std::shared_ptr<Rule> cr = std::make_shared<Rule>();

	// Set ID
	cr->id = rule.ruleid;

	// Get attribute by ID
	if(attributes != nullptr)
	{
		cr->attribute = cr->GetAttributeByID(*attributes, rule.attributeid);
	}

	cr->ruleType = type;

	if(rule.function.has_value())
	{
		cr->function = make_shared<FunctionRule>();

		cr->function->id = rule.function->id;
		cr->function->name = rule.function->name;
		cr->function->targetEntityId = rule.function->targetentityid;
		cr->function->targetAttributeId = rule.function->targetattribute.value_or("");
	}
	else
	{
		if(attributes == nullptr)
		{
			return cr;
		}

		for(const JSONFile::JSONCondition& jc : rule.conditions)
		{
			std::shared_ptr<Attribute> condAtt = cr->GetAttributeByID(*attributes, jc.attributeid);

			if(condAtt == nullptr)
			{
				continue;
			}

			std::shared_ptr<ValueVariant> expectedValue = nullptr;

			try
			{
				if(condAtt->GetType() == Attribute::INTEGER)
				{
					ValueVariant convertedValue;

					if(Attribute::TryConvertValue(jc.expectedvalue, Attribute::INTEGER, convertedValue))
					{
						expectedValue = std::make_shared<ValueVariant>(convertedValue);
					}
				}
				else if(condAtt->GetType() == Attribute::DOUBLE)
				{
					ValueVariant convertedValue;

					if(Attribute::TryConvertValue(jc.expectedvalue, Attribute::DOUBLE, convertedValue))
					{
						expectedValue = std::make_shared<ValueVariant>(convertedValue);
					}
				}
				else if(condAtt->GetType() == Attribute::STRING)
				{
					expectedValue = std::make_shared<ValueVariant>(jc.expectedvalue);
				}
				else if(condAtt->GetType() == Attribute::BOOLEAN)
				{
					ValueVariant convertedValue;

					if(Attribute::TryConvertValue(jc.expectedvalue, Attribute::BOOLEAN, convertedValue))
					{
						expectedValue = std::make_shared<ValueVariant>(convertedValue);
					}
				}
				else if(condAtt->GetType() == Attribute::DATE)
				{
					string dateValue = jc.expectedvalue;

					dateValue.erase(std::remove(dateValue.begin(), dateValue.end(), '-'), dateValue.end());

					if(Date::IsValidDate(dateValue))
					{
						expectedValue = std::make_shared<ValueVariant>(Date(dateValue));
					}
				}
			}
			catch(const std::exception&)
			{
				expectedValue = nullptr;
			}

			Condition::Operator op = cr->StringToOperator(jc.operant);

			std::shared_ptr<Condition> condition = std::make_shared<Condition>(cr, condAtt, expectedValue, op);

			//Add code to check for rule

			cr->AddCondition(condition);
		}
	}

	return cr;
}

Condition::Operator Rule::StringToOperator(std::string opStr)
{
	if (opStr == "equals")
	{
		return Condition::EQUALS;
	}
	else if (opStr == "not_equals")
	{
		return Condition::NOT_EQUALS;
	}
	else if (opStr == "greater_than")
	{
		return Condition::GREATER_THAN;
	}
	else if (opStr == "less_than")
	{
		return Condition::LESS_THAN;
	}
	else if (opStr == "greater_equal")
	{
		return Condition::GREATER_EQUAL;
	}
	else if (opStr == "less_equal")
	{
		return Condition::LESS_EQUAL;
	}

	return Condition::EQUALS;
}

std::shared_ptr<Attribute> Rule::GetAttributeByID(std::vector<std::shared_ptr<Attribute>> attributes, std::string id)
{
	for (const std::shared_ptr<Attribute>& att : attributes)
	{
		if (att->GetID() == id)
		{
			return att;
		}
	}

	return nullptr;
}

void Rule::AddCondition(std::shared_ptr<Condition> condition)
{
	conditions.push_back(condition);
}

bool Rule::GetState()
{
	if (!attribute || attribute->GetType() != Attribute::BOOLEAN || attribute->IsUnknown())
	{
		return false;
	}

	return std::get<bool>(attribute->GetValue());
}

std::shared_ptr<Attribute> Rule::GetAttribute()
{
	return attribute;
}

void Rule::AddRule(std::shared_ptr<Rule> rule)
{
	for(const auto& linkedRuleWeak : linkedRules)
	{
		if(auto linkedRule = linkedRuleWeak.lock())
		{
			if(linkedRule == rule)
			{
				return;
			}
		}
	}

	// insert weak_ptr to avoid ownership cycles
	std::weak_ptr<Rule> wp = rule;

	linkedRules.push_back(wp);
}

void Rule::Evaluate()
{
	RuleEvaluation evaluation;
	bool stateChanged = false;
	bool wasUnknown = attribute == nullptr || attribute->IsUnknown();
	Attribute::KnowledgeState previousKnowledgeState = attribute != nullptr ? attribute->GetKnowledgeState() : Attribute::KnowledgeState::Unknown;
	bool previousState = GetState();

	if (ruleType == AND)
	{
		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawKnown = false;
		bool allCertainTrue = true;

		for (const auto& conditionPtr : conditions)
		{
			if(!conditionPtr)
			{
				sawUnknown = true;
				allCertainTrue = false;

				continue;
			}

			Condition::Evaluation condition = conditionPtr->EvaluateKnowledge();

			if(condition.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;
				allCertainTrue = false;

				continue;
			}

			sawKnown = true;

			if(condition.knowledgeState == Attribute::KnowledgeState::Certain && !condition.value)
			{
				evaluation = RuleEvaluation{false, Attribute::KnowledgeState::Certain};
				break;
			}

			if(condition.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
				allCertainTrue = false;
			}
			else if(!condition.value)
			{
				sawUncertain = true;
				allCertainTrue = false;
			}
		}

		if(evaluation.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			if(conditions.empty() || allCertainTrue)
			{
				evaluation = RuleEvaluation{true, Attribute::KnowledgeState::Certain};
			}
			else if(!sawKnown && sawUnknown)
			{
				evaluation = RuleEvaluation{false, Attribute::KnowledgeState::Unknown};
			}
			else if(sawUncertain || sawUnknown)
			{
				evaluation = RuleEvaluation{false, Attribute::KnowledgeState::Uncertain};
			}
		}
	}
	else if (ruleType == OR)
	{
		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawKnown = false;
		bool allCertainFalse = true;

		for (const auto& conditionPtr : conditions)
		{
			if(!conditionPtr)
			{
				sawUnknown = true;
				allCertainFalse = false;

				continue;
			}

			Condition::Evaluation condition = conditionPtr->EvaluateKnowledge();

			if(condition.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;
				allCertainFalse = false;

				continue;
			}

			sawKnown = true;

			if(condition.knowledgeState == Attribute::KnowledgeState::Certain && condition.value)
			{
				evaluation = RuleEvaluation{true, Attribute::KnowledgeState::Certain};
				break;
			}

			if(condition.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
				allCertainFalse = false;
			}
			else if(condition.value)
			{
				sawUncertain = true;
				allCertainFalse = false;
			}
		}

		if(evaluation.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			if(conditions.empty() || allCertainFalse)
			{
				evaluation = RuleEvaluation{false, Attribute::KnowledgeState::Certain};
			}
			else if(!sawKnown && sawUnknown)
			{
				evaluation = RuleEvaluation{false, Attribute::KnowledgeState::Unknown};
			}
			else if(sawUncertain || sawUnknown)
			{
				evaluation = RuleEvaluation{false, Attribute::KnowledgeState::Uncertain};
			}
		}
	}
	else if(ruleType == FUNCTION)
	{
		return;
	}

	if(attribute)
	{
		if(evaluation.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			attribute->MakeUnknown(false);
		}
		else
		{
			attribute->SetValue(ValueVariant(evaluation.value), false);
			attribute->SetKnowledgeState(evaluation.knowledgeState, false);
		}

		stateChanged = wasUnknown != attribute->IsUnknown()
			|| previousState != GetState()
			|| previousKnowledgeState != attribute->GetKnowledgeState();
	}

	if(!stateChanged)
	{
		return;
	}

	// evaluate linked rules
	// linkedRules stores weak_ptrs
	std::vector<std::weak_ptr<Rule>> expired;

	for (const auto& weakRule : linkedRules)
	{
		if (auto linkedRule = weakRule.lock())
		{
			linkedRule->Evaluate();
		}
		else
		{
			// collect expired for cleanup
			expired.push_back(weakRule);
		}
	}

	// remove expired entries
	for (const auto& e : expired)
	{
		auto it = std::find_if(linkedRules.begin(), linkedRules.end(), [&](const std::weak_ptr<Rule>& w)
		{ 
			return w.lock() == e.lock(); 
		});
		
		if (it != linkedRules.end())
		{
			linkedRules.erase(it);
		}
	}
}
