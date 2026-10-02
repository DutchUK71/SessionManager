#include "Util.h"

/////////////////////////////////////////////////////////////////////////////////////////////////
//
// Name: GenerateGUID
//
// Description: Generates a new GUID and returns it as a string in the standard format.
//
// Parameters: None.
//
// Returns:
//    string: The generated GUID as a string.
//
/////////////////////////////////////////////////////////////////////////////////////////////////
string Util::GenerateGUID()
{
	// Create a new GUID
	GUID guid;

	// Generate the GUID
	CoCreateGuid(&guid);
	
	// Format the GUID as a string
	char buffer[37];
	
	// Format: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
	snprintf(buffer, sizeof(buffer),
			 "%08x-%04x-%04x-%04x-%012llx",
			 guid.Data1,
			 guid.Data2,
			 guid.Data3,
			 (guid.Data4[0] << 8) | guid.Data4[1],
			 *((unsigned long long*)&guid.Data4[2]));
	
	// Return the GUID string
	return string(buffer);
}

string Util::ToUpper(std::string& str)
{
	string result = str;

	for (char& c : result)
	{
		c = toupper(c);
	}

	return result;
}