/*
 * libclang_loader.h: load libclang at run time.
 *
 * Programs that read C headers through libclang include this file instead of
 * linking against libclang. neura_libclang_load() opens a libclang shared
 * library with dlopen (LoadLibrary on Windows) and fills a table of function
 * pointers, so a build needs only the headers under include/clang-c and the
 * platform's dynamic loader.
 *
 * Every table entry has the same name and signature as the libclang function
 * it points to:
 *
 *     NeuraLibclang lib;
 *     if (!neura_libclang_load(&lib, NULL)) {
 *         fprintf(stderr, "%s\n", lib.error);
 *         return 1;
 *     }
 *     CXIndex index = lib.clang_createIndex(0, 0);
 *     ...
 *     neura_libclang_unload(&lib);
 *
 * The headers are pinned to one LLVM release (see NOTICE.md). The loader
 * accepts any libclang that exports every function in the table, and all of
 * them predate LLVM 14; the libclang C API keeps these entry points and the
 * layout of the structs they pass by value stable across releases.
 */
#ifndef NEURA_LIBCLANG_LOADER_H
#define NEURA_LIBCLANG_LOADER_H

#include <clang-c/Index.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse options for importing a header: keep macro definitions in the
 * translation unit, and skip function bodies, which a declaration importer
 * never reads.
 */
#define NEURA_LIBCLANG_HEADER_PARSE_OPTIONS \
    (CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_SkipFunctionBodies)

/* The newest and oldest LLVM major versions the name search tries. */
#define NEURA_LIBCLANG_NEWEST_MAJOR 21
#define NEURA_LIBCLANG_OLDEST_MAJOR 14

/*
 * NEURA_LIBCLANG_FUNCTIONS(X) applies X(return type, name, parameters) to
 * every function the loader resolves. A library that lacks any of them is
 * rejected.
 */
