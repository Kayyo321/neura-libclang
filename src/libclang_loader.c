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
#include <windows.h>
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

/* ---- Platform loader ---------------------------------------------------- */

static void *open_library(const char *name, char *error, size_t error_size) {
#ifdef _WIN32
    HMODULE module = LoadLibraryA(name);
    if (!module) {
        DWORD code = GetLastError();
        char message[512] = "";
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                       0, message, (DWORD)sizeof message, NULL);
        snprintf(error, error_size, "%s: error %lu: %s", name, (unsigned long)code, message);
    }
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

static void close_library(void *handle) {
    if (!handle)
        return;
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
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
    DWORD length;
    (void)symbol;
    length = GetModuleFileNameA((HMODULE)lib->handle, lib->path, (DWORD)sizeof lib->path);
    if (length == 0 || length >= sizeof lib->path)
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
} Search;

/* Whether `name` is a path to a file that exists, rather than a bare name. */
static int existing_file(const char *name) {
    FILE *file;
    int is_path = strchr(name, '/') != NULL || strchr(name, '\\') != NULL;
    if (!is_path || !(file = fopen(name, "rb")))
        return 0;
    fclose(file);
    return 1;
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
#if defined(_WIN32)
    return search_try(search, "libclang.dll") ||
           search_try(search, "C:\\Program Files\\LLVM\\bin\\libclang.dll");
#elif defined(__APPLE__)
    return search_try(search, "libclang.dylib") ||
           search_try(search, "/Library/Developer/CommandLineTools/usr/lib/libclang.dylib") ||
           search_try(search, "/Applications/Xcode.app/Contents/Developer/Toolchains/"
                              "XcodeDefault.xctoolchain/usr/lib/libclang.dylib") ||
           search_try(search, "/opt/homebrew/opt/llvm/lib/libclang.dylib") ||
           search_try(search, "/usr/local/opt/llvm/lib/libclang.dylib");
#else
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
#endif
}

int neura_libclang_load(NeuraLibclang *lib, const char *explicit_path) {
    const char *environment;
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

    environment = getenv("NEURA_LIBCLANG");
    if (environment && environment[0]) {
        if (try_candidate(lib, environment, reason, sizeof reason) == CANDIDATE_LOADED)
            return 1;
        snprintf(lib->error, sizeof lib->error,
                 "cannot load libclang from NEURA_LIBCLANG=%s: %s", environment, reason);
        return 0;
    }

    memset(&search, 0, sizeof search);
    search.lib = lib;
    if (search_platform(&search))
        return 1;

    snprintf(lib->error, sizeof lib->error,
             "cannot find libclang: none of the %d standard library names could be loaded",
             search.tried);
    if (search.unusable[0])
        append_text(lib->error, sizeof lib->error,
                    ". These libraries exist but cannot be used:%s", search.unusable);
    if (search.first_missing[0])
        append_text(lib->error, sizeof lib->error, ". First loader error: %s",
                    search.first_missing);
    append_text(lib->error, sizeof lib->error,
                ". Install libclang %d or newer (Debian and Ubuntu: libclang1-19) or set "
                "NEURA_LIBCLANG to the path of the libclang shared library",
                NEURA_LIBCLANG_OLDEST_MAJOR);
    return 0;
}

void neura_libclang_unload(NeuraLibclang *lib) {
    close_library(lib->handle);
    memset(lib, 0, sizeof *lib);
}
