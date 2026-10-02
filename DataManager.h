#pragma once

#include <memory>
#include <vector>
#include "Attribute.h"
#include "ScreenData.h"
#include "ScreenFile.h"
#include "Rule.h"
#include "Entity.h"
#include "RuleSet.h"
#include "Summary.h"

class DataManager
{
	public:
	struct RadioSelectionChange
	{
		string entityId;
		string instanceId;
		string attributeId;
	};

	DataManager() = default;
	~DataManager() = default;

	shared_ptr<ScreenFile::Screen> HandleData(shared_ptr<vector<shared_ptr<Entity>>> entities,
		                                      shared_ptr<ScreenData::DataRoot> data,
		                                      shared_ptr<ScreenFile::Screen> screen,
		                                      shared_ptr<ScreenFile::ScreenData> screens,
		                                      string directTo,
		                                      shared_ptr<RuleSet>& ruleSet);

	shared_ptr<ScreenFile::Screen> GetSessionData(shared_ptr<vector<shared_ptr<Entity>>> entities, 
		                                          shared_ptr<ScreenFile::Screen> screen, 
		                                          string entityId, 
		                                          string instanceId,
										   		  shared_ptr<RuleSet>& ruleSet,
												  const ScreenFile::ScreenData* screens = nullptr);

	ScreenFile::CollectData GetPageData(shared_ptr<vector<shared_ptr<Entity>>> entities,
		shared_ptr<ScreenFile::Screen> screen,
		string entityId,
		string instanceId,
		int page,
		shared_ptr<RuleSet>& ruleSet,
		string filter);
	bool IsCollectTargetInScope(shared_ptr<vector<shared_ptr<Entity>>> entities,
		const ScreenFile::Element& element,
		const shared_ptr<Instance>& contextInstance,
		const string& targetInstanceId);

	shared_ptr<ScreenFile::Screen> GetScreen(shared_ptr<vector<shared_ptr<Entity>>> entities, 
		                                     shared_ptr<ScreenFile::ScreenData> screens, 
											 const ScreenLocation& loc,
		                                     shared_ptr<RuleSet> ruleSet,
											 bool skipDataLoad = false);

	void UpdateAttributeValue(shared_ptr<vector<shared_ptr<Entity>>> entities, const string& attributeId, const string& entityId, const string& instanceId, const string& strValue, const string& screenId);
	void SetDisplayElement(shared_ptr<vector<shared_ptr<Entity>>> entities, shared_ptr<ScreenFile::Screen> screen);
	vector<RadioSelectionChange> ClearHiddenRadioSelections(shared_ptr<vector<shared_ptr<Entity>>> entities, shared_ptr<ScreenFile::Screen> screen);
	shared_ptr<ScreenFile::Element> GetElementById(shared_ptr<ScreenFile::Screen> screen, const string& elementId);
	shared_ptr<ScreenFile::Screen> GetFirstEntityScreen(shared_ptr<ScreenFile::ScreenData> screens, const string& entityId);
	string GetFirstInstanceId(shared_ptr<vector<shared_ptr<Entity>>> entities, const string& entityId);
	void DeleteInstance(string entityId, string instanceId, shared_ptr<vector<shared_ptr<Entity>>> entities);
	shared_ptr<Instance> GetEntityInstanceById(shared_ptr<vector<shared_ptr<Entity>>> entities, const string& entityId, const string& instanceId);
	Summary::SummaryData GetSummaryData(shared_ptr<vector<shared_ptr<Entity>>> entities,
		const ScreenFile::ScreenData* screens = nullptr,
		shared_ptr<Instance> scopeInstance = nullptr);
	shared_ptr<ScreenFile::Screen> GetScreenByID(shared_ptr<ScreenFile::ScreenData> screens, const string& screenId);
	shared_ptr<Entity> GetEntityByID(shared_ptr<vector<shared_ptr<Entity>>> entities, const string& entityId);
	void Copy(shared_ptr<Instance> sourceInstance, shared_ptr<Instance> targetInstance);

	private:
	void ConvertValue(const string& strValue, Attribute::AttributeType type, shared_ptr<ValueVariant> outValue);
	shared_ptr<ScreenFile::Screen> GetNextScreen(shared_ptr<ScreenFile::ScreenData> screens, const string& currentScreenID, const string& entityId);
	void GetPage(int page, ScreenFile::Element& element, shared_ptr<Instance> instance, shared_ptr<RuleSet>& ruleSet, string filter = "");
	bool ApplyFilter(const ScreenFile::CollectInstance& instance, const string& filter);
};