#define NEURA_LIBCLANG_FUNCTIONS(X) \
    /* Strings and version. */ \
    X(const char *, clang_getCString, (CXString string)) \
    X(void, clang_disposeString, (CXString string)) \
    X(CXString, clang_getClangVersion, (void)) \
    \
    /* Index and translation units. */ \
    X(CXIndex, clang_createIndex, (int excludeDeclarationsFromPCH, int displayDiagnostics)) \
    X(void, clang_disposeIndex, (CXIndex index)) \
    X(enum CXErrorCode, clang_parseTranslationUnit2, \
      (CXIndex index, const char *source_filename, const char *const *command_line_args, \
       int num_command_line_args, struct CXUnsavedFile *unsaved_files, \
       unsigned num_unsaved_files, unsigned options, CXTranslationUnit *out_TU)) \
    X(void, clang_disposeTranslationUnit, (CXTranslationUnit unit)) \
    X(CXString, clang_getTranslationUnitSpelling, (CXTranslationUnit unit)) \
    X(CXCursor, clang_getTranslationUnitCursor, (CXTranslationUnit unit)) \
    \
    /* Diagnostics. */ \
    X(unsigned, clang_getNumDiagnostics, (CXTranslationUnit unit)) \
    X(CXDiagnostic, clang_getDiagnostic, (CXTranslationUnit unit, unsigned index)) \
    X(enum CXDiagnosticSeverity, clang_getDiagnosticSeverity, (CXDiagnostic diagnostic)) \
    X(CXString, clang_getDiagnosticSpelling, (CXDiagnostic diagnostic)) \
    X(CXSourceLocation, clang_getDiagnosticLocation, (CXDiagnostic diagnostic)) \
    X(CXString, clang_formatDiagnostic, (CXDiagnostic diagnostic, unsigned options)) \
    X(unsigned, clang_defaultDiagnosticDisplayOptions, (void)) \
    X(void, clang_disposeDiagnostic, (CXDiagnostic diagnostic)) \
    \
    /* Cursor traversal and identity. */ \
    X(unsigned, clang_visitChildren, \
      (CXCursor parent, CXCursorVisitor visitor, CXClientData client_data)) \
    X(CXCursor, clang_getNullCursor, (void)) \
    X(int, clang_Cursor_isNull, (CXCursor cursor)) \
    X(unsigned, clang_equalCursors, (CXCursor a, CXCursor b)) \
    X(unsigned, clang_hashCursor, (CXCursor cursor)) \
    X(CXCursor, clang_getCursorSemanticParent, (CXCursor cursor)) \
    X(CXCursor, clang_getCursorReferenced, (CXCursor cursor)) \
    X(CXCursor, clang_getCursorDefinition, (CXCursor cursor)) \
    X(unsigned, clang_isCursorDefinition, (CXCursor cursor)) \
    X(CXCursor, clang_getCanonicalCursor, (CXCursor cursor)) \
    \
    /* Cursor kind, spelling, and properties. */ \
    X(enum CXCursorKind, clang_getCursorKind, (CXCursor cursor)) \
    X(CXString, clang_getCursorKindSpelling, (enum CXCursorKind kind)) \
    X(unsigned, clang_isDeclaration, (enum CXCursorKind kind)) \
    X(unsigned, clang_isPreprocessing, (enum CXCursorKind kind)) \
    X(CXString, clang_getCursorSpelling, (CXCursor cursor)) \
    X(CXString, clang_getCursorDisplayName, (CXCursor cursor)) \
    X(enum CXLinkageKind, clang_getCursorLinkage, (CXCursor cursor)) \
    X(enum CX_StorageClass, clang_Cursor_getStorageClass, (CXCursor cursor)) \
    X(unsigned, clang_Cursor_isAnonymous, (CXCursor cursor)) \
    X(unsigned, clang_Cursor_isAnonymousRecordDecl, (CXCursor cursor)) \
    X(unsigned, clang_Cursor_isBitField, (CXCursor cursor)) \
    X(int, clang_getFieldDeclBitWidth, (CXCursor cursor)) \
    X(long long, clang_Cursor_getOffsetOfField, (CXCursor cursor)) \
    X(unsigned, clang_Cursor_isMacroFunctionLike, (CXCursor cursor)) \
    X(unsigned, clang_Cursor_isMacroBuiltin, (CXCursor cursor)) \
    X(unsigned, clang_Cursor_isFunctionInlined, (CXCursor cursor)) \
    X(unsigned, clang_Cursor_isVariadic, (CXCursor cursor)) \
    X(int, clang_Cursor_getNumArguments, (CXCursor cursor)) \
    X(CXCursor, clang_Cursor_getArgument, (CXCursor cursor, unsigned index)) \
    X(long long, clang_getEnumConstantDeclValue, (CXCursor cursor)) \
    X(unsigned long long, clang_getEnumConstantDeclUnsignedValue, (CXCursor cursor)) \
    X(CXType, clang_getEnumDeclIntegerType, (CXCursor cursor)) \
    X(CXType, clang_getTypedefDeclUnderlyingType, (CXCursor cursor)) \
    \
    /* Cursor types. */ \
    X(CXType, clang_getCursorType, (CXCursor cursor)) \
    X(CXType, clang_getCursorResultType, (CXCursor cursor)) \
    \
    /* Source locations. */ \
    X(CXSourceLocation, clang_getCursorLocation, (CXCursor cursor)) \
    X(CXSourceRange, clang_getCursorExtent, (CXCursor cursor)) \
    X(int, clang_Location_isInSystemHeader, (CXSourceLocation location)) \
    X(int, clang_Location_isFromMainFile, (CXSourceLocation location)) \
    X(void, clang_getExpansionLocation, \
      (CXSourceLocation location, CXFile *file, unsigned *line, unsigned *column, \
       unsigned *offset)) \
    X(void, clang_getSpellingLocation, \
      (CXSourceLocation location, CXFile *file, unsigned *line, unsigned *column, \
       unsigned *offset)) \
    X(void, clang_getPresumedLocation, \
      (CXSourceLocation location, CXString *filename, unsigned *line, unsigned *column)) \
    X(CXString, clang_getFileName, (CXFile file)) \
    \
    /* Types. */ \
    X(CXString, clang_getTypeSpelling, (CXType type)) \
    X(CXString, clang_getTypeKindSpelling, (enum CXTypeKind kind)) \
    X(CXString, clang_getTypedefName, (CXType type)) \
    X(CXType, clang_getCanonicalType, (CXType type)) \
    X(CXType, clang_Type_getNamedType, (CXType type)) \
    X(CXType, clang_getPointeeType, (CXType type)) \
    X(CXType, clang_getResultType, (CXType type)) \
    X(int, clang_getNumArgTypes, (CXType type)) \
    X(CXType, clang_getArgType, (CXType type, unsigned index)) \
    X(unsigned, clang_isFunctionTypeVariadic, (CXType type)) \
    X(enum CXCallingConv, clang_getFunctionTypeCallingConv, (CXType type)) \
    X(CXType, clang_getElementType, (CXType type)) \
    X(long long, clang_getNumElements, (CXType type)) \
    X(CXType, clang_getArrayElementType, (CXType type)) \
    X(long long, clang_getArraySize, (CXType type)) \
    X(unsigned, clang_isConstQualifiedType, (CXType type)) \
    X(unsigned, clang_isVolatileQualifiedType, (CXType type)) \
    X(unsigned, clang_isRestrictQualifiedType, (CXType type)) \
    X(CXCursor, clang_getTypeDeclaration, (CXType type)) \
    X(long long, clang_Type_getSizeOf, (CXType type)) \
    X(long long, clang_Type_getAlignOf, (CXType type)) \
    X(long long, clang_Type_getOffsetOf, (CXType type, const char *field)) \
    X(unsigned, clang_Type_visitFields, \
      (CXType type, CXFieldVisitor visitor, CXClientData client_data)) \
    \
    /* Constant evaluation. */ \
    X(CXEvalResult, clang_Cursor_Evaluate, (CXCursor cursor)) \
    X(CXEvalResultKind, clang_EvalResult_getKind, (CXEvalResult result)) \
    X(int, clang_EvalResult_getAsInt, (CXEvalResult result)) \
    X(long long, clang_EvalResult_getAsLongLong, (CXEvalResult result)) \
    X(unsigned, clang_EvalResult_isUnsignedInt, (CXEvalResult result)) \
    X(unsigned long long, clang_EvalResult_getAsUnsigned, (CXEvalResult result)) \
    X(double, clang_EvalResult_getAsDouble, (CXEvalResult result)) \
    X(const char *, clang_EvalResult_getAsStr, (CXEvalResult result)) \
    X(void, clang_EvalResult_dispose, (CXEvalResult result)) \
    \
    /* Tokens, for reading macro bodies. */ \
    X(void, clang_tokenize, \
      (CXTranslationUnit unit, CXSourceRange range, CXToken **tokens, unsigned *count)) \
    X(CXTokenKind, clang_getTokenKind, (CXToken token)) \
    X(CXString, clang_getTokenSpelling, (CXTranslationUnit unit, CXToken token)) \
    X(void, clang_disposeTokens, (CXTranslationUnit unit, CXToken *tokens, unsigned count))

