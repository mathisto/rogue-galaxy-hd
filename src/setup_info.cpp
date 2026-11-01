#include "setup_info.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace rg::setupinfo
{
namespace
{
constexpr const char *kSetupExe = "RogueGalaxy-Setup.exe";
constexpr const char *kState = "rg-install.json";
} // namespace

std::string recordedRelease()
{
    std::ifstream in(kState, std::ios::binary);
    if (!in)
    {
        return {};
    }
    std::stringstream text;
    text << in.rdbuf();
    const std::string s = text.str();
    // The first "release" is the file's own, before the steps' (the file is {"format", "release", ...}).
    const size_t key = s.find("\"release\"");
    if (key == std::string::npos)
    {
        return {};
    }
    const size_t open = s.find('"', s.find(':', key) + 1);
    const size_t close = open == std::string::npos ? open : s.find('"', open + 1);
    if (close == std::string::npos || close - open > 40)
    {
        return {};
    }
    return s.substr(open + 1, close - open - 1);
}

bool setupProgramPresent()
{
    std::error_code ec;
    return std::filesystem::is_regular_file(kSetupExe, ec);
}

bool openSetup()
{
#ifdef _WIN32
    if (!setupProgramPresent())
    {
        return false;
    }
    const std::filesystem::path exe = std::filesystem::absolute(kSetupExe);
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const std::wstring folder = exe.parent_path().wstring();
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                        folder.c_str(), &si, &pi))
    {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    return false;
#endif
}

} // namespace rg::setupinfo
