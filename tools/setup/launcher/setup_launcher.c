// RogueGalaxy-Setup.exe: starts the setup window without a console.
//
// It runs toolchain\python\pythonw.exe -X utf8 -m rgsetup.gui from tools\setup, in the folder this program is in.
// Nothing else: the window, the text and every decision are in tools\setup\rgsetup (docs/INSTALLER.md). The one
// message it can show is the one for a missing Python (a zip that was not unpacked whole). The sibling project's
// launcher (totk-pc-recomp, MIT, same author) is where this comes from.
//
// Plain C linked statically so it needs no DLL beside it. tools/make_release.py builds it with the bundled compiler;
// its icon and manifest come from a .res file that src/launcher/make_resources.py writes (windres is quarantined by
// some antivirus programs, see there).
#include <windows.h>

#include <wchar.h>

#define PATH_CHARS (MAX_PATH * 2)

static void fail(const wchar_t *folder)
{
    wchar_t text[PATH_CHARS + 256];
    swprintf(text, PATH_CHARS + 256,
             L"The setup could not find its Python in the folder \x201c%ls\x201d.\n\n"
             L"Unpack the whole zip again, then start the setup from there.",
             folder);
    MessageBoxW(NULL, text, L"Rogue Galaxy Recompiled Setup", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show)
{
    wchar_t folder[PATH_CHARS], python[PATH_CHARS], work[PATH_CHARS], command[PATH_CHARS + 128];
    DWORD n = GetModuleFileNameW(NULL, folder, PATH_CHARS);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show;
    if (n == 0 || n >= PATH_CHARS)
    {
        fail(L"?");
        return 1;
    }
    while (n > 0 && folder[n - 1] != L'\\')
    {
        n--;
    }
    folder[n > 0 ? n - 1 : 0] = 0;
    swprintf(python, PATH_CHARS, L"%ls\\toolchain\\python\\pythonw.exe", folder);
    swprintf(work, PATH_CHARS, L"%ls\\tools\\setup", folder);
    if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(work) == INVALID_FILE_ATTRIBUTES)
    {
        fail(folder);
        return 1;
    }
    swprintf(command, PATH_CHARS + 128, L"\"%ls\" -X utf8 -m rgsetup.gui", python);
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, 0, NULL, work, &si, &pi))
    {
        fail(folder);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
