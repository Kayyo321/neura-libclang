/*
 * libclang_loader.c: find and open libclang, then resolve the function table
 * declared in libclang_loader.h.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dladdr */
#endif
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, strncpy */
#endif

#include "libclang_loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
/* RegGetValueW. Named here so that a program linking the library by its
   file gets it too. */
#pragma comment(lib, "advapi32.lib")
#else
#include <dlfcn.h>
#endif

/* Function pointers are copied out of the loader's object pointers. */
typedef char neura_libclang_pointer_sizes_match
    [sizeof(void *) == sizeof(void (*)(void)) ? 1 : -1];

/*
 * Check every table entry against the declaration in the pinned headers.
 * The expressions are operands of sizeof, so they are never evaluated and
 * the program does not reference libclang's symbols.
 */
static void check_table_signatures(NeuraLibclang *lib) {
#define NEURA_LIBCLANG_CHECK(result, name, parameters) (void)sizeof(lib->name = &name);
    NEURA_LIBCLANG_FUNCTIONS(NEURA_LIBCLANG_CHECK)
#undef NEURA_LIBCLANG_CHECK
    (void)lib;
}

static void copy_text(char *destination, size_t size, const char *source) {
    if (size == 0)
        return;
    strncpy(destination, source ? source : "", size - 1);
    destination[size - 1] = '\0';
}

static void append_text(char *destination, size_t size, const char *format, ...) {
    size_t used = strlen(destination);
    va_list arguments;
    if (used + 1 >= size)
        return;
    va_start(arguments, format);
    vsnprintf(destination + used, size - used, format, arguments);
    va_end(arguments);
}

/* ---- Windows text and files --------------------------------------------- */

#ifdef _WIN32
/*
 * Names cross the Windows API as UTF-16. The library's interface is UTF-8, and
 * conversions are strict: text that is not valid UTF-8, or a Windows name
 * with an unpaired surrogate, is an error rather than a replacement
 * character.
 */

/* `text` as UTF-16, or NULL when it is not valid UTF-8. Free with free(). */
static wchar_t *to_wide(const char *text) {
    wchar_t *wide;
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (length <= 0)
        return NULL;
    wide = (wchar_t *)malloc((size_t)length * sizeof *wide);
    if (wide && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, length) != length) {
        free(wide);
        wide = NULL;
    }
    return wide;
}

/* Writes `text` as UTF-8 to `destination`. Returns 0, leaving the empty
   string, when it holds an unpaired surrogate or does not fit. */
static int to_narrow(char *destination, size_t size, const wchar_t *text) {
    int length;
    if (size == 0)
        return 0;
    destination[0] = '\0';
    if (size > 0x7fffffff)
        size = 0x7fffffff;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, destination, (int)size, NULL, NULL);
    if (length <= 0) {
        destination[0] = '\0';
        return 0;
    }
    return 1;
}

/* Whether a file, not a directory, exists at the UTF-16 path. */
static int wide_file_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* Whether a UTF-16 directory names a fixed location: a drive root
   (C:\ or C:/) or a UNC path. A relative directory, or a drive-relative one
   such as C:name, would be resolved against the current directory. */
