// HtmlToPdfWin.hpp
#pragma once

#define NOMINMAX
#include <windows.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class HtmlToPdfWin
{
    public:
    struct Options
    {
        std::wstring wkhtmltopdfExe; // e.g. L"wkhtmltopdf.exe" or a full path
        std::wstring pageSize;  //= L"A4";
        std::wstring orientation = L"Landscape"; // Portrait | Landscape
        int marginTopMm = 10;
        int marginRightMm = 10;
        int marginBottomMm = 10;
        int marginLeftMm = 10;
        bool disableSmartShrinking = false;

        // Because you said "no external assets", this is usually fine as false.
        bool enableLocalFileAccess = false;
    };

    explicit HtmlToPdfWin(Options opt) : opt_(std::move(opt))
    {
        if(opt_.wkhtmltopdfExe.empty())
        {
            throw std::runtime_error("wkhtmltopdfExe is required (full path to wkhtmltopdf.exe).");
        }
    }

    // Saves PDF to outputPdfPath (creates parent dirs if needed).
    void savePdf(const std::string& htmlUtf8, const std::wstring& outputPdfPath) const
    {
        ensureParentDirExists(outputPdfPath);

        // Write HTML to a temp UTF-8 file
        std::wstring tmpHtml = makeTempFilePath(L"html2pdf_", L".html");
        writeUtf8File(tmpHtml, htmlUtf8);

        // Build command line for CreateProcessW (single mutable string)
        std::wstring cmdLine = buildCommandLine(tmpHtml, outputPdfPath);

        // Launch wkhtmltopdf
        DWORD exitCode = 0;
        try
        {
            auto [exitCode, out] = runProcessCaptureOutput(cmdLine);
            // cleanup tmpHtml...

            if(exitCode != 0)
            {
                std::ostringstream oss;

                oss << "wkhtmltopdf failed with exit code " << exitCode << "\n"
                    << "wkhtmltopdf output:\n" << out;

                throw std::runtime_error(oss.str());
            }
        }
        catch(...)
        {
            DeleteFileW(tmpHtml.c_str());
            throw;
        }

        // Cleanup temp HTML
        DeleteFileW(tmpHtml.c_str());

        if(exitCode != 0)
        {
            std::ostringstream oss;
            oss << "wkhtmltopdf failed with exit code " << exitCode;
            throw std::runtime_error(oss.str());
        }

        // Basic existence check
        DWORD attrs = GetFileAttributesW(outputPdfPath.c_str());
        if(attrs == INVALID_FILE_ATTRIBUTES)
        {
            throw std::runtime_error("PDF output was not created: output path invalid.");
        }
    }

    private:
    Options opt_;

    static void ensureParentDirExists(const std::wstring& filePath)
    {
        // Simple parent-dir creation (one level at a time)
        // If you want robust recursive creation, you can swap this for std::filesystem.
        auto pos = filePath.find_last_of(L"\\/");
        if(pos == std::wstring::npos) return;

        std::wstring dir = filePath.substr(0, pos);
        if(dir.empty()) return;

        // CreateDirectoryW succeeds if already exists? It fails with ERROR_ALREADY_EXISTS.
        // We'll create segments progressively.
        for(size_t i = 1; i < dir.size(); ++i)
        {
            if(dir[i] == L'\\' || dir[i] == L'/')
            {
                std::wstring part = dir.substr(0, i);
                if(!part.empty() && part.back() != L':')
                {
                    CreateDirectoryW(part.c_str(), nullptr);
                }
            }
        }
        CreateDirectoryW(dir.c_str(), nullptr);
    }

    static std::wstring makeTempFilePath(const std::wstring& prefix, const std::wstring& ext)
    {
        wchar_t tempPathBuf[MAX_PATH + 1]{0};
        DWORD n = GetTempPathW(MAX_PATH, tempPathBuf);
        if(n == 0 || n > MAX_PATH)
        {
            throw std::runtime_error("GetTempPathW failed.");
        }

        wchar_t tempFileBuf[MAX_PATH + 1]{0};
        // GetTempFileNameW creates a file; we’ll just use the name then overwrite it.
        if(GetTempFileNameW(tempPathBuf, prefix.c_str(), 0, tempFileBuf) == 0)
        {
            throw std::runtime_error("GetTempFileNameW failed.");
        }

        std::wstring path = tempFileBuf;

        // Replace the .tmp extension with ext
        auto dot = path.find_last_of(L'.');
        if(dot != std::wstring::npos) path = path.substr(0, dot);
        path += ext;

        // Remove the created .tmp file, we'll create our own .html
        DeleteFileW(tempFileBuf);

        return path;
    }

    static void writeUtf8File(const std::wstring& path, const std::string& utf8)
    {
        std::ofstream out(path, std::ios::binary);
        if(!out) throw std::runtime_error("Failed to open temp HTML file for writing.");
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        if(!out) throw std::runtime_error("Failed to write HTML data.");
    }

    static std::wstring q(const std::wstring& s)
    {
        // Quote for CreateProcessW command line.
        // This is sufficient for typical paths with spaces.
        std::wstring out = L"\"";
        for(wchar_t c : s)
        {
            if(c == L'"') out += L"\\\"";
            else out += c;
        }
        out += L"\"";
        return out;
    }

    std::wstring buildCommandLine(const std::wstring& htmlPath, const std::wstring& pdfPath) const
    {
        std::wstringstream ss;

        // Note: CreateProcessW wants a mutable command line string; we return one.
        ss << q(opt_.wkhtmltopdfExe);

        //ss << L" --quiet";
        //ss << L" --page-size " << q(opt_.pageSize);
        //ss << L" --orientation " << q(opt_.orientation);

        ss << L" --margin-top " << opt_.marginTopMm << L"mm";
        ss << L" --margin-right " << opt_.marginRightMm << L"mm";
        ss << L" --margin-bottom " << opt_.marginBottomMm << L"mm";
        ss << L" --margin-left " << opt_.marginLeftMm << L"mm";
        ss << L" --load-error-handling ignore";
        ss << L" --load-media-error-handling ignore";
        ss << L" --print-media-type";
        ss << L" --disable-smart-shrinking";
        
        if(opt_.enableLocalFileAccess)
        {
            ss << L" --enable-local-file-access";
        }

        if(opt_.disableSmartShrinking)
        {
            ss << L" --disable-smart-shrinking";
        }

        // Input + output
        ss << L" " << q(htmlPath) << L" " << q(pdfPath);

        return ss.str();
    }

    static std::pair<DWORD, std::string> runProcessCaptureOutput(std::wstring cmdLine)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr, writePipe = nullptr;
        if(!CreatePipe(&readPipe, &writePipe, &sa, 0))
        {
            throw std::runtime_error("CreatePipe failed.");
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};

        std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
        buf.push_back(L'\0');

        BOOL ok = CreateProcessW(
            nullptr,
            buf.data(),
            nullptr,
            nullptr,
            TRUE,              // inherit handles so child can write to pipe
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi
        );

        CloseHandle(writePipe); // parent closes write end

        if(!ok)
        {
            CloseHandle(readPipe);
            DWORD err = GetLastError();
            std::ostringstream oss;
            oss << "CreateProcessW failed. Win32 error: " << err;
            throw std::runtime_error(oss.str());
        }

        // Read output
        std::string output;
        char buffer[4096];
        DWORD bytesRead = 0;
        while(ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
        {
            output.append(buffer, buffer + bytesRead);
        }

        WaitForSingleObject(pi.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        CloseHandle(readPipe);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        return {exitCode, output};
    }
};