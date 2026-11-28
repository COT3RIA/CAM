#include "HostProject.h"
#include "FileNaming.h"

#if JUCE_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace
{
    struct SearchState
    {
        DWORD processId = 0;
        std::string result;
    };

    BOOL CALLBACK enumWindowsProc (HWND hwnd, LPARAM lParam)
    {
        auto* state = reinterpret_cast<SearchState*> (lParam);

        DWORD pid = 0;
        GetWindowThreadProcessId (hwnd, &pid);

        if (pid != state->processId || ! IsWindowVisible (hwnd))
            return TRUE;

        wchar_t title[512] = {};
        const int length = GetWindowTextW (hwnd, title, 512);

        if (length <= 0)
            return TRUE;

        const auto name = naming::extractProjectName (juce::String (title).toStdString());

        if (! name.empty())
        {
            state->result = name;
            return FALSE; // нашли, дальше не ищем
        }

        return TRUE;
    }
}

juce::String detectHostProjectName()
{
    SearchState state;
    state.processId = GetCurrentProcessId();
    EnumWindows (enumWindowsProc, reinterpret_cast<LPARAM> (&state));
    return juce::String::fromUTF8 (state.result.c_str());
}

#else

// На macOS и Linux заголовки окон DAW недоступны без системных расширений: используется поле в плагине
juce::String detectHostProjectName()
{
    return {};
}

#endif
