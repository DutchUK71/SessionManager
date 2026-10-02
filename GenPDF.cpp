#include "GenPDF.h"
#include <iostream>
#include <cstdlib>
#include <cstring>

std::string GetEnvString(const char* name)
{
	char* buffer = nullptr;
	size_t length = 0;

	if(_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
	{
		return "";
	}

	std::string value(buffer);

	free(buffer);

	return value;
}

std::wstring GetEnvWideOrDefault(const char* name, const std::wstring& fallback)
{
	const std::string value = GetEnvString(name);

	if(value.empty())
	{
		return fallback;
	}

	return std::wstring(value.begin(), value.end());
}

bool FileExists(const std::wstring& path)
{
	const DWORD attributes = GetFileAttributesW(path.c_str());

	return attributes != INVALID_FILE_ATTRIBUTES
		&& (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ResolveWkhtmltopdfExe()
{
	const std::wstring configured = GetEnvWideOrDefault("CCP_WKHTMLTOPDF_EXE", L"");

	if(!configured.empty())
	{
		return configured;
	}

	const std::vector<std::wstring> candidates{
		L"tools\\wkhtmltox\\bin\\wkhtmltopdf.exe",
		L"..\\..\\..\\tools\\wkhtmltox\\bin\\wkhtmltopdf.exe"
	};

	for(const std::wstring& candidate : candidates)
	{
		if(FileExists(candidate))
		{
			return candidate;
		}
	}

	return L"wkhtmltopdf.exe";
}

DWORD GetEnvDwordOrDefault(const char* name, DWORD fallback)
{
	const std::string value = GetEnvString(name);

	if(value.empty())
	{
		return fallback;
	}

	try
	{
		const unsigned long parsed = std::stoul(value);

		return parsed == 0 ? fallback : static_cast<DWORD>(parsed);
	}
	catch(...)
	{
		return fallback;
	}
}

std::wstring GenPDF::GetTempFilePath(const std::wstring& prefix, const std::wstring& ext)
{
    wchar_t tempPath[MAX_PATH];
    if(!GetTempPathW(MAX_PATH, tempPath))
        throw std::runtime_error("GetTempPathW failed");

    wchar_t tempFile[MAX_PATH];
    if(!GetTempFileNameW(tempPath, prefix.c_str(), 0, tempFile))
        throw std::runtime_error("GetTempFileNameW failed");

    std::wstring path = tempFile;

    // Replace extension
    size_t dot = path.find_last_of(L'.');
    if(dot != std::wstring::npos)
        path = path.substr(0, dot);

    path += ext;

    DeleteFileW(tempFile);
    return path;
}

void GenPDF::WriteFileUtf8(const std::wstring& path, const std::string& data)
{
    std::ofstream out(path, std::ios::binary);
    if(!out)
        throw std::runtime_error("Failed to open file for writing");
    out.write(data.data(), data.size());
}

std::vector<uint8_t> GenPDF::ReadBinaryFile(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);

    if(!in)
    {
        throw std::runtime_error("Failed to open PDF output");
    }

    std::streamsize size = in.tellg();

    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    
    if(!in.read(reinterpret_cast<char*>(buffer.data()), size))
    {
        throw std::runtime_error("Failed to read PDF");
    }

    return buffer;
}

void GenPDF::RunProcessOrThrow(const std::wstring& cmdLine)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> buffer(cmdLine.begin(), cmdLine.end());
    buffer.push_back(L'\0');

    if(!CreateProcessW(
        nullptr,
        buffer.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi))
    {
        throw std::runtime_error("CreateProcessW failed");
    }

    const DWORD waitResult = WaitForSingleObject(pi.hProcess, GetEnvDwordOrDefault("CCP_PDF_TIMEOUT_MS", 60000));

    if(waitResult == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        throw std::runtime_error("wkhtmltopdf timed out");
    }

    if(waitResult == WAIT_FAILED)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        throw std::runtime_error("WaitForSingleObject failed");
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if(exitCode != 0)
    {
        std::ostringstream oss;
        oss << "wkhtmltopdf failed with exit code " << exitCode;
        throw std::runtime_error(oss.str());
    }
}

std::vector<uint8_t> GenPDF::GeneratePdfBytesFromHtml(const std::string& html)
{
    const std::wstring wkhtmltopdfExe = ResolveWkhtmltopdfExe();

    // Ensure full HTML document
    std::string fullHtml =
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
        + html +
        "</body></html>";

    std::wstring tempHtml = GetTempFilePath(L"htm", L".html");
    std::wstring tempPdf = GetTempFilePath(L"pdf", L".pdf");

    WriteFileUtf8(tempHtml, html);

    std::wstringstream cmd;
    cmd << L"\"" << wkhtmltopdfExe << L"\" "
        << L"--load-error-handling ignore "
        << L"--load-media-error-handling ignore "
        << L"\"" << tempHtml << L"\" "
        << L"\"" << tempPdf << L"\"";

    try
    {
        RunProcessOrThrow(cmd.str());

        auto pdfBytes = ReadBinaryFile(tempPdf);

        DeleteFileW(tempHtml.c_str());
        DeleteFileW(tempPdf.c_str());

        return pdfBytes;
    }
    catch(...)
    {
        DeleteFileW(tempHtml.c_str());
        DeleteFileW(tempPdf.c_str());

        throw;
    }
}
