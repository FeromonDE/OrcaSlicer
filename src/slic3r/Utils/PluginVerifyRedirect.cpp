#include "PluginVerifyRedirect.hpp"

#if defined(_WIN32) && defined(ORCA_BAMBU_SIGNED_HOST_BRIDGE)

#include <windows.h>
#include <wincrypt.h>
#include <intrin.h>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MinHook.h"

namespace {

wchar_t g_our_dll[MAX_PATH] = {0};
wchar_t g_our_exe[MAX_PATH] = {0};
wchar_t g_genuine_dll_w[MAX_PATH] = {0};
char    g_genuine_dll_a[MAX_PATH] = {0};
wchar_t g_genuine_exe_w[MAX_PATH] = {0};
char    g_genuine_exe_a[MAX_PATH] = {0};

using fn_gmfw = DWORD (WINAPI *)(HMODULE, LPWSTR, DWORD);
using fn_gmfa = DWORD (WINAPI *)(HMODULE, LPSTR, DWORD);
using fn_cqo  = BOOL (WINAPI *)(DWORD, const void*, DWORD, DWORD, DWORD,
                                DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);

fn_gmfw o_gmfw = nullptr;
fn_gmfa o_gmfa = nullptr;
fn_cqo  o_cqo  = nullptr;

bool find_bambu_studio(wchar_t* exe, wchar_t* dir)
{
    exe[0] = 0;
    dir[0] = 0;

    HKEY hk = nullptr;
    if (::RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Bambu Studio",
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &hk) != ERROR_SUCCESS)
        return false;

    wchar_t icon[MAX_PATH + 1] = {0};
    DWORD cb = sizeof(icon) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG rc = ::RegQueryValueExW(hk, L"DisplayIcon", nullptr, &type,
                                       reinterpret_cast<LPBYTE>(icon), &cb);
    ::RegCloseKey(hk);

    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;

    wchar_t* comma = wcsrchr(icon, L',');
    wchar_t* slash = wcsrchr(icon, L'\\');
    if (comma && (!slash || comma > slash))
        *comma = 0;

    slash = wcsrchr(icon, L'\\');
    if (!slash || ::GetFileAttributesW(icon) == INVALID_FILE_ATTRIBUTES)
        return false;

    wcscpy_s(exe, MAX_PATH, icon);
    slash[1] = 0;
    wcscpy_s(dir, MAX_PATH, icon);
    return true;
}

bool resolve_genuine_files()
{
    wchar_t genuine_exe[MAX_PATH] = {0};
    wchar_t genuine_dir[MAX_PATH] = {0};

    const char* env_dll = std::getenv("BAMBU_BRIDGE_GENUINE_DLL");
    const char* env_exe = std::getenv("BAMBU_BRIDGE_GENUINE_EXE");

    if (env_dll && *env_dll) {
        ::MultiByteToWideChar(CP_UTF8, 0, env_dll, -1, g_genuine_dll_w, MAX_PATH);
    }
    if (env_exe && *env_exe) {
        ::MultiByteToWideChar(CP_UTF8, 0, env_exe, -1, g_genuine_exe_w, MAX_PATH);
    }

    if ((!g_genuine_dll_w[0] || !g_genuine_exe_w[0]) &&
        find_bambu_studio(genuine_exe, genuine_dir)) {
        if (!g_genuine_exe_w[0])
            wcscpy_s(g_genuine_exe_w, MAX_PATH, genuine_exe);

        if (!g_genuine_dll_w[0]) {
            wcscpy_s(g_genuine_dll_w, MAX_PATH, genuine_dir);
            wcscat_s(g_genuine_dll_w, MAX_PATH, L"BambuStudio.dll");
        }
    }

    if (!g_genuine_dll_w[0]) {
        wcscpy_s(g_genuine_dll_w, MAX_PATH,
                 L"C:\\Program Files\\Bambu Studio\\BambuStudio.dll");
    }
    if (!g_genuine_exe_w[0]) {
        wcscpy_s(g_genuine_exe_w, MAX_PATH,
                 L"C:\\Program Files\\Bambu Studio\\bambu-studio.exe");
    }

    if (::GetFileAttributesW(g_genuine_dll_w) == INVALID_FILE_ATTRIBUTES)
        g_genuine_dll_w[0] = 0;
    if (::GetFileAttributesW(g_genuine_exe_w) == INVALID_FILE_ATTRIBUTES)
        g_genuine_exe_w[0] = 0;

    if (g_genuine_dll_w[0])
        ::WideCharToMultiByte(CP_ACP, 0, g_genuine_dll_w, -1,
                              g_genuine_dll_a, MAX_PATH, nullptr, nullptr);
    if (g_genuine_exe_w[0])
        ::WideCharToMultiByte(CP_ACP, 0, g_genuine_exe_w, -1,
                              g_genuine_exe_a, MAX_PATH, nullptr, nullptr);

    return g_genuine_dll_w[0] || g_genuine_exe_w[0];
}

