#include "win_app.hpp"

#include <windows.h>
#include <shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool native = false;
    bool register_host = false;
    bool unregister_host = false;
    std::wstring chrome_id;
    std::wstring edge_id;
    for (int i = 1; i < count; ++i) {
        const std::wstring argument = arguments[i];
        if (argument == L"--native-host") native = true;
        else if (argument == L"--register") register_host = true;
        else if (argument == L"--unregister") unregister_host = true;
        else if (argument == L"--chrome-id" && i + 1 < count) chrome_id = arguments[++i];
        else if (argument == L"--edge-id" && i + 1 < count) edge_id = arguments[++i];
    }
    if (arguments) LocalFree(arguments);
    if (native) return glint::run_native_host();
    if (register_host || unregister_host) return glint::run_register(unregister_host, chrome_id.c_str(), edge_id.c_str());
    return glint::run_desktop();
}