#define NEURA_LIBCLANG_FIELD(result, name, parameters) result (*name) parameters;

typedef struct NeuraLibclang {
    /* The handle returned by the platform loader, or NULL. */
    void *handle;
    /* The name or path that was passed to the platform loader. */
    char requested[1024];
    /* The file the library was loaded from, as the platform loader reports it. */
    char path[1024];
    /* clang_getClangVersion(), for example "Debian clang version 19.1.7 (3+b1)". */
    char version[256];
    /* The LLVM major version parsed from `version`, or 0 if it has none. */
    int major;
    /* Why the last neura_libclang_load() failed, or the empty string. */
    char error[4096];

    NEURA_LIBCLANG_FUNCTIONS(NEURA_LIBCLANG_FIELD)
} NeuraLibclang;

#undef NEURA_LIBCLANG_FIELD

/*
 * Load libclang and fill `lib`. Returns 1 on success. On failure returns 0,
 * leaves every function pointer NULL, and describes the failure in
 * lib->error.
 *
 * The first source that names a library decides the search:
 *   1. `explicit_path`, when it is neither NULL nor empty;
 *   2. otherwise the NEURA_LIBCLANG environment variable, when it is set and
 *      not empty;
 * Only that library is tried, so a wrong setting is reported instead of
 * being replaced by another libclang. With neither, the loader tries the
 * platform's usual names in order and keeps the first library that exports
 * every function:
 *   Linux and other ELF systems: libclang.so, libclang.so.1, then for each
 *     major version N from NEURA_LIBCLANG_NEWEST_MAJOR down to
 *     NEURA_LIBCLANG_OLDEST_MAJOR libclang-N.so, libclang-N.so.1,
 *     libclang-N.so.N, and libclang.so.N, then /usr/lib/llvm-N/lib/libclang.so
 *     and /usr/lib/llvm-N/lib/libclang.so.1 for each N;
 *   macOS: libclang.dylib, then the Xcode Command Line Tools, Xcode, and
 *     Homebrew locations;
 *   Windows: libclang.dll, then the default LLVM installation directory.
 */
int neura_libclang_load(NeuraLibclang *lib, const char *explicit_path);

/* Close the library opened by neura_libclang_load() and clear `lib`. */
void neura_libclang_unload(NeuraLibclang *lib);

#ifdef __cplusplus
}
#endif

#endif /* NEURA_LIBCLANG_LOADER_H */
