#pragma once

#include <string>

class Relationship
{
	public:
	Relationship()
	{
	};

	~Relationship() = default;

	std::string id;
	std::string description;
	std::string sourceEntity;
	std::string targetEntity;
	std::string type;
};
