# neura-libclang

The [libclang](https://clang.llvm.org/docs/LibClang.html) C API headers and a
small run-time loader, for programs that read C headers without linking
against libclang. [Neura](https://github.com/Kayyo321/neura) uses it as a
submodule to import C headers.

Building against this repository needs only a C compiler and the platform's
dynamic loader. At run time, `neura_libclang_load()` opens a system libclang
with `dlopen` (`LoadLibraryExW` on Windows), checks that it exports every
function the importer uses, and fills a table of function pointers. Nothing
links libclang, and no LLVM source tree is needed.

## Version pin

Pinned to LLVM 19.1.7 (tag `llvmorg-19.1.7`, commit `cd708029e0b2869e80abe31ddb175f7c35361f90`).

`include/clang-c/` holds that release's `clang/include/clang-c/` headers,
unmodified. [NOTICE.md](NOTICE.md) records their origin and
[LICENSE.TXT](LICENSE.TXT) is LLVM's license.

The loader accepts libclang 14 or newer. The functions it resolves all
predate LLVM 14, and libclang keeps their signatures and the structs they
pass by value stable, so one set of headers serves every supported release.
Headers newer than the installed library are fine as long as callers use
only the functions in the table.

## Contents

| Path | Purpose |
| --- | --- |
| `include/clang-c/` | The pinned libclang C API headers |
| `src/libclang_loader.h` | The function table, `neura_libclang_load()`, and `neura_libclang_unload()` |
| `src/libclang_loader.c` | Library search, symbol resolution, and error reporting |
| `tests/self_test.c` | Loads the system libclang and reads a small header through the table |
| `scripts/update-headers.sh` | Replaces the headers and license with another release's |

## Using the loader

With CMake, add the directory and link the `neura_libclang` target. It
provides the include directories for `<clang-c/Index.h>` and
`libclang_loader.h`, and links `${CMAKE_DL_LIBS}`.

```cmake
add_subdirectory(third_party/neura-libclang)
target_link_libraries(my_tool PRIVATE neura_libclang)
```

```c
#include "libclang_loader.h"

NeuraLibclang lib;
if (!neura_libclang_load(&lib, NULL)) {
    fprintf(stderr, "%s\n", lib.error);
    return 1;
}
printf("using %s: %s\n", lib.path, lib.version);

CXIndex index = lib.clang_createIndex(0, 0);
CXTranslationUnit unit;
const char *args[] = {"-x", "c"};
if (lib.clang_parseTranslationUnit2(index, "api.h", args, 2, NULL, 0,
                                    NEURA_LIBCLANG_HEADER_PARSE_OPTIONS,
                                    &unit) == CXError_Success) {
    /* lib.clang_visitChildren(lib.clang_getTranslationUnitCursor(unit), ...) */
    lib.clang_disposeTranslationUnit(unit);
}
lib.clang_disposeIndex(index);
neura_libclang_unload(&lib);
```

Each table entry has the name and signature of the libclang function it
points to; the loader checks every entry against the pinned headers at
compile time. `NEURA_LIBCLANG_FUNCTIONS` in `libclang_loader.h` lists them:
strings and version, index and translation units, diagnostics, cursor
traversal, kinds, spellings, and properties (including bit-fields and
function-like macros), source locations, types (size, alignment, field
offsets, canonical, pointee, result, and argument types, variadic
functions), enumerator values, constant evaluation through
`clang_Cursor_Evaluate`, and tokens. `NEURA_LIBCLANG_HEADER_PARSE_OPTIONS`
keeps the detailed preprocessing record, so macro definitions appear as
cursors, and skips function bodies.

### Where the loader looks

The first source that names a library decides the search:

1. The `explicit_path` argument, when it is not `NULL` or empty.
2. The `NEURA_LIBCLANG` environment variable, when it is set and not empty.

When either names a library, only that library is tried, so a wrong setting
is reported instead of being replaced by another libclang. Otherwise the
loader tries the platform's usual names and keeps the first library that
exports every function in the table:

- **Linux and other ELF systems:** `libclang.so`, `libclang.so.1`, then for
  each LLVM major version N from 21 down to 14 `libclang-N.so`,
  `libclang-N.so.1`, `libclang-N.so.N`, and `libclang.so.N`, then
  `/usr/lib/llvm-N/lib/libclang.so` and `/usr/lib/llvm-N/lib/libclang.so.1`.
- **macOS:** `libclang.dylib`, then the Xcode Command Line Tools, Xcode, and
  Homebrew (`/opt/homebrew/opt/llvm`, `/usr/local/opt/llvm`) locations.
- **Windows:** `libclang.dll` in the LLVM installation
  (`%ProgramW6432%\LLVM\bin`, then the `bin` directory below the one
  recorded in the registry key `HKLM\SOFTWARE\LLVM\LLVM`), then in the C++
  Clang tools of Visual Studio (`VC\Tools\Llvm\x64\bin` of the developer
  environment's Visual Studio, `VCINSTALLDIR`, then of the newest Visual
  Studio that `vswhere` reports with them, waiting at most 30 seconds for
  it), then in each directory of `PATH`
  by its full path. Only files that exist are tried, and the current
  directory never is.

Names are UTF-8. On Windows, a name with `/` or `\`, or one that begins with
a drive, is a path: that file is loaded, made absolute against the current
directory when it is relative, and its own dependencies are found in its
directory (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`). A bare name is looked for in
the application's directory and the system directories, then in each
directory of `PATH`, never in the current directory. `neura_libclang_unload()`
leaves `libclang.dll` loaded until the process exits, because a process that
has unloaded it fails at exit.

After a successful load, `lib.requested` is the name passed to the platform
loader, `lib.path` the file it opened, `lib.version` the
`clang_getClangVersion()` string, and `lib.major` the LLVM major version. On
failure, `lib.error` says which library or setting failed and why, and every
function pointer is `NULL`.

On Debian and Ubuntu, install libclang with `apt install libclang1-19` (or a
newer `libclang1-N`); the development package is not needed.
On Windows, install LLVM (`winget install LLVM.LLVM`) or the C++ Clang tools
for Windows component of Visual Studio.

## Building and testing

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`neura-libclang-load` loads the system libclang and checks a small header:
record size, alignment, and field offsets, a bit-field width, function
argument and result types, a variadic function, enumerator values, constant
evaluation, function-like and object-like macros, and macro tokens. Without
a libclang it exits with 77, which CTest reports as skipped; set
`NEURA_LIBCLANG_REQUIRED=1` to make that a failure. `neura-libclang-missing`
and `neura-libclang-env` check the errors for a missing explicit path and a
missing `NEURA_LIBCLANG`. On Windows, `neura-libclang-cwd` checks that a bare
name is not looked for in the current directory.

When used as a subproject, the tests are registered only when the parent
sets `NEURA_LIBCLANG_TESTS` to `ON` before `add_subdirectory`.

## Updating the headers

1. Run the script with the new release tag. It downloads that tag's
   `clang/include/clang-c/` headers and `llvm/LICENSE.TXT`, replaces the
   copies here, and records the tag and commit in `NOTICE.md` and on the
   "Pinned to" line above:

   ```sh
   scripts/update-headers.sh llvmorg-20.1.8
   ```

2. Review `git diff`, especially headers that were added or removed and
   changes to the functions the loader resolves.
3. Rebuild. The compile-time check in `src/libclang_loader.c` fails if a
   signature in `NEURA_LIBCLANG_FUNCTIONS` no longer matches the headers.
4. Run the tests against the oldest and newest libclang you support. If the
   new release's library is newer than `NEURA_LIBCLANG_NEWEST_MAJOR` in
   `src/libclang_loader.h`, raise that value and the version range in this
   README.
5. Commit, then bump the submodule in projects that use it.

## License

Apache License 2.0 with LLVM Exceptions; see [LICENSE.TXT](LICENSE.TXT) and
[NOTICE.md](NOTICE.md).
