#pragma once

#include <memory>
#include "Rules/Instance.h"

class Functions
{
	public:

	enum FunctionName
	{
		FunctionUnknown,
		FunctionInstanceCount,
		FunctionCopy
	};

	int InstanceCount(const shared_ptr<Instance>& instance, const string& entityId, shared_ptr<Attribute> attribute);

	void CopyAttribute(const shared_ptr<Instance>& instance, const string& sourceEntityId, const string& sourceAttributeId, shared_ptr<Attribute> targetAttribute);
	
	void HandleFunction(shared_ptr<Rule> rule, shared_ptr<Instance> instance);

	static FunctionName GetFunctionType(string typeStr);
};
