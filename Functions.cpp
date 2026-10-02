#include "Functions.h"

int Functions::InstanceCount(const shared_ptr<Instance>& instance, const string& entityId, shared_ptr<Attribute> attribute)
{
	if(instance == nullptr || attribute == nullptr || attribute->GetType() != Attribute::INTEGER)
	{
		return 0;
	}

	int count = 0;	

	int currentVal = attribute->IsUnknown() ? -1 : get<int>(attribute->GetValue());

	for(const shared_ptr<Instance>& linkedInst : instance->linkedInstances)
	{
		if(linkedInst != nullptr && linkedInst->entityId == entityId)
		{
			count++;
		}
	}

	if(currentVal != count)
	{
		attribute->SetValue(ValueVariant(count));
	}

	return count;
}

void Functions::CopyAttribute(const shared_ptr<Instance>& instance, const string& sourceEntityId, const string& sourceAttributeId, shared_ptr<Attribute> targetAttribute)
{
	if(instance == nullptr || targetAttribute == nullptr || sourceEntityId.empty() || sourceAttributeId.empty())
	{
		return;
	}

	shared_ptr<Instance> sourceInstance = nullptr;

	if(instance->entityId == sourceEntityId)
	{
		sourceInstance = instance;
	}
	else
	{
		for(const shared_ptr<Instance>& linkedInst : instance->linkedInstances)
		{
			if(linkedInst != nullptr && linkedInst->entityId == sourceEntityId)
			{
				sourceInstance = linkedInst;

				break;
			}
		}
	}

	if(sourceInstance == nullptr)
	{
		return;
	}

	shared_ptr<Attribute> sourceAttribute = sourceInstance->GetAttributeByID(sourceAttributeId);

	if(sourceAttribute == nullptr)
	{
		return;
	}

	if(sourceAttribute->IsUnknown())
	{
		targetAttribute->SetValueFromString("");

		return;
	}

	const string sourceValue = sourceAttribute->GetStringValue();

	targetAttribute->SetValueFromString(sourceValue);
}

Functions::FunctionName Functions::GetFunctionType(string typeStr)
{
	typeStr = Util::ToUpper(typeStr);

	if(typeStr == "INSTANCECOUNT")
	{
		return FunctionInstanceCount;
	}
	else if(typeStr == "COPY")
	{
		return FunctionCopy;
	}

	return Functions::FunctionName::FunctionUnknown;
}

void Functions::HandleFunction(shared_ptr<Rule> rule, shared_ptr<Instance> instance)
{
	if(rule == nullptr || rule->function == nullptr || instance == nullptr)
	{
		return;
	}

	string name = rule->function->name;

	shared_ptr<Attribute> att = rule->GetAttribute();

	if(att == nullptr)
	{
		return;
	}

	Functions::FunctionName fn = Functions::GetFunctionType(name);

	switch(fn)
	{
		case Functions::FunctionName::FunctionInstanceCount: InstanceCount(instance, rule->function->targetEntityId, att); break;
		case Functions::FunctionName::FunctionCopy: CopyAttribute(instance, rule->function->targetEntityId, rule->function->targetAttributeId, att); break;
	}
}
