/*
 * self_test.c: load the system libclang and read a small header through the
 * function table.
 *
 *   neura_libclang_self_test           load libclang and check the header
 *   neura_libclang_self_test missing   check the error for a missing explicit path
 *   neura_libclang_self_test env       check the error for a missing NEURA_LIBCLANG
 *                                      (run with NEURA_LIBCLANG set to such a path)
 *
 * When the standard search finds no usable libclang, the default mode exits
 * with 77, which CTest reports as skipped. Set NEURA_LIBCLANG_REQUIRED=1 to
 * make that a failure. A NEURA_LIBCLANG that cannot be loaded always fails.
 */
#include "libclang_loader.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SKIPPED 77

static const char header_name[] = "neura_self_test.h";
static const char header[] =
    "#define ANSWER 42\n"
    "#define SQUARE(x) ((x) * (x))\n"
    "typedef struct point { int x; int y; unsigned flags : 3; } point;\n"
    "enum color { RED, GREEN = 5, BLUE };\n"
    "static const long long big = 1LL << 40;\n"
    "int add(int a, int b);\n"
    "int log_message(const char *format, ...);\n";

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "self_test.c:%d: check failed: %s\n", __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

typedef struct Found {
    const NeuraLibclang *lib;
    CXCursor point_struct, add, log_message, big, red, green, blue, answer, square;
    int found;
} Found;

static int spelled(const NeuraLibclang *lib, CXCursor cursor, const char *expected) {
    CXString spelling = lib->clang_getCursorSpelling(cursor);
    int same = strcmp(lib->clang_getCString(spelling), expected) == 0;
    lib->clang_disposeString(spelling);
    return same;
}

static enum CXChildVisitResult visit(CXCursor cursor, CXCursor parent, CXClientData data) {
    Found *found = (Found *)data;
    const NeuraLibclang *lib = found->lib;
    enum CXCursorKind kind = lib->clang_getCursorKind(cursor);
    (void)parent;

    if (!lib->clang_Location_isFromMainFile(lib->clang_getCursorLocation(cursor)))
        return CXChildVisit_Continue;

#define REMEMBER(cursor_kind, name, slot) \
    if (kind == cursor_kind && spelled(lib, cursor, name)) { \
        found->slot = cursor; \
        ++found->found; \
    }
    REMEMBER(CXCursor_StructDecl, "point", point_struct)
    REMEMBER(CXCursor_FunctionDecl, "add", add)
    REMEMBER(CXCursor_FunctionDecl, "log_message", log_message)
    REMEMBER(CXCursor_VarDecl, "big", big)
    REMEMBER(CXCursor_EnumConstantDecl, "RED", red)
    REMEMBER(CXCursor_EnumConstantDecl, "GREEN", green)
    REMEMBER(CXCursor_EnumConstantDecl, "BLUE", blue)
    REMEMBER(CXCursor_MacroDefinition, "ANSWER", answer)
    REMEMBER(CXCursor_MacroDefinition, "SQUARE", square)
#undef REMEMBER

    return kind == CXCursor_EnumDecl ? CXChildVisit_Recurse : CXChildVisit_Continue;
}

static int count_errors(const NeuraLibclang *lib, CXTranslationUnit unit) {
    unsigned i, count = lib->clang_getNumDiagnostics(unit);
    int errors = 0;
    for (i = 0; i < count; ++i) {
        CXDiagnostic diagnostic = lib->clang_getDiagnostic(unit, i);
        if (lib->clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
            CXString text = lib->clang_formatDiagnostic(
                diagnostic, lib->clang_defaultDiagnosticDisplayOptions());
            fprintf(stderr, "diagnostic: %s\n", lib->clang_getCString(text));
            lib->clang_disposeString(text);
            ++errors;
        }
        lib->clang_disposeDiagnostic(diagnostic);
    }
    return errors;
}

/* The second token of ANSWER's definition, which should be "42". */
static void check_macro_tokens(const NeuraLibclang *lib, CXTranslationUnit unit, CXCursor macro) {
    CXToken *tokens = NULL;
    unsigned count = 0;
    lib->clang_tokenize(unit, lib->clang_getCursorExtent(macro), &tokens, &count);
    CHECK(count == 2);
    if (count == 2) {
        CXString value = lib->clang_getTokenSpelling(unit, tokens[1]);
        CHECK(lib->clang_getTokenKind(tokens[1]) == CXToken_Literal);
        CHECK(strcmp(lib->clang_getCString(value), "42") == 0);
        lib->clang_disposeString(value);
    }
    lib->clang_disposeTokens(unit, tokens, count);
}

/* Only `flags` in struct point is a bit-field. */
static enum CXVisitorResult visit_field(CXCursor cursor, CXClientData data) {
    const NeuraLibclang *lib = (const NeuraLibclang *)data;
    if (spelled(lib, cursor, "flags")) {
        CHECK(lib->clang_Cursor_isBitField(cursor));
        CHECK(lib->clang_getFieldDeclBitWidth(cursor) == 3);
    } else {
        CHECK(!lib->clang_Cursor_isBitField(cursor));
    }
    return CXVisit_Continue;
}

