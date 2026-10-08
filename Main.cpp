#include "httplib.h"
#include "Rules/Attribute.h"
#include "Rules/Rule.h"
#ifdef WIN32
#   include <crtdbg.h>
#endif
#include <cstdlib>
#include <iostream>
#include "json.hpp"
#include "JSONFile.h"
#include "Rules/Date.h"
#include "ScreenFile.h"
#include "ScreenData.h"
#include "DataManager.h"
#include "Rules/RuleSet.h"
#include "RuleServer.h"

#define _CRTDBG_MAP_ALLOC

using Op = Condition::Operator;

using json = nlohmann::json;

int main()
{
	RuleServer rs = RuleServer();

	rs.StartServer();

	// Enable memory leak detection
#ifdef WIN32
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	return 0;
}
