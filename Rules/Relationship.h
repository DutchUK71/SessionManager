#pragma once

#include <string>


struct Relationship
{
	std::string id;
	std::string description;
	std::string sourceEntity;
	std::string targetEntity;
	std::string type;
};
