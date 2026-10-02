#include "ScreenData.h"
#include "json.hpp"
#include <fstream>
#include <iostream>

using json = nlohmann::json;
using namespace std;

// DATA
void from_json(const json& j, ScreenData::Data& e)
{
	e.attributeId = j.at("attributeId").get<std::string>();
	e.value = j.at("value").get<std::string>();
}

void to_json(json& j, const ScreenData::Data& e)
{
	j["attributeId"] = e.attributeId;
	j["value"] = e.value;
}

// DATAROOT
void from_json(const json& j, ScreenData::DataRoot& e)
{
	e.screenid = j.at("screenid").get<std::string>();
	e.entityId = j.at("entityId").get<std::string>();
	e.instanceId = j.at("instanceId").get<std::string>();
	e.data = j.at("data").get<std::vector<ScreenData::Data>>();
}

void to_json(json& j, const ScreenData::DataRoot& e)
{
	j["screenid"] = e.screenid;
	j["entityId"] = e.entityId;
	j["instanceId"] = e.instanceId;
	j["data"] = e.data;
}

void ScreenData::SaveFile(const ScreenData::DataRoot& data, const string& filePath)
{
	json j = data;

	std::ofstream file(filePath);

	if(!file.is_open())
	{
		std::cout << "Failed to open file for writing: " << filePath << std::endl;

		return;
	}

	file << j.dump(4);
}