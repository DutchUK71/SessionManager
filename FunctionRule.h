#pragma once

#include <string>
#include <memory>
#include <vector>

using namespace std;

class FunctionRule
{
	public:
	FunctionRule()
	{
	};

	~FunctionRule()
	{
	}

	std::string id;
	std::string name;
	std::string targetEntityId;
	std::string targetAttributeId;
};
