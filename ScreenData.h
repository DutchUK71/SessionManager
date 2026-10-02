#pragma once

#include <string>
#include <vector>

using namespace std;

class ScreenData
{
	public:
	struct Data
	{
		string attributeId;
		string value;
	};

	struct DataRoot
	{
		string screenid;
		string nextScreenId;
		string entityId;
		string nextEntityId;
		string instanceId;
		string nextInstanceId;
		vector<Data> data;
	};

	void SaveFile(const ScreenData::DataRoot& data, const string& filePath);
};