static int wide_absolute(const wchar_t *path) {
    if ((path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'))
        return 1;
    return ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

/* Whether a UTF-8 name is a path rather than a bare name: it holds a
   separator or begins with a drive. */
static int is_path_name(const char *name) {
    return strchr(name, '/') != NULL || strchr(name, '\\') != NULL ||
           (((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')) && name[1] == ':');
}

/* "<name>: error <code>: <the system's message>". */
static void describe_windows_error(char *error, size_t error_size, const char *name, DWORD code) {
    wchar_t message[512];
    char text[1024];
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                                  message, (DWORD)(sizeof message / sizeof message[0]), NULL);
    /* The message ends with a period and a line break, which would split the
       sentence it is embedded in. */
    while (length > 0 && (message[length - 1] == L'\r' || message[length - 1] == L'\n' ||
                          message[length - 1] == L' ' || message[length - 1] == L'.'))
        --length;
    /* FormatMessageW stores at most nSize - 1 characters. */
    message[length] = L'\0';
    if (length > 0 && to_narrow(text, sizeof text, message))
        snprintf(error, error_size, "%s: error %lu: %s", name, (unsigned long)code, text);
    else
        snprintf(error, error_size, "%s: error %lu", name, (unsigned long)code);
}

/* LoadLibraryExW without the system's error dialog: a file that is not a
   valid image, or one whose dependency is missing, fails with its error code
   instead of waiting for someone to close a message box. */
static HMODULE load_quietly(const wchar_t *name, DWORD flags) {
    DWORD previous = 0;
    BOOL changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous);
    HMODULE module = LoadLibraryExW(name, NULL, flags);
    DWORD error = GetLastError();
    if (changed)
        SetThreadErrorMode(previous, NULL);
    SetLastError(error);
    return module;
}

/* Loads a library by its full path. Its own dependencies are found beside it
   (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR), as the Visual Studio libclang.dll needs,
   then in the system directories; never in the current directory. */
static HMODULE load_full_path(const wchar_t *path) {
    return load_quietly(path, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

/* The value of an environment variable, or NULL when it is unset or empty. */
static const wchar_t *environment_value(const wchar_t *name) {
    const wchar_t *value = _wgetenv(name);
    return value && value[0] ? value : NULL;
}

/*
 * Calls `visit` with each directory of PATH that names a fixed location (see
 * wide_absolute), without surrounding quotes, until it returns nonzero.
 * Returns what the last call returned, or 0.
 */
static int each_path_directory(int (*visit)(const wchar_t *directory, void *data), void *data) {
    const wchar_t *path = environment_value(L"PATH");
    wchar_t directory[1024];
    while (path && *path) {
        const wchar_t *end = wcschr(path, L';');
        size_t length = end ? (size_t)(end - path) : wcslen(path);
        const wchar_t *start = path;
        path = end ? end + 1 : NULL;
        if (length >= 2 && start[0] == L'"' && start[length - 1] == L'"') {
            ++start;
            length -= 2;
        }
        if (length == 0 || length >= sizeof directory / sizeof directory[0])
            continue;
        memcpy(directory, start, length * sizeof *directory);
        directory[length] = L'\0';
        if (wide_absolute(directory) && visit(directory, data))
            return 1;
    }
    return 0;
}

/* `directory`\`name` as UTF-16 in `out`; 0 when it does not fit. */
static int join_wide(wchar_t *out, size_t size, const wchar_t *directory, const wchar_t *name) {
    size_t length = wcslen(directory);
    int separator = length > 0 && directory[length - 1] != L'\\' && directory[length - 1] != L'/';
    int written = swprintf(out, size, separator ? L"%ls\\%ls" : L"%ls%ls", directory, name);
    return written > 0 && (size_t)written < size;
}

typedef struct PathLoad {
    const wchar_t *name;
    HMODULE module;
    /* The error of the first file found that would not load, or 0. */
    DWORD error;
} PathLoad;

static int load_from_directory(const wchar_t *directory, void *data) {
    PathLoad *load = (PathLoad *)data;
    wchar_t candidate[1024];
    if (!join_wide(candidate, sizeof candidate / sizeof candidate[0], directory, load->name) ||
        !wide_file_exists(candidate))
        return 0;
    load->module = load_full_path(candidate);
    if (!load->module && load->error == 0)
        load->error = GetLastError();
    return load->module != NULL;
}
#endif

/* ---- Platform loader ---------------------------------------------------- */

/*
 * Windows: a path (text with '/' or '\\', or a drive) loads that file, made
 * absolute against the current directory when it is relative. A bare name is
 * looked for in the application's directory and the system directories, then
 * in each directory of PATH by its full path; never in the current directory.
 */
static void *open_library(const char *name, char *error, size_t error_size) {
#ifdef _WIN32
    HMODULE module = NULL;
    DWORD code = ERROR_MOD_NOT_FOUND;
    wchar_t *wide = to_wide(name);
    if (!wide) {
        snprintf(error, error_size, "%s: the name is not valid UTF-8", name);
        return NULL;
    }
    if (is_path_name(name)) {
        DWORD size = GetFullPathNameW(wide, 0, NULL, NULL);
        wchar_t *full = size > 0 ? (wchar_t *)malloc((size_t)size * sizeof *full) : NULL;
        if (!full) {
            code = size > 0 ? ERROR_NOT_ENOUGH_MEMORY : GetLastError();
        } else {
            DWORD length = GetFullPathNameW(wide, size, full, NULL);
            if (length == 0 || length >= size) {
                code = length == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE;
            } else {
                module = load_full_path(full);
                if (!module)
                    code = GetLastError();
            }
            free(full);
        }
    } else {
        module = load_quietly(wide, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            PathLoad load;
            code = GetLastError();
            load.name = wide;
            load.module = NULL;
            load.error = 0;
            if (each_path_directory(load_from_directory, &load))
                module = load.module;
            else if (load.error != 0)
                code = load.error;
        }
    }
    free(wide);
    if (!module)
        describe_windows_error(error, error_size, name, code);
    return (void *)module;
#else
    void *handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *message = dlerror();
        copy_text(error, error_size, message ? message : name);
    }
    return handle;
#endif
}

/*
 * Windows keeps the library loaded until the process exits: once libclang.dll
 * has been unloaded with FreeLibrary, the process fails at exit (0xC0000005
 * with libclang 19), so closing only forgets the handle.
 */
static void close_library(void *handle) {
    if (!handle)
        return;
#ifndef _WIN32
    dlclose(handle);
#endif
}

static void *find_symbol(void *handle, const char *name) {
#ifdef _WIN32
    FARPROC address = GetProcAddress((HMODULE)handle, name);
    void *result;
    memcpy(&result, &address, sizeof result);
    return result;
#else
    return dlsym(handle, name);
#endif
}

/* Record the file the library came from; fall back to the requested name. */
static void find_library_path(NeuraLibclang *lib, void *symbol) {
#ifdef _WIN32
    wchar_t path[1024];
    DWORD length;
    (void)symbol;
    length = GetModuleFileNameW((HMODULE)lib->handle, path, (DWORD)(sizeof path / sizeof path[0]));
    if (length == 0 || length >= sizeof path / sizeof path[0] || !to_narrow(lib->path, sizeof lib->path, path))
        copy_text(lib->path, sizeof lib->path, lib->requested);
#else
    Dl_info info;
    if (dladdr(symbol, &info) && info.dli_fname && info.dli_fname[0])
        copy_text(lib->path, sizeof lib->path, info.dli_fname);
    else
        copy_text(lib->path, sizeof lib->path, lib->requested);
#endif
}

/* ---- Loading one candidate ---------------------------------------------- */

static void clear_table(NeuraLibclang *lib) {
#define NEURA_LIBCLANG_CLEAR(result, name, parameters) lib->name = NULL;
    NEURA_LIBCLANG_FUNCTIONS(NEURA_LIBCLANG_CLEAR)
#undef NEURA_LIBCLANG_CLEAR
}

/* The first run of digits after "version " in a clang version string. */
static int parse_major(const char *version) {
    const char *at = strstr(version, "version ");
    int major = 0;
    if (!at)
        return 0;
    at += strlen("version ");
    while (*at >= '0' && *at <= '9' && major < 100000) {
        major = major * 10 + (*at - '0');
        ++at;
    }
    return major;
}

enum candidate_result { CANDIDATE_LOADED, CANDIDATE_MISSING, CANDIDATE_UNUSABLE };

/*
 * Try one library. CANDIDATE_MISSING means the platform loader could not
 * open it; CANDIDATE_UNUSABLE means it opened but lacks a function. Either
 * way `reason` explains why.
 */
static enum candidate_result try_candidate(NeuraLibclang *lib, const char *name, char *reason,
                                           size_t reason_size) {
    void *handle;
    void *symbol = NULL;
    reason[0] = '\0';
    handle = open_library(name, reason, reason_size);
    if (!handle)
        return CANDIDATE_MISSING;

#define NEURA_LIBCLANG_RESOLVE(result, name_, parameters) \
    symbol = find_symbol(handle, #name_); \
    if (!symbol) { \
        snprintf(reason, reason_size, "%s: does not export %s", name, #name_); \
        clear_table(lib); \
        close_library(handle); \
        return CANDIDATE_UNUSABLE; \
    } \
    memcpy(&lib->name_, &symbol, sizeof symbol);
    NEURA_LIBCLANG_FUNCTIONS(NEURA_LIBCLANG_RESOLVE)
#undef NEURA_LIBCLANG_RESOLVE

    lib->handle = handle;
    copy_text(lib->requested, sizeof lib->requested, name);
    find_library_path(lib, symbol);
    {
        CXString version = lib->clang_getClangVersion();
        copy_text(lib->version, sizeof lib->version, lib->clang_getCString(version));
        lib->clang_disposeString(version);
    }
    lib->major = parse_major(lib->version);
    return CANDIDATE_LOADED;
}

/* ---- Search ------------------------------------------------------------- */

typedef struct Search {
    NeuraLibclang *lib;
    int tried;
    /* Candidates that exist but could not be used. */
    char unusable[2048];
    /* The platform loader's message for the first candidate it could not open. */
    char first_missing[512];
#ifdef _WIN32
    /* The candidates tried so far, so that a directory reached two ways (the
       LLVM directory and its registry entry, a PATH entry) is tried once. */
    char seen[16][1024];
    int seen_count;
#endif
} Search;

/* Whether `name` is a path to a file that exists, rather than a bare name. */
static int existing_file(const char *name) {
#ifdef _WIN32
    wchar_t *wide;
    int exists;
    if (!is_path_name(name) || !(wide = to_wide(name)))
        return 0;
    exists = wide_file_exists(wide);
    free(wide);
    return exists;
#else
    FILE *file;
    int is_path = strchr(name, '/') != NULL || strchr(name, '\\') != NULL;
    if (!is_path || !(file = fopen(name, "rb")))
        return 0;
    fclose(file);
    return 1;
#endif
}

/* Returns 1 when `name` loaded. */
static int search_try(Search *search, const char *name) {
    char reason[1024];
    enum candidate_result result = try_candidate(search->lib, name, reason, sizeof reason);
    ++search->tried;
    if (result == CANDIDATE_LOADED)
        return 1;
    /* A library that exists but will not open is as useful to report as one
       that lacks functions. */
    if (result == CANDIDATE_UNUSABLE || existing_file(name))
        append_text(search->unusable, sizeof search->unusable, "\n  %s", reason);
    else if (!search->first_missing[0])
        copy_text(search->first_missing, sizeof search->first_missing, reason);
    return 0;
}

#if defined(_WIN32)
/*
 * Tries libclang.dll in `directory` when the file exists and was not tried
 * before. Only existing files are tried, so the search reports the libraries
 * it found rather than every place it looked.
 */
static int search_try_directory(Search *search, const wchar_t *directory) {
    wchar_t candidate[1024];
    char name[1024];
    int i;
    if (!wide_absolute(directory) ||
        !join_wide(candidate, sizeof candidate / sizeof candidate[0], directory, L"libclang.dll") ||
        !wide_file_exists(candidate) || !to_narrow(name, sizeof name, candidate))
        return 0;
    for (i = 0; i < search->seen_count; ++i)
        if (_stricmp(search->seen[i], name) == 0)
            return 0;
    if (search->seen_count < (int)(sizeof search->seen / sizeof search->seen[0]))
        copy_text(search->seen[search->seen_count++], sizeof search->seen[0], name);
    return search_try(search, name);
}

/* `base` followed by `tail`, then tried as a directory. */
static int search_try_below(Search *search, const wchar_t *base, const wchar_t *tail) {
    wchar_t directory[1024];
    return join_wide(directory, sizeof directory / sizeof directory[0], base, tail) &&
           search_try_directory(search, directory);
}

static int search_path_directory(const wchar_t *directory, void *data) {
    return search_try_directory((Search *)data, directory);
}

/*
 * The installation directory of the newest Visual Studio 2017 or later that
 * has the C++ Clang tools, as vswhere reports it: vswhere is installed with
 * Visual Studio at %ProgramFiles(x86)%\Microsoft Visual Studio\Installer. It
 * runs without a window, with only its output pipe and NUL inherited, for at
 * most VSWHERE_TIMEOUT_MS. Returns 0 when vswhere is missing, fails, runs for
 * longer, or names no installation.
 */
#define VSWHERE_TIMEOUT_MS 30000
static int visual_studio_with_clang(wchar_t *installation, size_t size) {
    const wchar_t *root = environment_value(L"ProgramFiles(x86)");
    wchar_t program[1024];
    wchar_t command[1400];
    char output[2048];
    size_t used = 0;
    SECURITY_ATTRIBUTES inheritable;
    HANDLE read_end = NULL, write_end = NULL, null = INVALID_HANDLE_VALUE;
    HANDLE inherited[2];
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    SIZE_T attributes_size = 0;
    DWORD exit_code = 1;
    BOOL started = FALSE;
    int found = 0;

    memset(&startup, 0, sizeof startup);
    memset(&process, 0, sizeof process);
    if (!root || !join_wide(program, sizeof program / sizeof program[0], root,
                            L"Microsoft Visual Studio\\Installer\\vswhere.exe") ||
        !wide_file_exists(program))
        return 0;
    if (swprintf(command, sizeof command / sizeof command[0],
                 L"\"%ls\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang "
                 L"-property installationPath -utf8",
                 program) < 0)
        return 0;

    inheritable.nLength = sizeof inheritable;
    inheritable.lpSecurityDescriptor = NULL;
    inheritable.bInheritHandle = TRUE;
    if (!CreatePipe(&read_end, &write_end, &inheritable, 0))
        return 0;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                       OPEN_EXISTING, 0, NULL);
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    if (null != INVALID_HANDLE_VALUE && attributes_size > 0)
        attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attributes_size);
    if (attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size)) {
        inherited[0] = write_end;
        inherited[1] = null;
        if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited,
                                      NULL, NULL)) {
            startup.StartupInfo.cb = sizeof startup;
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = null;
            startup.StartupInfo.hStdOutput = write_end;
            startup.StartupInfo.hStdError = null;
            startup.lpAttributeList = attributes;
            started = CreateProcessW(program, command, NULL, NULL, TRUE,
                                     EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, NULL, NULL,
                                     &startup.StartupInfo, &process);
        }
        DeleteProcThreadAttributeList(attributes);
    }
    free(attributes);
    /* Only the child holds the write end now. */
    CloseHandle(write_end);
    if (null != INVALID_HANDLE_VALUE)
        CloseHandle(null);
    if (started) {
        /* Drain the pipe while waiting, so that vswhere never blocks on a full
           pipe, and read only what PeekNamedPipe reports, so that no read
           blocks past the deadline: a vswhere that hangs, as during an update
           of the Visual Studio installer, is ended after
           VSWHERE_TIMEOUT_MS and the search goes on without it. The pipe is
           drained once more after the exit; then PeekNamedPipe fails with
           ERROR_BROKEN_PIPE. More output than fits is a failure. */
        const ULONGLONG deadline = GetTickCount64() + VSWHERE_TIMEOUT_MS;
        int exited = 0, failed = 0;
        for (;;) {
            DWORD available = 0;
            ULONGLONG now;
            if (!PeekNamedPipe(read_end, NULL, 0, NULL, &available, NULL))
                available = 0;
            if (available > 0) {
                char chunk[512];
                DWORD count = 0;
                if (!ReadFile(read_end, chunk, available < sizeof chunk ? available : (DWORD)sizeof chunk, &count,
                              NULL)) {
                    failed = 1;
                    break;
                }
                if (used + count >= sizeof output) {
                    failed = 1;
                    break;
                }
                memcpy(output + used, chunk, count);
                used += count;
                continue;
            }
            if (exited)
                break;
            now = GetTickCount64();
            if (now >= deadline) {
                failed = 1;
                break;
            }
            exited = WaitForSingleObject(process.hProcess, (DWORD)(deadline - now < 20 ? deadline - now : 20)) ==
                     WAIT_OBJECT_0;
        }
        if (!exited) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
        } else if (!failed) {
            GetExitCodeProcess(process.hProcess, &exit_code);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    CloseHandle(read_end);
    if (!started || exit_code != 0 || used >= sizeof output)
        return 0;
    /* The first line, without its line break. */
    output[used] = '\0';
    output[strcspn(output, "\r\n")] = '\0';
    if (output[0]) {
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output, -1, installation, (int)size);
        found = length > 0;
    }
    return found;
}

/*
 * The search order of W19 in Neura's Windows port: never the current
 * directory, and only files that exist.
 */
static int search_platform(Search *search) {
    const wchar_t *program_files = environment_value(L"ProgramW6432");
    const wchar_t *visual_studio = environment_value(L"VCINSTALLDIR");
    wchar_t directory[1024];
    DWORD size = (DWORD)sizeof directory;

    /* 1. The LLVM installer's default directory, then the directory it
          recorded in the registry. */
    if (!program_files)
        program_files = environment_value(L"ProgramFiles");
    if (program_files && search_try_below(search, program_files, L"LLVM\\bin"))
        return 1;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LLVM\\LLVM", NULL, RRF_RT_REG_SZ, NULL, directory, &size) ==
            ERROR_SUCCESS &&
        directory[0] && search_try_below(search, directory, L"bin"))
        return 1;
    /* 2. The C++ Clang tools of Visual Studio: those of the developer
          environment's Visual Studio, then of the newest one with them. */
    if (visual_studio && search_try_below(search, visual_studio, L"Tools\\Llvm\\x64\\bin"))
        return 1;
    if (visual_studio_with_clang(directory, sizeof directory / sizeof directory[0]) &&
        search_try_below(search, directory, L"VC\\Tools\\Llvm\\x64\\bin"))
        return 1;
    /* 3. Each directory of PATH, by its full path. */
    return each_path_directory(search_path_directory, search);
}
#elif defined(__APPLE__)
static int search_platform(Search *search) {
    return search_try(search, "libclang.dylib") ||
           search_try(search, "/Library/Developer/CommandLineTools/usr/lib/libclang.dylib") ||
           search_try(search, "/Applications/Xcode.app/Contents/Developer/Toolchains/"
                              "XcodeDefault.xctoolchain/usr/lib/libclang.dylib") ||
           search_try(search, "/opt/homebrew/opt/llvm/lib/libclang.dylib") ||
           search_try(search, "/usr/local/opt/llvm/lib/libclang.dylib");
}
#else
static int search_try_versioned(Search *search, const char *format) {
    char name[256];
    int major;
    for (major = NEURA_LIBCLANG_NEWEST_MAJOR; major >= NEURA_LIBCLANG_OLDEST_MAJOR; --major) {
        snprintf(name, sizeof name, format, major, major);
        if (search_try(search, name))
            return 1;
    }
    return 0;
}

static int search_platform(Search *search) {
    if (search_try(search, "libclang.so") || search_try(search, "libclang.so.1"))
        return 1;
    {
        int major;
        char name[256];
        for (major = NEURA_LIBCLANG_NEWEST_MAJOR; major >= NEURA_LIBCLANG_OLDEST_MAJOR; --major) {
            const char *formats[] = {"libclang-%d.so", "libclang-%d.so.1", "libclang-%d.so.%d",
                                     "libclang.so.%d"};
            size_t i;
            for (i = 0; i < sizeof formats / sizeof formats[0]; ++i) {
                snprintf(name, sizeof name, formats[i], major, major);
                if (search_try(search, name))
                    return 1;
            }
        }
    }
    return search_try_versioned(search, "/usr/lib/llvm-%d/lib/libclang.so") ||
           search_try_versioned(search, "/usr/lib/llvm-%d/lib/libclang.so.1");
}
#endif

#ifdef _WIN32
/* The NEURA_LIBCLANG environment variable as UTF-8, or NULL when it is unset
   or empty. It is read from the UTF-16 environment, so the value does not
   depend on the process code page; a value that is not valid text is kept
   as a description, which no library has as its name. Free with free(). */
static char *configured_library(void) {
    const wchar_t *wide = environment_value(L"NEURA_LIBCLANG");
    static const char invalid[] = "(a value that is not valid text)";
    char *value;
    int length;
    if (!wide)
        return NULL;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    value = (char *)malloc(length > 0 ? (size_t)length : sizeof invalid);
    if (!value)
        return NULL;
    if (length <= 0 || !to_narrow(value, (size_t)length, wide))
        memcpy(value, invalid, sizeof invalid);
    return value;
}
#endif

int neura_libclang_load(NeuraLibclang *lib, const char *explicit_path) {
#ifdef _WIN32
    char *environment;
#else
    const char *environment;
#endif
    char reason[1024];
    Search search;

    memset(lib, 0, sizeof *lib);
    check_table_signatures(lib);

    if (explicit_path && explicit_path[0]) {
        if (try_candidate(lib, explicit_path, reason, sizeof reason) == CANDIDATE_LOADED)
            return 1;
        snprintf(lib->error, sizeof lib->error, "cannot load libclang from %s: %s",
                 explicit_path, reason);
        return 0;
    }

#ifdef _WIN32
    environment = configured_library();
    if (environment) {
        int loaded = try_candidate(lib, environment, reason, sizeof reason) == CANDIDATE_LOADED;
        if (!loaded)
            snprintf(lib->error, sizeof lib->error,
                     "cannot load libclang from NEURA_LIBCLANG=%s: %s", environment, reason);
        free(environment);
        return loaded;
    }
#else
    environment = getenv("NEURA_LIBCLANG");
    if (environment && environment[0]) {
        if (try_candidate(lib, environment, reason, sizeof reason) == CANDIDATE_LOADED)
            return 1;
        snprintf(lib->error, sizeof lib->error,
                 "cannot load libclang from NEURA_LIBCLANG=%s: %s", environment, reason);
        return 0;
    }
#endif

    memset(&search, 0, sizeof search);
    search.lib = lib;
    if (search_platform(&search))
        return 1;

#ifdef _WIN32
    if (search.tried == 0)
        snprintf(lib->error, sizeof lib->error,
                 "cannot find libclang: no libclang.dll in the LLVM installation (%%ProgramW6432%%\\LLVM\\bin or "
                 "the directory in HKLM\\SOFTWARE\\LLVM\\LLVM), Visual Studio's C++ Clang tools, or a directory "
                 "of PATH");
    else
        snprintf(lib->error, sizeof lib->error,
                 "cannot find libclang: none of the %d libclang.dll files found could be loaded", search.tried);
#else
    snprintf(lib->error, sizeof lib->error,
             "cannot find libclang: none of the %d standard library names could be loaded",
             search.tried);
#endif
    if (search.unusable[0])
        append_text(lib->error, sizeof lib->error,
                    ". These libraries exist but cannot be used:%s", search.unusable);
    if (search.first_missing[0])
        append_text(lib->error, sizeof lib->error, ". First loader error: %s",
                    search.first_missing);
#ifdef _WIN32
    append_text(lib->error, sizeof lib->error,
                ". Install LLVM %d or newer (winget install LLVM.LLVM) or the C++ Clang tools of Visual Studio, "
                "or set NEURA_LIBCLANG to the path of libclang.dll",
                NEURA_LIBCLANG_OLDEST_MAJOR);
#else
    append_text(lib->error, sizeof lib->error,
                ". Install libclang %d or newer (Debian and Ubuntu: libclang1-19) or set "
                "NEURA_LIBCLANG to the path of the libclang shared library",
                NEURA_LIBCLANG_OLDEST_MAJOR);
#endif
    return 0;
}

void neura_libclang_unload(NeuraLibclang *lib) {
    close_library(lib->handle);
    memset(lib, 0, sizeof *lib);
}