static void check_header(const NeuraLibclang *lib) {
    const char *arguments[] = {"-x", "c", "-std=c11"};
    struct CXUnsavedFile file;
    CXIndex index = lib->clang_createIndex(0, 0);
    CXTranslationUnit unit = NULL;
    enum CXErrorCode code;
    Found found;

    file.Filename = header_name;
    file.Contents = header;
    file.Length = (unsigned long)(sizeof header - 1);
    code = lib->clang_parseTranslationUnit2(index, header_name, arguments, 3, &file, 1,
                                            NEURA_LIBCLANG_HEADER_PARSE_OPTIONS, &unit);
    CHECK(code == CXError_Success);
    if (code != CXError_Success) {
        lib->clang_disposeIndex(index);
        return;
    }
    CHECK(count_errors(lib, unit) == 0);

    memset(&found, 0, sizeof found);
    found.lib = lib;
    lib->clang_visitChildren(lib->clang_getTranslationUnitCursor(unit), visit, &found);
    CHECK(found.found == 9);

    if (found.found == 9) {
        CXType point = lib->clang_getCursorType(found.point_struct);
        CXType add = lib->clang_getCursorType(found.add);
        CXType log_message = lib->clang_getCursorType(found.log_message);
        CXEvalResult big;

        /* Record layout. Offsets are in bits. */
        CHECK(lib->clang_Type_getSizeOf(point) == 12);
        CHECK(lib->clang_Type_getAlignOf(point) == 4);
        CHECK(lib->clang_Type_getOffsetOf(point, "y") == 32);
        CHECK(lib->clang_Type_getOffsetOf(point, "flags") == 64);
        lib->clang_Type_visitFields(point, visit_field, (CXClientData)lib);

        /* Functions. */
        CHECK(lib->clang_getNumArgTypes(add) == 2);
        CHECK(lib->clang_getCanonicalType(lib->clang_getResultType(add)).kind == CXType_Int);
        CHECK(lib->clang_getArgType(add, 1).kind == CXType_Int);
        CHECK(!lib->clang_isFunctionTypeVariadic(add));
        CHECK(lib->clang_isFunctionTypeVariadic(log_message));
        /* libclang parses for the host, which this file is compiled for too.
         * Plain char is signed on some hosts (x86-64) and unsigned on others
         * (AArch64 Linux), and libclang names the two differently. */
        CHECK(lib->clang_getPointeeType(lib->clang_getArgType(log_message, 0)).kind ==
              (CHAR_MIN < 0 ? CXType_Char_S : CXType_Char_U));
        CHECK(lib->clang_isConstQualifiedType(
            lib->clang_getPointeeType(lib->clang_getArgType(log_message, 0))));

        /* Enumerators. */
        CHECK(lib->clang_getEnumConstantDeclValue(found.red) == 0);
        CHECK(lib->clang_getEnumConstantDeclValue(found.green) == 5);
        CHECK(lib->clang_getEnumConstantDeclValue(found.blue) == 6);

        /* Constant evaluation. */
        big = lib->clang_Cursor_Evaluate(found.big);
        CHECK(big != NULL);
        if (big) {
            CHECK(lib->clang_EvalResult_getKind(big) == CXEval_Int);
            CHECK(lib->clang_EvalResult_getAsLongLong(big) == 1LL << 40);
            lib->clang_EvalResult_dispose(big);
        }

        /* Macros come from the detailed preprocessing record. */
        CHECK(!lib->clang_Cursor_isMacroFunctionLike(found.answer));
        CHECK(lib->clang_Cursor_isMacroFunctionLike(found.square));
        check_macro_tokens(lib, unit, found.answer);
    }

    lib->clang_disposeTranslationUnit(unit);
    lib->clang_disposeIndex(index);
}

/* Load `explicit_path`, expect failure, and expect `mention` in the error. */
static int run_missing(const char *explicit_path, const char *mention) {
    NeuraLibclang lib;
    CHECK(!neura_libclang_load(&lib, explicit_path));
    CHECK(lib.handle == NULL);
    CHECK(lib.clang_createIndex == NULL);
    CHECK(strstr(lib.error, mention) != NULL);
    printf("expected error: %s\n", lib.error);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    NeuraLibclang lib;
    const char *required = getenv("NEURA_LIBCLANG_REQUIRED");
    const char *configured = getenv("NEURA_LIBCLANG");

    if (argc > 1 && strcmp(argv[1], "missing") == 0)
        return run_missing("/nonexistent/neura/libclang.so", "/nonexistent/neura/libclang.so");
    if (argc > 1 && strcmp(argv[1], "env") == 0) {
        CHECK(configured != NULL && configured[0] != '\0');
        return configured ? run_missing(NULL, "NEURA_LIBCLANG=") : 1;
    }
    if (argc > 1) {
        fprintf(stderr, "usage: %s [missing | env]\n", argv[0]);
        return 2;
    }

    if (!neura_libclang_load(&lib, NULL)) {
        int skip = !(configured && configured[0]) && !(required && strcmp(required, "1") == 0);
        fprintf(stderr, "%s\n", lib.error);
        return skip ? SKIPPED : 1;
    }
    printf("loaded %s (requested %s)\n", lib.path, lib.requested);
    printf("version %s (major %d)\n", lib.version, lib.major);
    CHECK(lib.major >= NEURA_LIBCLANG_OLDEST_MAJOR);

    check_header(&lib);
    neura_libclang_unload(&lib);
    CHECK(lib.handle == NULL);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
