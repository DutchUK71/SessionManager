#pragma once

#include "HtmlToPdfWin.hpp"
#include <string>
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <stdexcept>

class GenPDF
{
	public:
	std::vector<uint8_t> GeneratePdfBytesFromHtml(const std::string& html);

	private:
	void RunProcessOrThrow(const std::wstring& cmdLine);
	std::vector<uint8_t> ReadBinaryFile(const std::wstring& path);
	void WriteFileUtf8(const std::wstring& path, const std::string& data);
	std::wstring GetTempFilePath(const std::wstring& prefix, const std::wstring& ext);
};