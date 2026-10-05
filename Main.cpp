#include "httplib.h"
#include "Attribute.h"
#include "Rule.h"
#ifdef WIN32
#   include <crtdbg.h>
#endif
#include <cstdlib>
#include <iostream>
#include "json.hpp"
#include "JSONFile.h"
#include "Date.h"
#include "ScreenFile.h"
#include "ScreenData.h"
#include "DataManager.h"
#include "RuleSet.h"
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
