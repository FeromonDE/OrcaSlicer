// Why?
#define _WIN32_WINNT 0x0601
// The standard Windows includes.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <wchar.h>



#ifdef SLIC3R_GUI
extern "C"
{
    // Let the NVIDIA and AMD know we want to use their graphics card
    // on a dual graphics card system.
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif /* SLIC3R_GUI */

#include <stdlib.h>
#include <stdio.h>

#ifdef SLIC3R_GUI
    #include <GL/GL.h>
#endif /* SLIC3R_GUI */

#include <string>
#include <vector>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>

#include <stdio.h>

#ifdef SLIC3R_GUI
class OpenGLVersionCheck
{
public:
    std::string version;
    std::string glsl_version;
    std::string vendor;
    std::string renderer;

    HINSTANCE   hOpenGL = nullptr;
    bool 		success = false;

    bool load_opengl_dll()
    {
        MSG      msg     = {0};
        WNDCLASS wc      = {0};
        wc.lpfnWndProc   = OpenGLVersionCheck::supports_opengl2_wndproc;
        wc.hInstance     = (HINSTANCE)GetModuleHandle(nullptr);
        wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND);
        wc.lpszClassName = L"OrcaSlicer_opengl_version_check";
        wc.style = CS_OWNDC;
        if (RegisterClass(&wc)) {
            HWND hwnd = CreateWindowW(wc.lpszClassName, L"OrcaSlicer_opengl_version_check", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, 0, 0, wc.hInstance, (LPVOID)this);
            if (hwnd) {
                message_pump_exit = false;
                while (GetMessage(&msg, NULL, 0, 0 ) > 0 && ! message_pump_exit)
                    DispatchMessage(&msg);
            }
        }
        return this->success;
    }

    void unload_opengl_dll()
    {
        if (this->hOpenGL) {
            BOOL released = FreeLibrary(this->hOpenGL);
            if (released)
                printf("System OpenGL library released\n");
            else
                printf("System OpenGL library NOT released\n");
            this->hOpenGL = nullptr;
        }
    }

    bool is_version_greater_or_equal_to(unsigned int major, unsigned int minor) const
    {
        // printf("is_version_greater_or_equal_to, version: %s\n", version.c_str());
        std::vector<std::string> tokens;
        boost::split(tokens, version, boost::is_any_of(" "), boost::token_compress_on);
        if (tokens.empty())
            return false;

        std::vector<std::string> numbers;
        boost::split(numbers, tokens[0], boost::is_any_of("."), boost::token_compress_on);

        unsigned int gl_major = 0;
        unsigned int gl_minor = 0;
        if (numbers.size() > 0)
            gl_major = ::atoi(numbers[0].c_str());
        if (numbers.size() > 1)
            gl_minor = ::atoi(numbers[1].c_str());
        // printf("Major: %d, minor: %d\n", gl_major, gl_minor);
        if (gl_major < major)
            return false;
        else if (gl_major > major)
            return true;
        else
            return gl_minor >= minor;
    }

protected:
    static bool message_pump_exit;

    void check(HWND hWnd)
    {
        hOpenGL = LoadLibraryExW(L"opengl32.dll", nullptr, 0);
        if (hOpenGL == nullptr) {
            printf("Failed loading the system opengl32.dll\n");
            return;
        }

        typedef HGLRC 		(WINAPI *Func_wglCreateContext)(HDC);
        typedef BOOL 		(WINAPI *Func_wglMakeCurrent  )(HDC, HGLRC);
        typedef BOOL     	(WINAPI *Func_wglDeleteContext)(HGLRC);
        typedef GLubyte* 	(WINAPI *Func_glGetString     )(GLenum);

        Func_wglCreateContext 	wglCreateContext = (Func_wglCreateContext)GetProcAddress(hOpenGL, "wglCreateContext");
        Func_wglMakeCurrent 	wglMakeCurrent 	 = (Func_wglMakeCurrent)  GetProcAddress(hOpenGL, "wglMakeCurrent");
        Func_wglDeleteContext 	wglDeleteContext = (Func_wglDeleteContext)GetProcAddress(hOpenGL, "wglDeleteContext");
        Func_glGetString 		glGetString 	 = (Func_glGetString)	  GetProcAddress(hOpenGL, "glGetString");

        if (wglCreateContext == nullptr || wglMakeCurrent == nullptr || wglDeleteContext == nullptr || glGetString == nullptr) {
            printf("Failed loading the system opengl32.dll: The library is invalid.\n");
            return;
        }

        PIXELFORMATDESCRIPTOR pfd =
        {
            sizeof(PIXELFORMATDESCRIPTOR),
            1,
            PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
            PFD_TYPE_RGBA,            	// The kind of framebuffer. RGBA or palette.
            32,                        	// Color depth of the framebuffer.
            0, 0, 0, 0, 0, 0,
            0,
            0,
            0,
            0, 0, 0, 0,
            24,                        	// Number of bits for the depthbuffer
            8,                        	// Number of bits for the stencilbuffer
            0,                        	// Number of Aux buffers in the framebuffer.
            PFD_MAIN_PLANE,
            0,
            0, 0, 0
        };

        HDC ourWindowHandleToDeviceContext = ::GetDC(hWnd);
        // Gdi32.dll
        int letWindowsChooseThisPixelFormat = ::ChoosePixelFormat(ourWindowHandleToDeviceContext, &pfd);
        // Gdi32.dll
        SetPixelFormat(ourWindowHandleToDeviceContext, letWindowsChooseThisPixelFormat, &pfd);
        // Opengl32.dll
        HGLRC glcontext = wglCreateContext(ourWindowHandleToDeviceContext);
        wglMakeCurrent(ourWindowHandleToDeviceContext, glcontext);
        // Opengl32.dll
        const char *data = (const char*)glGetString(GL_VERSION);
        if (data != nullptr)
            this->version = data;
        // printf("check -version: %s\n", version.c_str());
        data = (const char*)glGetString(0x8B8C); // GL_SHADING_LANGUAGE_VERSION
        if (data != nullptr)
            this->glsl_version = data;
        data = (const char*)glGetString(GL_VENDOR);
        if (data != nullptr)
            this->vendor = data;
        data = (const char*)glGetString(GL_RENDERER);
        if (data != nullptr)
            this->renderer = data;
        // Opengl32.dll
        wglDeleteContext(glcontext);
        ::ReleaseDC(hWnd, ourWindowHandleToDeviceContext);
        this->success = true;
    }

    static LRESULT CALLBACK supports_opengl2_wndproc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch(message)
        {
        case WM_CREATE:
        {
            CREATESTRUCT *pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            OpenGLVersionCheck *ogl_data = reinterpret_cast<OpenGLVersionCheck*>(pCreate->lpCreateParams);
            ogl_data->check(hWnd);
            DestroyWindow(hWnd);
            return 0;
        }
        case WM_NCDESTROY:
            message_pump_exit = true;
            return 0;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
};

bool OpenGLVersionCheck::message_pump_exit = false;
#endif /* SLIC3R_GUI */

extern "C" {
    typedef int (__stdcall *Slic3rMainFunc)(int argc, wchar_t **argv);
    Slic3rMainFunc orcaslicer_main = nullptr;
}


enum class BambuHostStageResult {
    Ready,
    NoBambuStudio,
    NeedsElevation,
    Failed
};

static bool find_bambu_studio(wchar_t* exe, wchar_t* dir)
{
    exe[0] = 0;
    dir[0] = 0;

    HKEY key = nullptr;
    if (::RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Bambu Studio",
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &key) != ERROR_SUCCESS)
        return false;

    wchar_t icon[MAX_PATH + 1] = {0};
    DWORD bytes = sizeof(icon) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG rc = ::RegQueryValueExW(
        key, L"DisplayIcon", nullptr, &type,
        reinterpret_cast<LPBYTE>(icon), &bytes);
    ::RegCloseKey(key);

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

static bool same_file_identity(const wchar_t* a, const wchar_t* b)
{
    WIN32_FILE_ATTRIBUTE_DATA aa = {};
    WIN32_FILE_ATTRIBUTE_DATA bb = {};
    if (!::GetFileAttributesExW(a, GetFileExInfoStandard, &aa) ||
        !::GetFileAttributesExW(b, GetFileExInfoStandard, &bb))
        return false;

    return aa.nFileSizeHigh == bb.nFileSizeHigh &&
           aa.nFileSizeLow == bb.nFileSizeLow &&
           aa.ftLastWriteTime.dwHighDateTime == bb.ftLastWriteTime.dwHighDateTime &&
           aa.ftLastWriteTime.dwLowDateTime == bb.ftLastWriteTime.dwLowDateTime;
}

static BambuHostStageResult stage_current_bambu_host(const wchar_t* orca_dir)
{
    wchar_t source_exe[MAX_PATH + 1] = {0};
    wchar_t source_dir[MAX_PATH + 1] = {0};
    if (!find_bambu_studio(source_exe, source_dir))
        return BambuHostStageResult::NoBambuStudio;

    wchar_t staged_exe[MAX_PATH + 1] = {0};
    wcscpy_s(staged_exe, MAX_PATH, orca_dir);
    wcscat_s(staged_exe, MAX_PATH, L"bambu-studio.exe");

    if (same_file_identity(source_exe, staged_exe))
        return BambuHostStageResult::Ready;

    if (::CopyFileW(source_exe, staged_exe, FALSE))
        return BambuHostStageResult::Ready;

    const DWORD error = ::GetLastError();
    if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
        return BambuHostStageResult::NeedsElevation;

    return BambuHostStageResult::Failed;
}

static bool elevate_host_stage(const wchar_t* self_exe)
{
    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = self_exe;
    info.lpParameters = L"--stage-bambu-host";
    info.nShow = SW_HIDE;

    if (!::ShellExecuteExW(&info) || !info.hProcess)
        return false;

    ::WaitForSingleObject(info.hProcess, 120000);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(info.hProcess, &exit_code);
    ::CloseHandle(info.hProcess);
    return exit_code == 0;
}

static bool launch_bambu_host(const wchar_t* orca_dir, int argc, wchar_t** argv)
{
    wchar_t host_exe[MAX_PATH + 1] = {0};
    wcscpy_s(host_exe, MAX_PATH, orca_dir);
    wcscat_s(host_exe, MAX_PATH, L"bambu-studio.exe");
    if (::GetFileAttributesW(host_exe) == INVALID_FILE_ATTRIBUTES)
        return false;

    std::wstring command = L"\"";
    command += host_exe;
    command += L"\"";
    for (int i = 1; i < argc; ++i) {
        command += L" \"";
        command += argv[i];
        command += L"\"";
    }
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');

    PROCESS_INFORMATION process = {};
    BOOL launched = FALSE;

    DWORD shell_pid = 0;
    ::GetWindowThreadProcessId(::GetShellWindow(), &shell_pid);
    HANDLE shell_process = shell_pid
        ? ::OpenProcess(PROCESS_CREATE_PROCESS, FALSE, shell_pid)
        : nullptr;

    if (shell_process) {
        STARTUPINFOEXW startup = {};
        startup.StartupInfo.cb = sizeof(startup);

        SIZE_T attribute_size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        startup.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                ::HeapAlloc(::GetProcessHeap(), 0, attribute_size));

        if (startup.lpAttributeList &&
            ::InitializeProcThreadAttributeList(
                startup.lpAttributeList, 1, 0, &attribute_size) &&
            ::UpdateProcThreadAttribute(
                startup.lpAttributeList,
                0,
                PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                &shell_process,
                sizeof(shell_process),
                nullptr,
                nullptr)) {
            launched = ::CreateProcessW(
                host_exe,
                command_buffer.data(),
                nullptr,
                nullptr,
                FALSE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                orca_dir,
                &startup.StartupInfo,
                &process);
        }

        if (startup.lpAttributeList) {
            ::DeleteProcThreadAttributeList(startup.lpAttributeList);
            ::HeapFree(::GetProcessHeap(), 0, startup.lpAttributeList);
        }
        ::CloseHandle(shell_process);
    }

    if (!launched) {
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        launched = ::CreateProcessW(
            host_exe,
            command_buffer.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            orca_dir,
            &startup,
            &process);
    }

    if (!launched)
        return false;

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

extern "C" {
#ifdef SLIC3R_WRAPPER_NOCONSOLE
int APIENTRY wWinMain(HINSTANCE /* hInstance */, HINSTANCE /* hPrevInstance */, PWSTR /* lpCmdLine */, int /* nCmdShow */)
{
    int 	  argc;
    wchar_t **argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
#else
int wmain(int argc, wchar_t **argv)
{
#endif
    // Allow the asserts to open message box, such message box allows to ignore the assert and continue with the application.
    // Without this call, the seemingly same message box is being opened by the abort() function, but that is too late and
    // the application will be killed even if "Ignore" button is pressed.
    _set_error_mode(_OUT_TO_MSGBOX);

    std::vector<wchar_t*> argv_extended;
    argv_extended.emplace_back(argv[0]);

#ifdef SLIC3R_WRAPPER_GCODEVIEWER
    wchar_t gcodeviewer_param[] = L"--gcodeviewer";
    argv_extended.emplace_back(gcodeviewer_param);
#endif /* SLIC3R_WRAPPER_GCODEVIEWER */

#ifdef SLIC3R_GUI
    // Here one may push some additional parameters based on the wrapper type.
    bool force_mesa = false;
#endif /* SLIC3R_GUI */
    bool stage_bambu_host_only = false;
    for (int i = 1; i < argc; ++ i) {
        if (wcscmp(argv[i], L"--stage-bambu-host") == 0) {
            stage_bambu_host_only = true;
            continue;
        }
#ifdef SLIC3R_GUI
        if (wcscmp(argv[i], L"--sw-renderer") == 0)
            force_mesa = true;
        else if (wcscmp(argv[i], L"--no-sw-renderer") == 0)
            force_mesa = false;
#endif /* SLIC3R_GUI */
        argv_extended.emplace_back(argv[i]);
    }
    argv_extended.emplace_back(nullptr);

#ifdef SLIC3R_GUI
    OpenGLVersionCheck opengl_version_check;
    bool load_mesa = false;
    if (!stage_bambu_host_only) {
        load_mesa =
            // Forced from the command line.
            force_mesa ||
            // Try to load the default OpenGL driver and test its context version.
            ! opengl_version_check.load_opengl_dll() || ! opengl_version_check.is_version_greater_or_equal_to(2, 0);
    }
#endif /* SLIC3R_GUI */

    wchar_t path_to_exe[MAX_PATH + 1] = { 0 };
    ::GetModuleFileNameW(nullptr, path_to_exe, MAX_PATH);
    wchar_t self_exe[MAX_PATH + 1] = { 0 };
    wcscpy_s(self_exe, MAX_PATH, path_to_exe);

    wchar_t drive[_MAX_DRIVE];
    wchar_t dir[_MAX_DIR];
    wchar_t fname[_MAX_FNAME];
    wchar_t ext[_MAX_EXT];
    _wsplitpath(path_to_exe, drive, dir, fname, ext);
    _wmakepath(path_to_exe, drive, dir, nullptr, nullptr);

#if defined(_M_X64) || defined(__x86_64__)
    if (stage_bambu_host_only) {
        const BambuHostStageResult staged = stage_current_bambu_host(path_to_exe);
        return staged == BambuHostStageResult::Ready ? 0 : 1;
    }

    // Direct Orca launch: refresh the staged host from the currently installed
    // Bambu Studio whenever its signed executable changes, then relaunch under it.
    // If Bambu Studio is absent, continue in normal Orca mode.
    if (_wcsicmp(fname, L"bambu-studio") != 0) {
        BambuHostStageResult staged = stage_current_bambu_host(path_to_exe);

        if (staged == BambuHostStageResult::NeedsElevation &&
            elevate_host_stage(self_exe)) {
            staged = stage_current_bambu_host(path_to_exe);
        }

        if (staged == BambuHostStageResult::Ready &&
            launch_bambu_host(path_to_exe, argc, argv)) {
            return 0;
        }
    }
#else
    if (stage_bambu_host_only)
        return 1;
#endif

    wchar_t path_to_python[MAX_PATH + 1] = { 0 };
    wcscpy(path_to_python, path_to_exe);
    wcscat(path_to_python, L"python");
    DWORD python_attrs = GetFileAttributesW(path_to_python);
    if (python_attrs != INVALID_FILE_ATTRIBUTES && (python_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        SetDllDirectoryW(path_to_python);
    }

#ifdef SLIC3R_GUI
// https://wiki.qt.io/Cross_compiling_Mesa_for_Windows
// http://download.qt.io/development_releases/prebuilt/llvmpipe/windows/
    if (load_mesa) {
        opengl_version_check.unload_opengl_dll();
        wchar_t path_to_mesa[MAX_PATH + 1] = { 0 };
        wcscpy(path_to_mesa, path_to_exe);
        wcscat(path_to_mesa, L"mesa\\opengl32.dll");
        printf("Loading MESA OpenGL library: %S\n", path_to_mesa);
        HINSTANCE hInstance_OpenGL = LoadLibraryExW(path_to_mesa, nullptr, 0);
        if (hInstance_OpenGL == nullptr) {
            printf("MESA OpenGL library was not loaded\n");
        } else
            printf("MESA OpenGL library was loaded successfully\n");
    }
#endif /* SLIC3R_GUI */


    wchar_t path_to_slic3r[MAX_PATH + 1] = { 0 };
    wcscpy(path_to_slic3r, path_to_exe);
    wcscat(path_to_slic3r, L"BambuStudio.dll");
//	printf("Loading Slic3r library: %S\n", path_to_slic3r);
    HINSTANCE hInstance_Slic3r = LoadLibraryExW(path_to_slic3r, nullptr, 0);
    if (hInstance_Slic3r == nullptr) {
        printf("BambuStudio.dll was not loaded, error=%lu\n", GetLastError());
        return -1;
    }

    // resolve function address here
    orcaslicer_main = (Slic3rMainFunc)GetProcAddress(hInstance_Slic3r,
#ifdef _WIN64
        // there is just a single calling conversion, therefore no mangling of the function name.
        "orcaslicer_main"
#else	// stdcall calling convention declaration
        "_bambustu_main@8"
#endif
        );
    if (orcaslicer_main == nullptr) {
        printf("could not locate the function orcaslicer_main in BambuStudio.dll\n");
        return -1;
    }
    // argc minus the trailing nullptr of the argv
    return orcaslicer_main((int)argv_extended.size() - 1, argv_extended.data());
}
}
