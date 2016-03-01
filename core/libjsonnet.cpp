/*
Copyright 2015 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <cstdlib>
#include <cstring>
#include <cerrno>

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

extern "C" {
#include "libjsonnet.h"
}

#include "desugarer.h"
#include "formatter.h"
#include "parser.h"
#include "static_analysis.h"
#include "vm.h"

static void memory_panic(void)
{
    fputs("FATAL ERROR: A memory allocation error occurred.\n", stderr);
    abort();
}

static char *from_string(JsonnetVm* vm, const std::string &v)
{
    char *r = jsonnet_realloc(vm, nullptr, v.length() + 1);
    std::strcpy(r, v.c_str());
    return r;
}

static char *default_import_callback(void *ctx, const char *dir, const char *file,
                                     char **found_here_cptr, int *success);

struct JsonnetVm {
    double gcGrowthTrigger;
    unsigned maxStack;
    unsigned gcMinObjects;
    unsigned maxTrace;
    std::map<std::string, VmExt> ext;
    JsonnetImportCallback *importCallback;
    void *importCallbackContext;
    bool stringOutput;
    std::vector<std::string> jpaths;

    FmtOpts fmtOpts;
    bool fmtDebugDesugaring;
    
    JsonnetVm(void)
      : gcGrowthTrigger(2.0), maxStack(500), gcMinObjects(1000), maxTrace(20),
        importCallback(default_import_callback), importCallbackContext(this), stringOutput(false),
        fmtDebugDesugaring(false)
    {
        jpaths.emplace_back("/usr/share/" + std::string(jsonnet_version()) + "/");
        jpaths.emplace_back("/usr/local/share/" + std::string(jsonnet_version()) + "/");
    }
};

enum ImportStatus {
    IMPORT_STATUS_OK,
    IMPORT_STATUS_FILE_NOT_FOUND,
    IMPORT_STATUS_IO_ERROR
};

static enum ImportStatus try_path(const std::string &dir, const std::string &rel,
                                  std::string &content, std::string &found_here,
                                  std::string &err_msg)
{
    std::string abs_path;
    if (rel.length() == 0) {
        err_msg = "The empty string is not a valid filename";
        return IMPORT_STATUS_IO_ERROR;
    }
    // It is possible that rel is actually absolute.
    if (rel[0] == '/') {
        abs_path = rel;
    } else {
        abs_path = dir + rel;
    }

    if (abs_path[abs_path.length() - 1] == '/') {
        err_msg = "Attempted to import a directory";
        return IMPORT_STATUS_IO_ERROR;
    }

    std::ifstream f;
    f.open(abs_path.c_str());
    if (!f.good()) return IMPORT_STATUS_FILE_NOT_FOUND;
    try {
        content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure &io_err) {
        err_msg = io_err.what();
        return IMPORT_STATUS_IO_ERROR;
    }
    if (!f.good()) {
        err_msg = strerror(errno);
        return IMPORT_STATUS_IO_ERROR;
    }

    found_here = abs_path;

    return IMPORT_STATUS_OK;
}

static char *default_import_callback(void *ctx, const char *dir, const char *file,
                                     char **found_here_cptr, int *success)
{
    auto *vm = static_cast<JsonnetVm*>(ctx);

    std::string input, found_here, err_msg;

    ImportStatus status = try_path(dir, file, input, found_here, err_msg);

    std::vector<std::string> jpaths(vm->jpaths);

    // If not found, try library search path.
    while (status == IMPORT_STATUS_FILE_NOT_FOUND) {
        if (jpaths.size() == 0) {
            *success = 0;
            const char *err = "No match locally or in the Jsonnet library paths.";
            char *r = jsonnet_realloc(vm, nullptr, std::strlen(err) + 1);
            std::strcpy(r, err);
            return r;
        }
        status = try_path(jpaths.back(), file, input, found_here, err_msg);
        jpaths.pop_back();
    }

    if (status == IMPORT_STATUS_IO_ERROR) {
        *success = 0;
        return from_string(vm, err_msg);
    } else {
        assert(status == IMPORT_STATUS_OK);
        *success = 1;
        *found_here_cptr = from_string(vm, found_here);
        return from_string(vm, input);
    }
}

#define TRY try {
#define CATCH(func) \
    } catch (const std::bad_alloc &) {\
        memory_panic(); \
    } catch (const std::exception &e) {\
        std::cerr << "Something went wrong during " func ", please report this: " \
                  << e.what() << std::endl; \
        abort(); \
    }

const char *jsonnet_version(void)
{
    return LIB_JSONNET_VERSION;
}

JsonnetVm *jsonnet_make(void)
{
    TRY
    return new JsonnetVm();
    CATCH("jsonnet_make")
    return nullptr;
}

void jsonnet_destroy(JsonnetVm *vm)
{
    TRY
    delete vm;
    CATCH("jsonnet_destroy")
}

void jsonnet_max_stack(JsonnetVm *vm, unsigned v)
{
    vm->maxStack = v;
}

void jsonnet_gc_min_objects(JsonnetVm *vm, unsigned v)
{
    vm->gcMinObjects = v;
}

void jsonnet_gc_growth_trigger(JsonnetVm *vm, double v)
{
    vm->gcGrowthTrigger = v;
}

void jsonnet_string_output(struct JsonnetVm *vm, int v)
{
    vm->stringOutput = bool(v);
}

void jsonnet_import_callback(struct JsonnetVm *vm, JsonnetImportCallback *cb, void *ctx)
{
    vm->importCallback = cb;
    vm->importCallbackContext = ctx;
}

void jsonnet_ext_var(JsonnetVm *vm, const char *key, const char *val)
{
    vm->ext[key] = VmExt(val, false);
}

void jsonnet_ext_code(JsonnetVm *vm, const char *key, const char *val)
{
    vm->ext[key] = VmExt(val, true);
}

void jsonnet_fmt_debug_desugaring(JsonnetVm *vm, int v)
{
    vm->fmtDebugDesugaring = v;
}

void jsonnet_fmt_indent(JsonnetVm *vm, int v)
{
    vm->fmtOpts.indent = v;
}

void jsonnet_fmt_max_blank_lines(JsonnetVm *vm, int v)
{
    vm->fmtOpts.maxBlankLines = v;
}

void jsonnet_fmt_string(JsonnetVm *vm, int v)
{
    if (v != 'd' && v != 's' && v != 'l')
        v = 'l';
    vm->fmtOpts.stringStyle = v;
}

void jsonnet_fmt_comment(JsonnetVm *vm, int v)
{
    if (v != 'h' && v != 's' && v != 'l')
        v = 'l';
    vm->fmtOpts.commentStyle = v;
}

void jsonnet_fmt_pad_arrays(JsonnetVm *vm, int v)
{
    vm->fmtOpts.padArrays = v;
}

void jsonnet_fmt_pad_objects(JsonnetVm *vm, int v)
{
    vm->fmtOpts.padObjects = v;
}

void jsonnet_fmt_pretty_field_names(JsonnetVm *vm, int v)
{
    vm->fmtOpts.prettyFieldNames = v;
}

void jsonnet_max_trace(JsonnetVm *vm, unsigned v)
{
    vm->maxTrace = v;
}

void jsonnet_jpath_add(JsonnetVm *vm, const char *path_)
{
    if (std::strlen(path_) == 0) return;
    std::string path = path_;
    if (path[path.length() - 1] != '/') path += '/';
    vm->jpaths.emplace_back(path);
}

static char *jsonnet_fmt_snippet_aux(JsonnetVm *vm, const char *filename, const char *snippet,
                                     int *error)
{
    try {
        Allocator alloc;
        std::string json_str;
        AST *expr;
        std::map<std::string, std::string> files;
        Tokens tokens = jsonnet_lex(filename, snippet);

        // std::cout << jsonnet_unlex(tokens);

        expr = jsonnet_parse(&alloc, tokens);
        Fodder final_fodder = tokens.front().fodder;

        if (vm->fmtDebugDesugaring)
            jsonnet_desugar(&alloc, expr);

        json_str = jsonnet_fmt(expr, final_fodder, vm->fmtOpts);

        json_str += "\n";

        *error = false;
        return from_string(vm, json_str);

    } catch (StaticError &e) {
        std::stringstream ss;
        ss << "STATIC ERROR: " << e << std::endl;
        *error = true;
        return from_string(vm, ss.str());
    }

}

char *jsonnet_fmt_file(JsonnetVm *vm, const char *filename, int *error)
{
    TRY
        std::ifstream f;
        f.open(filename);
        if (!f.good()) {
            std::stringstream ss;
            ss << "Opening input file: " << filename << ": " << strerror(errno);
            *error = true;
            return from_string(vm, ss.str());
        }
        std::string input;
        input.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());

        return jsonnet_fmt_snippet_aux(vm, filename, input.c_str(), error);
    CATCH("jsonnet_fmt_file")
    return nullptr;  // Never happens.
}

char *jsonnet_fmt_snippet(JsonnetVm *vm, const char *filename, const char *snippet, int *error)
{
    TRY
        return jsonnet_fmt_snippet_aux(vm, filename, snippet, error);
    CATCH("jsonnet_fmt_snippet")
    return nullptr;  // Never happens.
}


static char *jsonnet_evaluate_snippet_aux(JsonnetVm *vm, const char *filename,
                                          const char *snippet, int *error, bool multi)
{
    try {
        Allocator alloc;
        std::string json_str;
        AST *expr;
        std::map<std::string, std::string> files;
        Tokens tokens = jsonnet_lex(filename, snippet);

        expr = jsonnet_parse(&alloc, tokens);

        jsonnet_desugar(&alloc, expr);

        jsonnet_static_analysis(expr);
        if (multi) {
            files = jsonnet_vm_execute_multi(&alloc, expr, vm->ext, vm->maxStack,
                                             vm->gcMinObjects, vm->gcGrowthTrigger,
                                             vm->importCallback, vm->importCallbackContext,
                                             vm->stringOutput);
        } else {
            json_str = jsonnet_vm_execute(&alloc, expr, vm->ext, vm->maxStack,
                                          vm->gcMinObjects, vm->gcGrowthTrigger,
                                          vm->importCallback, vm->importCallbackContext,
                                          vm->stringOutput);
            json_str += "\n";
        }

        if (multi) {
            size_t sz = 1; // final sentinel
            for (const auto &pair : files) {
                sz += pair.first.length() + 1; // include sentinel
                sz += pair.second.length() + 2; // Add a '\n' as well as sentinel
            }
            char *buf = (char*)::malloc(sz);
            if (buf == nullptr) memory_panic();
            std::ptrdiff_t i = 0;
            for (const auto &pair : files) {
                memcpy(&buf[i], pair.first.c_str(), pair.first.length() + 1);
                i += pair.first.length() + 1;
                memcpy(&buf[i], pair.second.c_str(), pair.second.length());
                i += pair.second.length();
                buf[i] = '\n';
                i++;
                buf[i] = '\0';
                i++;
            }
            buf[i] = '\0'; // final sentinel
            *error = false;
            return buf;
        } else {
            *error = false;
            return from_string(vm, json_str);
        }

    } catch (StaticError &e) {
        std::stringstream ss;
        ss << "STATIC ERROR: " << e << std::endl;
        *error = true;
        return from_string(vm, ss.str());

    } catch (RuntimeError &e) {
        std::stringstream ss;
        ss << "RUNTIME ERROR: " << e.msg << std::endl;
        const long max_above = vm->maxTrace / 2;
        const long max_below = vm->maxTrace - max_above;
        const long sz = e.stackTrace.size();
        for (long i = 0 ; i < sz ; ++i) {
            const auto &f = e.stackTrace[i];
            if (vm->maxTrace > 0 && i >= max_above && i < sz - max_below) {
                if (i == max_above)
                    ss << "\t..." << std::endl;
            } else {
                ss << "\t" << f.location << "\t" << f.name << std::endl;
            }
        }
        *error = true;
        return from_string(vm, ss.str());
    }

}

static char *jsonnet_evaluate_file_aux(JsonnetVm *vm, const char *filename, int *error, bool multi)
{
    std::ifstream f;
    f.open(filename);
    if (!f.good()) {
        std::stringstream ss;
        ss << "Opening input file: " << filename << ": " << strerror(errno);
        *error = true;
        return from_string(vm, ss.str());
    }
    std::string input;
    input.assign(std::istreambuf_iterator<char>(f),
                 std::istreambuf_iterator<char>());

    return jsonnet_evaluate_snippet_aux(vm, filename, input.c_str(), error, multi);
}

char *jsonnet_evaluate_file(JsonnetVm *vm, const char *filename, int *error)
{
    TRY
    return jsonnet_evaluate_file_aux(vm, filename, error, false);
    CATCH("jsonnet_evaluate_file")
    return nullptr;  // Never happens.
}

char *jsonnet_evaluate_file_multi(JsonnetVm *vm, const char *filename, int *error)
{
    TRY
    return jsonnet_evaluate_file_aux(vm, filename, error, true);
    CATCH("jsonnet_evaluate_file_multi")
    return nullptr;  // Never happens.
}

char *jsonnet_evaluate_snippet(JsonnetVm *vm, const char *filename, const char *snippet, int *error)
{
    TRY
    return jsonnet_evaluate_snippet_aux(vm, filename, snippet, error, false);
    CATCH("jsonnet_evaluate_snippet")
    return nullptr;  // Never happens.
}

char *jsonnet_evaluate_snippet_multi(JsonnetVm *vm, const char *filename,
                                     const char *snippet, int *error)
{
    TRY
    return jsonnet_evaluate_snippet_aux(vm, filename, snippet, error, true);
    CATCH("jsonnet_evaluate_snippet_multi")
    return nullptr;  // Never happens.
}

char *jsonnet_realloc(JsonnetVm *vm, char *str, size_t sz)
{
    (void) vm;
    if (str == nullptr) {
        if (sz == 0) return nullptr;
        auto *r = static_cast<char*>(::malloc(sz));
        if (r == nullptr) memory_panic();
        return r;
    } else {
        if (sz == 0) {
            ::free(str);
            return nullptr;
        } else {
            auto *r = static_cast<char*>(::realloc(str, sz));
            if (r == nullptr) memory_panic();
            return r;
        }
    }
}