bool caller_is_bambu_plugin(void* return_address)
{
    if (!return_address || !o_gmfw)
        return false;

    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(return_address),
            &module) ||
        !module)
        return false;

    wchar_t path[MAX_PATH] = {0};
    o_gmfw(module, path, MAX_PATH);

    return wcsstr(path, L"bambu_networking") != nullptr ||
           wcsstr(path, L"BambuSource") != nullptr;
}

const wchar_t* redirect_w(const wchar_t* path)
{
    if (!path)
        return nullptr;

    if (g_our_dll[0] && g_genuine_dll_w[0] &&
        _wcsicmp(path, g_our_dll) == 0)
        return g_genuine_dll_w;

    if (g_our_exe[0] && g_genuine_exe_w[0] &&
        _wcsicmp(path, g_our_exe) == 0)
        return g_genuine_exe_w;

    return nullptr;
}

const char* redirect_a(const char* path)
{
    if (!path)
        return nullptr;

    wchar_t wide[MAX_PATH] = {0};
    ::MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH);

    if (g_our_dll[0] && g_genuine_dll_a[0] &&
        _wcsicmp(wide, g_our_dll) == 0)
        return g_genuine_dll_a;

    if (g_our_exe[0] && g_genuine_exe_a[0] &&
        _wcsicmp(wide, g_our_exe) == 0)
        return g_genuine_exe_a;

    return nullptr;
}

DWORD WINAPI hook_get_module_filename_w(HMODULE module, LPWSTR filename, DWORD size)
{
    void* return_address = _ReturnAddress();
    const DWORD result = o_gmfw(module, filename, size);

    if (filename && result && caller_is_bambu_plugin(return_address)) {
        if (const wchar_t* target = redirect_w(filename)) {
            const size_t len = wcslen(target);
            if (size > len) {
                wcscpy_s(filename, size, target);
                return static_cast<DWORD>(len);
            }
        }
    }

    return result;
}

DWORD WINAPI hook_get_module_filename_a(HMODULE module, LPSTR filename, DWORD size)
{
    void* return_address = _ReturnAddress();
    const DWORD result = o_gmfa(module, filename, size);

    if (filename && result && caller_is_bambu_plugin(return_address)) {
        if (const char* target = redirect_a(filename)) {
            const size_t len = strlen(target);
            if (size > len) {
                strcpy_s(filename, size, target);
                return static_cast<DWORD>(len);
            }
        }
    }

    return result;
}

BOOL WINAPI hook_crypt_query_object(
    DWORD object_type,
    const void* object,
    DWORD expected_content_flags,
    DWORD expected_format_flags,
    DWORD flags,
    DWORD* message_and_cert_encoding_type,
    DWORD* content_type,
    DWORD* format_type,
    HCERTSTORE* cert_store,
    HCRYPTMSG* message,
    const void** context)
{
    const void* use_object = object;

    if (object_type == CERT_QUERY_OBJECT_FILE &&
        object &&
        caller_is_bambu_plugin(_ReturnAddress())) {
        if (const wchar_t* target =
                redirect_w(reinterpret_cast<const wchar_t*>(object)))
            use_object = target;
    }

    return o_cqo(object_type, use_object,
                 expected_content_flags, expected_format_flags, flags,
                 message_and_cert_encoding_type, content_type, format_type,
                 cert_store, message, context);
}

} // namespace

namespace Slic3r {

void install_plugin_verify_redirect()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&install_plugin_verify_redirect),
        &self);

    ::GetModuleFileNameW(self, g_our_dll, MAX_PATH);
    ::GetModuleFileNameW(nullptr, g_our_exe, MAX_PATH);

    if (!resolve_genuine_files())
        return;

    const MH_STATUS init = ::MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return;

    HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    HMODULE crypt32 = ::GetModuleHandleW(L"crypt32.dll");
    if (!crypt32)
        crypt32 = ::LoadLibraryW(L"crypt32.dll");

    if (kernel32) {
        ::MH_CreateHook(
            reinterpret_cast<void*>(::GetProcAddress(kernel32, "GetModuleFileNameW")),
            reinterpret_cast<void*>(hook_get_module_filename_w),
            reinterpret_cast<void**>(&o_gmfw));
        ::MH_CreateHook(
            reinterpret_cast<void*>(::GetProcAddress(kernel32, "GetModuleFileNameA")),
            reinterpret_cast<void*>(hook_get_module_filename_a),
            reinterpret_cast<void**>(&o_gmfa));
    }

    if (crypt32) {
        ::MH_CreateHook(
            reinterpret_cast<void*>(::GetProcAddress(crypt32, "CryptQueryObject")),
            reinterpret_cast<void*>(hook_crypt_query_object),
            reinterpret_cast<void**>(&o_cqo));
    }

    ::MH_EnableHook(MH_ALL_HOOKS);
}

} // namespace Slic3r

#else

namespace Slic3r {
void install_plugin_verify_redirect() {}
}

#endif
