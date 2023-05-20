#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
/*#include <sys/poll.h>*/
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <malloc.h>
#endif
#if !defined(__wasi__) && !defined(_WIN32)
#include <dlfcn.h>
#endif

#ifdef HAVE_QUICKJS_CONFIG_H
#include <quickjs-config.h>
#endif

#ifndef CONFIG_BIGNUM
#warning No bignum!
#endif

#ifndef CONFIG_SHEXT
#ifdef _WIN32
#define CONFIG_SHEXT ".dll"
#elif defined(__APPLE__)
#define CONFIG_SHEXT ".dylib"
#else
#define CONFIG_SHEXT ".so"
#endif
#endif

#include <list.h>
#include <cutils.h>
#include "path.h"
#include "utils.h"
#include "vector.h"
#include <quickjs-libc.h>
#include "quickjs-internal.h"
#include "buffer-utils.h"
#include "base64.h"
#include "debug.h"

typedef JSModuleDef* ModuleInitFunction(JSContext*, const char*);
typedef char* ModuleLoader(JSContext*, const char*);

static thread_local int debug_module_loader = 0;
static thread_local Vector module_debug = VECTOR_INIT();
static thread_local Vector module_list = VECTOR_INIT();

#ifndef QUICKJS_MODULE_PATH
#ifdef CONFIG_PREFIX
#define QUICKJS_MODULE_PATH CONFIG_PREFIX "/lib/quickjs"
#endif
#endif

static const char jsm_default_module_path[] = QUICKJS_MODULE_PATH;

// static JSModuleLoaderFunc* module_loader = 0;
static thread_local JSValue package_json;
static thread_local const char* exename;
static thread_local JSRuntime* rt;
static thread_local JSContext* ctx;
static thread_local int interactive = 0;

static const char* const module_extensions[] = {
    CONFIG_SHEXT, ".js", "/index.js",
    //    "/package.json",
};

static inline BOOL
is_searchable(const char* path) {
  return !path_isexplicit(path);
}

static inline BOOL
has_dot_or_slash(const char* s) {
  return !!s[str_chrs(s, "." PATHSEP_S, 2)];
}

static char*
is_module(JSContext* ctx, const char* module_name) {
  BOOL yes = path_isfile1(module_name);

  if(debug_module_loader > 2)
    printf("%-18s(module_name=\"%s\")=%s\n", __FUNCTION__, module_name, ((yes) ? "TRUE" : "FALSE"));

  return yes ? js_strdup(ctx, module_name) : 0;
}

static int
module_has_suffix(const char* module_name) {
  size_t i, n;

  n = countof(module_extensions);
  for(i = 0; i < n; i++)
    if(has_suffix(module_name, module_extensions[i]))
      return strlen(module_name) - strlen(module_extensions[i]);

  return 0;
}

#ifdef HAVE_GET_MODULE_LOADER_FUNC
JSModuleLoaderFunc* js_std_get_module_loader_func();
void js_std_set_module_loader_func(JSModuleLoaderFunc* func);
#endif

#if !DONT_HAVE_MALLOC_USABLE_SIZE && !defined(ANDROID)
#if HAVE_MALLOC_USABLE_SIZE
#ifndef HAVE_MALLOC_USABLE_SIZE_DEFINITION
extern size_t malloc_usable_size();
#endif
#endif
#endif

#define trim_dotslash(str) (!strncmp((str), "./", 2) ? (str) + 2 : (str))

typedef struct {
  const char* module_name;
  JSModuleDef* (*module_func)(JSContext*, const char*);
  uint8_t* byte_code;
  uint32_t byte_code_len;
  JSModuleDef* def;
  BOOL initialized : 1;
} BuiltinModule;

#define jsm_module_extern_compiled(name) \
  extern const uint8_t qjsc_##name[]; \
  extern const uint32_t qjsc_##name##_size;

#define jsm_module_extern_native(name) extern JSModuleDef* js_init_module_##name(JSContext*, const char*)

#define jsm_module_record_compiled(name) \
  (BuiltinModule) { #name, 0, qjsc_##name, qjsc_##name##_size, 0 }

#define jsm_module_record_native(name) \
  (BuiltinModule) { #name, js_init_module_##name, 0, 0, 0 }

jsm_module_extern_native(std);
jsm_module_extern_native(os);
jsm_module_extern_native(child_process);
jsm_module_extern_native(deep);
jsm_module_extern_native(inspect);
jsm_module_extern_native(lexer);
jsm_module_extern_native(misc);
// jsm_module_extern_native(mmap);
jsm_module_extern_native(path);
jsm_module_extern_native(pointer);
jsm_module_extern_native(predicate);
jsm_module_extern_native(repeater);
jsm_module_extern_native(tree_walker);
jsm_module_extern_native(xml);

jsm_module_extern_compiled(console);
jsm_module_extern_compiled(events);
jsm_module_extern_compiled(fs);
jsm_module_extern_compiled(perf_hooks);
jsm_module_extern_compiled(process);
jsm_module_extern_compiled(repl);
jsm_module_extern_compiled(require);
jsm_module_extern_compiled(tty);
jsm_module_extern_compiled(util);

static thread_local Vector jsm_stack = VECTOR_INIT();
static thread_local Vector jsm_builtin_modules = VECTOR_INIT();
static thread_local BOOL jsm_modules_initialized;

#ifdef CONFIG_BIGNUM
jsm_module_extern_compiled(qjscalc);
static int bignum_ext = 1;
#endif

void js_std_set_worker_new_context_func(JSContext* (*func)(JSRuntime* rt));

static void
jsm_dump_error(JSContext* ctx) {
  js_error_print(ctx, JS_GetException(ctx));
}

enum {
  SCRIPT_LIST,
  SCRIPT_FILE,
  SCRIPT_FILENAME,
  SCRIPT_DIRNAME,
};

static char**
jsm_stack_ptr(int i) {
  int size;
  if((size = vector_size(&jsm_stack, sizeof(char*))) > 0) {
    if(i < 0)
      i += size;
    return vector_at(&jsm_stack, sizeof(char*), i);
  }

  return 0;
}

static char**
jsm_stack_find(const char* module) {
  char** ptr;

  if(jsm_stack.size == 0)
    return 0;

  vector_foreach_t(&jsm_stack, ptr) if(!path_compare2(*ptr, module)) return ptr;
  return 0;
}

static char*
jsm_stack_at(int i) {
  char** ptr;
  if((ptr = jsm_stack_ptr(i)))
    return *ptr;
  return 0;
}

static char*
jsm_stack_top() {
  return jsm_stack_at(-1);
}

static size_t
jsm_stack_count() {
  return vector_size(&jsm_stack, sizeof(char*));
}

static char*
jsm_stack_string() {
  static DynBuf buf;
  char** ptr;
  int i = jsm_stack_count();

  if(buf.buf == 0)
    dbuf_init2(&buf, 0, vector_realloc);

  buf.size = 0;
  while(--i >= 0) { dbuf_printf(&buf, "%i: %s\n", i, jsm_stack_at(i)); }

  dbuf_0(&buf);
  return (char*)buf.buf;
}

static JSValue
jsm_stack_get(JSContext* ctx, JSValueConst this_val, int magic) {
  JSValue ret = JS_UNDEFINED;

  switch(magic) {
    case SCRIPT_LIST: {
      char** ptr;
      size_t i = 0;

      ret = JS_NewArray(ctx);
      vector_foreach_t(&jsm_stack, ptr) {
        JSValue str = JS_NewString(ctx, *ptr);
        JS_SetPropertyUint32(ctx, ret, i++, str);
      }

      break;
    }

    case SCRIPT_FILE:
    case SCRIPT_FILENAME: {
      char* file;

      if((file = jsm_stack_top())) {
        char* abs = path_absolute1(file);
        ret = JS_NewString(ctx, abs);
        free(abs);
      }

      break;
    }

    case SCRIPT_DIRNAME: {
      char* file;

      if((file = jsm_stack_top())) {
        char* abs = path_absolute1(file);
        char* dir;
        if((dir = path_dirname1(abs))) {
          ret = JS_NewString(ctx, dir);
          free(dir);
        }

        free(abs);
      }

      break;
    }
  }
  return ret;
}

static void
jsm_stack_push(JSContext* ctx, const char* file) {
  vector_putptr(&jsm_stack, js_strdup(ctx, file));
}

static void
jsm_stack_pop(JSContext* ctx) {
  char** ptr;

  ptr = vector_back(&jsm_stack, sizeof(char*));
  js_free(ctx, *ptr);
  vector_pop(&jsm_stack, sizeof(char*));
}

static int
jsm_stack_load(JSContext* ctx, const char* file, BOOL module, BOOL is_main) {
  JSValue val;
  int32_t ret;
  JSValue global_obj = JS_GetGlobalObject(ctx);

  JS_SetPropertyStr(ctx, global_obj, "module", JS_NewObject(ctx));
  jsm_stack_push(ctx, file);

  errno = 0;
  val = js_eval_file(ctx, file, module ? JS_EVAL_TYPE_MODULE : 0);

  if(vector_size(&jsm_stack, sizeof(char*)) > 1)
    jsm_stack_pop(ctx);

  if(JS_IsException(val)) {
    JSValue stack = JS_IsObject(ctx->rt->current_exception) ? JS_GetPropertyStr(ctx, ctx->rt->current_exception, "stack") : JS_UNDEFINED;
    const char* msg = JS_ToCString(ctx, ctx->rt->current_exception);
    const char* st = JS_ToCString(ctx, stack);
    fprintf(stderr, "Error evaluating '%s': %s (%s)\n", file, msg, js_value_typestr(ctx, stack));

    if(st) {
      if(*st)
        fprintf(stderr, "Stack:\n%s\n", st);
      JS_FreeCString(ctx, st);
    }
    if(msg)
      JS_FreeCString(ctx, msg);

    js_error_print(ctx, ctx->rt->current_exception);
    return -1;
  }

  if(JS_IsModule(val) || module) {
    if(!JS_IsModule(val)) {
      JSModuleDef* m = js_module_at(ctx, -1);
      val = module_value(ctx, m);
    }

    module_exports_get(ctx, JS_VALUE_GET_PTR(val), TRUE, global_obj);
  } else {
    JS_ToInt32(ctx, &ret, val);
  }

  if(!JS_IsModule(val))
    JS_FreeValue(ctx, val);

  JS_FreeValue(ctx, global_obj);
  return 0;
}

JSModuleDef* js_init_module_deep(JSContext*, const char*);
JSModuleDef* js_init_module_inspect(JSContext*, const char*);
JSModuleDef* js_init_module_lexer(JSContext*, const char*);
JSModuleDef* js_init_module_misc(JSContext*, const char*);
JSModuleDef* js_init_module_path(JSContext*, const char*);
JSModuleDef* js_init_module_pointer(JSContext*, const char*);
JSModuleDef* js_init_module_predicate(JSContext*, const char*);
JSModuleDef* js_init_module_repeater(JSContext*, const char*);
JSModuleDef* js_init_module_tree_walker(JSContext*, const char*);
JSModuleDef* js_init_module_xml(JSContext*, const char*);

void
jsm_init_modules(JSContext* ctx) {
  if(jsm_modules_initialized)
    return;

  jsm_modules_initialized = TRUE;

  dbuf_init2(&jsm_builtin_modules.dbuf, 0, &vector_realloc);

#define jsm_builtin_native(name) vector_push(&jsm_builtin_modules, jsm_module_record_native(name));

  jsm_builtin_native(std);
  jsm_builtin_native(os);
  jsm_builtin_native(child_process);
  jsm_builtin_native(deep);
  jsm_builtin_native(inspect);
  jsm_builtin_native(lexer);
  jsm_builtin_native(misc);
  // jsm_builtin_native(mmap);
  jsm_builtin_native(path);
  jsm_builtin_native(pointer);
  jsm_builtin_native(predicate);
  jsm_builtin_native(repeater);
  jsm_builtin_native(tree_walker);
  jsm_builtin_native(xml);

#define jsm_builtin_compiled(name) vector_push(&jsm_builtin_modules, jsm_module_record_compiled(name));

  jsm_builtin_compiled(console);
  jsm_builtin_compiled(events);
  jsm_builtin_compiled(fs);
  jsm_builtin_compiled(perf_hooks);
  jsm_builtin_compiled(process);
  jsm_builtin_compiled(repl);
  jsm_builtin_compiled(require);
  jsm_builtin_compiled(tty);
  jsm_builtin_compiled(util);
}

static BuiltinModule*
jsm_builtin_find(const char* name) {
  BuiltinModule* rec;

  vector_foreach_t(&jsm_builtin_modules, rec) if(!strcmp(rec->module_name, name)) return rec;

  return 0;
}

static JSModuleDef*
jsm_builtin_init(JSContext* ctx, BuiltinModule* rec) {
  JSModuleDef* m;
  JSValue ret = JS_UNINITIALIZED, obj = JS_UNDEFINED;

  jsm_stack_push(ctx, rec->module_name);

  if(rec->def == 0) {
    if(debug_module_loader >= 2)
      printf("(3) %-30s internal\n", rec->module_name);

    /* C native module */
    if(rec->module_func) {
      m = rec->module_func(ctx, rec->module_name);
      obj = js_value_mkptr(JS_TAG_MODULE, m);

      if(!rec->initialized && !JS_IsUndefined(obj)) {
        JSValue func_obj = JS_DupValue(ctx, obj);
        JS_EvalFunction(ctx, func_obj);
        rec->initialized = TRUE;
      }

      /* bytecode compiled module */
    } else {

      obj = JS_ReadObject(ctx, rec->byte_code, rec->byte_code_len, JS_READ_OBJ_BYTECODE);

      m = js_value_ptr(obj);

      JS_ResolveModule(ctx, obj);
      ret = JS_EvalFunction(ctx, obj);

      /* rename module */
      JS_FreeAtom(ctx, m->module_name);
      m->module_name = JS_NewAtom(ctx, rec->module_name);
    }

    rec->def = m;
  }

  jsm_stack_pop(ctx);

  return rec->def;
}

static JSValue
jsm_load_json(JSContext* ctx, const char* file) {
  uint8_t* buf;
  size_t len;
  if(!(buf = js_load_file(ctx, &len, file)))
    return JS_ThrowInternalError(ctx, "Loading '%s' failed", file);
  return JS_ParseJSON(ctx, buf, len, file);
}

static JSValue
jsm_load_package(JSContext* ctx, const char* file) {
  if(JS_IsUndefined(package_json) || JS_VALUE_GET_TAG(package_json) == 0) {
    package_json = jsm_load_json(ctx, file ? file : "package.json");

    if(JS_IsException(package_json)) {
      JS_GetException(ctx);
      package_json = JS_NULL;
    }
  }
  return package_json;
}

static char*
jsm_search_list(JSContext* ctx, const char* module_name, const char* list) {
  const char* s;
  char* t = 0;
  size_t i, j = strlen(module_name);

  if(debug_module_loader >= 2)
    printf("%-18s(module_name=\"%s\" list =\"%s\")\n", __FUNCTION__, module_name, list);

  if(!(t = js_malloc(ctx, strlen(list) + 1 + strlen(module_name) + 1)))
    return 0;

  for(s = list; *s; s += i) {
    if((i = str_chrs(s, ";:\n", 3)) == 0)
      break;
    strncpy(t, s, i);
    t[i] = '/';
    strcpy(&t[i + 1], module_name);
    if(path_isfile1(t))
      return t;
    if(s[i])
      ++i;
  }

  return 0;
}

static char*
jsm_search_path(JSContext* ctx, const char* module_name) {
  const char* list;

  if(debug_module_loader >= 2)
    printf("%-18s(module_name=\"%s\")\n", __FUNCTION__, module_name);

  assert(is_searchable(module_name));

  if(!(list = getenv("QUICKJS_MODULE_PATH")))
    list = jsm_default_module_path;

  return jsm_search_list(ctx, module_name, list);
}

static char*
jsm_search_suffix(JSContext* ctx, const char* module_name, ModuleLoader* fn) {
  size_t i, n, len = strlen(module_name);
  char *s, *t = 0;

  if(debug_module_loader > 3)
    printf("%-18s(module_name=\"%s\", fn=%s)\n",
           __FUNCTION__,
           module_name,
           fn == &is_module         ? "is_module"
           : fn == &jsm_search_path ? "jsm_search_path"
                                    : "<unknown>");

  if(!(s = js_mallocz(ctx, (len + 31) & (~0xf))))
    return 0;

  strcpy(s, module_name);
  n = countof(module_extensions);
  for(i = 0; i < n; i++) {
    s[len] = '\0';
    if(has_suffix(s, module_extensions[i]))
      continue;
    strcat(s, module_extensions[i]);

    if((t = fn(ctx, s)))
      break;
  }

  js_free(ctx, s);
  return t;
}

static char*
jsm_search_module(JSContext* ctx, const char* module_name) {
  char* s = 0;
  BOOL search = is_searchable(module_name);
  BOOL suffix = module_has_suffix(module_name);
  ModuleLoader* fn = search ? &jsm_search_path : &is_module;

  s = suffix ? fn(ctx, module_name) : jsm_search_suffix(ctx, module_name, fn);

  if(debug_module_loader >= 2)
    printf("%-18s(module_name=\"%s\") search=%s suffix=%s fn=%s result=%s\n",
           __FUNCTION__,
           module_name,
           ((search) ? "TRUE" : "FALSE"),
           ((suffix) ? "TRUE" : "FALSE"),
           search ? "search_module" : "is_module",
           s);

  return s;
}

/* end of "new breed" module loader functions */

char*
jsm_module_package(JSContext* ctx, const char* module) {
  JSValue package;
  char *rel, *file = 0;

  rel = path_isabsolute1(module) ? path_relative1(module) : strdup(module);

  if(!has_suffix(module, CONFIG_SHEXT)) {
    package = jsm_load_package(ctx, "package.json");

    if(JS_IsObject(package)) {
      JSValue aliases, target = JS_UNDEFINED;

      aliases = JS_GetPropertyStr(ctx, package, "_moduleAliases");
      if(!JS_IsException(aliases) && JS_IsObject(aliases)) {
        target = JS_GetPropertyStr(ctx, aliases, path_trimdotslash1(rel));

        if(!JS_IsUndefined(target)) {
          file = js_tostring(ctx, target);

          if(debug_module_loader >= 1)
            printf("(2) %-30s => %s (package.json)\n", module, file);
        }
      }
      JS_FreeValue(ctx, aliases);
      JS_FreeValue(ctx, target);
    }
  }
  free(rel);

  return file;
}

void
jsm_module_script(DynBuf* buf, const char* path, const char* name, BOOL star) {
  enum { NAMED = 0, ALL, EXEC } mode = NAMED;

  for(; *path; ++path) {
    switch(*path) {
      case '!':
        if(!star)
          mode = EXEC;
        continue;
      case '*':
        if(!name)
          mode = ALL;
        continue;
    }
    break;
  }

  buf->size = 0;

  dbuf_putstr(buf, "import ");
  if(star)
    dbuf_putstr(buf, "* as ");
  dbuf_putstr(buf, "tmp from '");
  dbuf_putstr(buf, path);
  dbuf_putstr(buf, "';\n");

  switch(mode) {
    case EXEC: {
      dbuf_putstr(buf, "tmp();\n");
      break;
    }

    case ALL: {
      dbuf_putstr(buf, "Object.assign(globalThis, tmp);\n");
      break;
    }

    default: {
      size_t len = 0;
      char* tmp;

      if(!name)
        name = basename(path);

      if((tmp = strrchr(name, '.')))
        len = tmp - name;
      else
        len = strlen(name);

      dbuf_putstr(buf, "globalThis['");
      if(len)
        dbuf_put(buf, name, len);
      else
        dbuf_putstr(buf, name);
      dbuf_putstr(buf, "'] = tmp;\n");
      break;
    }
  }

  dbuf_0(buf);
}

static JSModuleDef*
jsm_module_find(JSContext* ctx, const char* name, int start_pos) {
  JSModuleDef* m;
  BuiltinModule* bltin;

  while(*name == '!' || *name == '*') ++name;

  if((m = js_module_find_from(ctx, name, start_pos)))
    return m;

  /*if((bltin = jsm_builtin_find(name))) {
    if(bltin->def)
      return bltin->def;
  }*/

  return 0;
}

static JSModuleDef*
jsm_module_load(JSContext* ctx, const char* path, const char* name) {
  struct list_head* last_module = js_modules_list(ctx)->prev;
  DynBuf dbuf;

  dbuf_init2(&dbuf, 0, 0);

  jsm_module_script(&dbuf, path, name, FALSE);

  if(*path != '*' && !js_eval_str(ctx, dbuf.buf, "<internal>", JS_EVAL_TYPE_MODULE)) {
  } else {
    JS_GetException(ctx);

    jsm_module_script(&dbuf, path, name, TRUE);

    if(js_eval_str(ctx, dbuf.buf, "<internal>", JS_EVAL_TYPE_MODULE)) {
      dbuf_free(&dbuf);
      return 0;
    }
  }

  dbuf_free(&dbuf);

  if(last_module->next == js_modules_list(ctx))
    return 0;

  assert(last_module->next != js_modules_list(ctx));

  JSModuleDef* m = last_module->next->next != js_modules_list(ctx) ? list_entry(last_module->next->next, JSModuleDef, link) : jsm_module_find(ctx, path, 0);
  return m;
}

JSModuleDef*
jsm_module_json(JSContext* ctx, const char* name) {
  DynBuf db;
  JSValue ret;
  JSModuleDef* m = 0;
  uint8_t* ptr;
  size_t len, i;

  if(!(ptr = js_load_file(ctx, &len, name)))
    return 0;

  js_dbuf_init(ctx, &db);
  dbuf_putstr(&db, "export default ");

  i = scan_whitenskip(ptr, len);

  dbuf_put(&db, ptr + i, len - i);
  js_free(ctx, ptr);
  dbuf_0(&db);

  ret = JS_Eval(ctx, db.buf, db.size, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if(JS_VALUE_GET_TAG(ret) == JS_TAG_MODULE)
    m = JS_VALUE_GET_PTR(ret);
  JS_FreeValue(ctx, ret);

  dbuf_free(&db);
  return m;
}

char*
jsm_module_locate(JSContext* ctx, const char* module_name, void* opaque) {
  char *file = 0, *s = 0;
  JSModuleDef* m = 0;

  s = js_strdup(ctx, module_name);

  for(;;) {
    if(debug_module_loader - !strcmp(module_name, s) >= 3)
      printf("%-18s[1](module_name=\"%s\", opaque=%p) s=%s\n", __FUNCTION__, module_name, opaque, s);

    if(has_dot_or_slash(s))
      if(path_isfile1(s))
        break;

    if(is_searchable(s)) {
      size_t len;
      if((file = jsm_search_module(ctx, s))) {
        js_free(ctx, s);
        s = js_strdup(ctx, file);
        break;
      }

      /*if(path_component1(s) == 3 && !strncmp(s, "lib", 3)) {
        strcpy(s, &s[3 + path_separator1(&s[3])]);
        continue;
      }*/

    } else {
      if((file = jsm_search_suffix(ctx, s, is_module))) {
        js_free(ctx, s);
        s = js_strdup(ctx, file);
        break;
      }
    }
    break;
  }

  return s;
}

JSModuleDef*
jsm_module_loader(JSContext* ctx, const char* module_name, void* opaque) {
  const char* name = module_name;
  char* s = 0;
  JSModuleDef* m = 0;

  if(str_start(module_name, "file://"))
    module_name += 7;

  if(str_start(module_name, "data:")) {
    char* decoded;
    JSValue module;
    size_t length = strlen(module_name);
    size_t offset = byte_chr(module_name, length, ',');

    if(!module_name[offset])
      return NULL;

    ++offset;
    length -= offset;

    if(!(decoded = js_malloc(ctx, b64url_get_decoded_buffer_size(length) + 1)))
      return 0;

    length = b64url_decode((const uint8_t*)&module_name[offset], length, decoded);
    decoded[length] = '\0';

    module = JS_Eval(ctx, decoded, length, "<data-url>", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    js_free(ctx, decoded);

    if(!JS_IsException(module)) {
      js_module_set_import_meta(ctx, module, FALSE, FALSE);
      m = JS_VALUE_GET_PTR(module);

      JS_FreeAtom(ctx, m->module_name);
      m->module_name = JS_NewAtomLen(ctx, name, offset - 1);
    }

    JS_FreeValue(ctx, module);
    return m;
  }

restart:
  if(jsm_stack_find(module_name) != 0) {
    printf("\x1b[1;31mWARNING: circular module dependency '%s' from:\n%s\x1b[0m\n", module_name, jsm_stack_string());
    exit(1);
  }

  if(!module_name[str_chrs(module_name, PATHSEP_S "/", 2)]) {
    BuiltinModule* rec;
    if((rec = jsm_builtin_find(module_name))) {
      if(s)
        js_free(ctx, s);
      return jsm_builtin_init(ctx, rec);
    }
  }

  if(s == 0) {
    {
      if(!s)
        if((s = jsm_module_package(ctx, module_name))) {

          if(is_searchable(s)) {
            BuiltinModule* rec;
            if((rec = jsm_builtin_find(s))) {
              free(s);
              return jsm_builtin_init(ctx, rec);
            }
          }
        }

      if(!s)
        s = js_strdup(ctx, module_name);

      if(s && is_searchable(s)) {
        char* tmp;
        if((tmp = jsm_module_locate(ctx, s, opaque))) {
          js_free(ctx, s);
          s = tmp;
        }
      }

      if(!s)
        s = jsm_module_locate(ctx, module_name, opaque);

      if(s) {
        module_name = s;
        goto restart;
      }
    }
  }

  if(s) {
    if(debug_module_loader >= 1)
      printf("%s \"%s\" -> \"%s\"\n", __FUNCTION__, name, s);

    jsm_stack_push(ctx, s);
    if(str_ends(s, ".json"))
      m = jsm_module_json(ctx, s);
    else
      m = js_module_loader(ctx, s, opaque);
    js_free(ctx, s);
    jsm_stack_pop(ctx);

  } else {
    if(debug_module_loader)
      printf("%s \"%s\" -> null\n", __FUNCTION__, module_name);
  }

  return m;
}

char*
jsm_module_normalize(JSContext* ctx, const char* path, const char* name, void* opaque) {
  char* file = 0;
  BuiltinModule* bltin = 0;

  if(!has_dot_or_slash(name) && (bltin = jsm_builtin_find(name))) {

    if(bltin->def)
      file = js_atom_tostring(ctx, bltin->def->module_name);

  } else {
    if(path[0] != '<' && (path_isdotslash(name) || path_isdotdot(name)) && has_dot_or_slash(name)) {
      DynBuf dir;
      size_t dsl;
      js_dbuf_allocator(ctx, &dir);

      if(path_isimplicit(path))
        dbuf_putstr(&dir, "." PATHSEP_S);

      path_concat5(path, path_dirlen1(path), name, strlen(name), &dir);
      dsl = path_skipdotslash2(dir.buf, dir.size);

      /* XXX BUG: should use path_normalize* to resolve symlinks */
      path_normalize2(dir.buf + dsl, dir.size - dsl);
      file = dir.buf;
    } else if(has_suffix(name, CONFIG_SHEXT) && !path_isabsolute1(name)) {
      DynBuf db;
      js_dbuf_init(ctx, &db);

      path_concat3(QUICKJS_C_MODULE_DIR, name, &db);
      file = (char*)db.buf;
    } else if(has_dot_or_slash(name) && path_exists1(name) && path_isrelative(name)) {
      file = path_absolute1(name);
      path_normalize1(file);
    }
  }

  if(!bltin && has_dot_or_slash(name) && !module_has_suffix(name)) {
    char* tmp;
    if((tmp = jsm_search_suffix(ctx, file ? file : name, &is_module))) {
      if(file)
        js_free(ctx, file);
      file = tmp;
    }
  }

  if(file == 0)
    file = js_strdup(ctx, name);

  if(debug_module_loader >= 1)
    printf("%s %s: \"%s\" => \"%s\"\n", __FUNCTION__, path, name, file);

  return file;
}

static void
jsm_module_save(void) {
  char* home = path_gethome1(getuid());
  DynBuf db;
  FILE* f;

  dbuf_init2(&db, 0, 0);
  path_concat3(home, "/.qjsm_modules", &db);

  if((f = fopen(db.buf, "w"))) {
    char** ptr;
    JSModuleDef* m;

    vector_foreach_t(&module_list, ptr) {
      char* name = *ptr;

      fputs(name, f);
      fputs("\n", f);
    }

    fclose(f);
  }

  dbuf_free(&db);
}

static void
jsm_module_restore(void) {
  char* home = path_gethome1(getuid());
  DynBuf db;
  dbuf_init2(&db, 0, 0);
  path_concat3(home, "/.qjsm_modules", &db);

  FILE* f;
  char buf[1024];

  if((f = fopen(db.buf, "r"))) {
    char* s;

    while((s = fgets(buf, sizeof(buf), f))) {
      buf[str_chrs(s, "\r\n", 2)] = '\0';
      if(vector_finds(&module_list, buf) == -1)
        vector_pushstring(&module_list, s);
    }

    fclose(f);
  }

  dbuf_free(&db);
}

/* also used to initialize the worker context */
static JSContext*
jsm_context_new(JSRuntime* rt) {
  JSContext* ctx;

  ctx = JS_NewContext(rt);
  if(!ctx)
    return 0;
#ifdef CONFIG_BIGNUM
  if(bignum_ext) {
    JS_AddIntrinsicBigFloat(ctx);
    JS_AddIntrinsicBigDecimal(ctx);
    JS_AddIntrinsicOperators(ctx);
    JS_EnableBignumExt(ctx, TRUE);
  }

#endif

  jsm_init_modules(ctx);

#define jsm_module_native(name) js_init_module_##name(ctx, #name);

  /* jsm_module_native(std);
   jsm_module_native(os);
 #ifndef __wasi__
   //jsm_module_native(child_process);
 #endif
   jsm_module_native(deep);
   jsm_module_native(inspect);
   jsm_module_native(lexer);
   jsm_module_native(misc);
 #ifndef __wasi__
   //jsm_module_native(mmap);
 #endif
   jsm_module_native(path);
   jsm_module_native(pointer);
   jsm_module_native(predicate);
   jsm_module_native(repeater);
   jsm_module_native(tree_walker);
   jsm_module_native(xml);*/

  // printf("Set module loader (rt=%p): %p\n", rt);

  return ctx;
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD 0
#else
#define MALLOC_OVERHEAD 8
#endif

struct trace_malloc_data {
  uint8_t* base;
};

static inline unsigned long long
jsm_trace_malloc_ptr_offset(uint8_t* ptr, struct trace_malloc_data* dp) {
  return ptr - dp->base;
}

/* default memory allocation functions with memory limitation */
static inline size_t
jsm_trace_malloc_usable_size(void* ptr) {
#if defined(__APPLE__)
  return malloc_size(ptr);
#elif defined(_WIN32)
  return _msize(ptr);
#elif defined(EMSCRIPTEN) || defined(__dietlibc__) || defined(__MSYS__) || defined(ANDROID) || defined(DONT_HAVE_MALLOC_USABLE_SIZE)
  return 0;
#elif defined(__linux__) || defined(HAVE_MALLOC_USABLE_SIZE)
  return malloc_usable_size(ptr);
#else
#warning change this to `return 0;` if compilation fails
  /* change this to `return 0;` if compilation fails */
  return malloc_usable_size(ptr);
#endif
}

static void
#ifdef _WIN32
    /* mingw printf is used */
    __attribute__((format(gnu_printf, 2, 3)))
#else
    __attribute__((format(printf, 2, 3)))
#endif
    jsm_trace_malloc_printf(JSMallocState* s, const char* fmt, ...) {
  va_list ap;
  int c;

  va_start(ap, fmt);
  while((c = *fmt++) != '\0') {
    if(c == '%') {
      /* only handle %p and %zd */
      if(*fmt == 'p') {
        uint8_t* ptr = va_arg(ap, void*);
        if(ptr == 0) {
          printf("0");
        } else {
          printf("H%+06lld.%zd", jsm_trace_malloc_ptr_offset(ptr, s->opaque), jsm_trace_malloc_usable_size(ptr));
        }

        fmt++;
        continue;
      }

      if(fmt[0] == 'z' && fmt[1] == 'd') {
        size_t sz = va_arg(ap, size_t);
        printf("%zd", sz);
        fmt += 2;
        continue;
      }
    }
    putc(c, stdout);
  }

  va_end(ap);
}

static void
jsm_trace_malloc_init(struct trace_malloc_data* s) {
  free(s->base = malloc(8));
}

static void*
jsm_trace_malloc(JSMallocState* s, size_t size) {
  void* ptr;

  /* Do not allocate zero bytes: behavior is platform dependent */
  assert(size != 0);

  if(unlikely(s->malloc_size + size > s->malloc_limit))
    return 0;

  ptr = malloc(size);
  jsm_trace_malloc_printf(s, "A %zd -> %p\n", size, ptr);

  if(ptr) {
    s->malloc_count++;
    s->malloc_size += jsm_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  }

  return ptr;
}

static void
jsm_trace_free(JSMallocState* s, void* ptr) {
  if(!ptr)
    return;

  jsm_trace_malloc_printf(s, "F %p\n", ptr);
  s->malloc_count--;
  s->malloc_size -= jsm_trace_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  free(ptr);
}

static void*
jsm_trace_realloc(JSMallocState* s, void* ptr, size_t size) {
  size_t old_size;

  if(!ptr) {
    if(size == 0)
      return 0;
    return jsm_trace_malloc(s, size);
  }

  old_size = jsm_trace_malloc_usable_size(ptr);

  if(size == 0) {
    jsm_trace_malloc_printf(s, "R %zd %p\n", size, ptr);
    s->malloc_count--;
    s->malloc_size -= old_size + MALLOC_OVERHEAD;
    free(ptr);
    return 0;
  }

  if(s->malloc_size + size - old_size > s->malloc_limit)
    return 0;

  jsm_trace_malloc_printf(s, "R %zd %p", size, ptr);

  ptr = realloc(ptr, size);
  jsm_trace_malloc_printf(s, " -> %p\n", ptr);

  if(ptr)
    s->malloc_size += jsm_trace_malloc_usable_size(ptr) - old_size;

  return ptr;
}

static const JSMallocFunctions trace_mf = {
    jsm_trace_malloc,
    jsm_trace_free,
    jsm_trace_realloc,
#if defined(__APPLE__)
    malloc_size,
#elif defined(_WIN32)
    (size_t(*)(const void*))_msize,
#elif defined(EMSCRIPTEN) || defined(__dietlibc__) || defined(__MSYS__) || defined(ANDROID) || defined(DONT_HAVE_MALLOC_USABLE_SIZE_DEFINITION)
    0,
#elif defined(__linux__) || defined(HAVE_MALLOC_USABLE_SIZE)
    (size_t(*)(const void*))malloc_usable_size,
#else
#warning change this to `0,` if compilation fails
    /* change this to `0,` if compilation fails */
    malloc_usable_size,
#endif
};

void
jsm_help(void) {
  printf("QuickJS version " CONFIG_VERSION "\n"
         "usage: %s [options] [file [args]]\n"
         "-h  --help         list options\n"
         "-e  --eval EXPR    evaluate EXPR\n"
         "-i  --interactive  go to interactive mode\n"
         "-m  --module NAME  load an ES6 module\n"
         "-I  --include file include an additional file\n"
         "    --std          make 'std' and 'os' available to the loaded script\n"
#ifdef CONFIG_BIGNUM
         "    --no-bignum    disable the bignum extensions (BigFloat, "
         "BigDecimal)\n"
         "    --qjscalc      load the QJSCalc runtime (default if invoked as "
         "qjscalc)\n"
#endif
         "-T  --trace        trace memory allocation\n"
         "-d  --dump         dump the memory usage stats\n"
         "    --memory-limit n       limit the memory usage to 'n' bytes\n"
         "    --stack-size n         limit the stack size to 'n' bytes\n"
         "    --unhandled-rejection  dump unhandled promise rejections\n"
         "-q  --quit         just instantiate the interpreter and quit\n"
#ifdef SIGUSR1
         "\n"
         "  USR1 signal starts interactive mode\n"
#endif
         ,
         exename);
  exit(1);
}

static JSValue
jsm_eval_script(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  const char* str;
  size_t len;
  JSValue ret;
  int32_t module;

  str = JS_ToCStringLen(ctx, &len, argv[0]);

  if(argc > 1)
    JS_ToInt32(ctx, &module, argv[1]);
  else
    module = str_ends(str, ".mjs");

  switch(magic) {
    case 0: {
      ret = js_eval_file(ctx, str, module ? JS_EVAL_TYPE_MODULE : 0);
      break;
    }

    case 1: {
      ret = js_eval_buf(ctx, str, len, 0, module ? JS_EVAL_TYPE_MODULE : 0);
      break;
    }
  }

  if(JS_IsException(ret))
    if(JS_IsNull(JS_GetRuntime(ctx)->current_exception))
      ret = JS_GetException(ctx);

  if(JS_VALUE_GET_TAG(ret) == JS_TAG_MODULE) {
    JSModuleDef* m = JS_VALUE_GET_PTR(ret);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "name", module_name(ctx, m));
    JS_SetPropertyStr(ctx, obj, "exports", module_exports(ctx, m));
    ret = obj;
  }

  JS_FreeCString(ctx, str);
  return ret;
}

enum {
  FIND_MODULE,
  FIND_MODULE_INDEX,
  LOAD_MODULE,
  ADD_MODULE,
  REQUIRE_MODULE,
  LOCATE_MODULE,
  NORMALIZE_MODULE,
  RESOLVE_MODULE,
  GET_MODULE_NAME,
  GET_MODULE_OBJECT,
  GET_MODULE_EXPORTS,
  GET_MODULE_IMPORTS,
  GET_MODULE_NAMESPACE,
  GET_MODULE_FUNCTION,
  GET_MODULE_EXCEPTION,
  GET_MODULE_META_OBJ,
};

static JSValue
jsm_module_func(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  JSValue val = JS_EXCEPTION;
  JSModuleDef* m = 0;
  const char* name = 0;

  if(magic >= RESOLVE_MODULE || (magic == NORMALIZE_MODULE && JS_IsModule(argv[0]))) {
    if(!(m = js_module_def(ctx, argv[0])))
      return JS_ThrowTypeError(ctx,
                               "%s: argument 1 expecting module",
                               ((const char* const[]){
                                   "normalizeModule",
                                   "resolveModule",
                                   "getModuleName",
                                   "getModuleObject",
                                   "getModuleExports",
                                   "getModuleNamespace",
                                   "getModuleFunction",
                                   "getModuleException",
                                   "getModuleMetaObject",
                               })[magic - NORMALIZE_MODULE]);
  } else {
    name = js_tostring(ctx, argv[0]);
  }

  if(magic == LOAD_MODULE || magic == REQUIRE_MODULE) {
    char* path;

    if((path = jsm_module_normalize(ctx, ".", name, 0))) {
      js_free(ctx, name);
      name = path;
    }
  }

  switch(magic) {
    case ADD_MODULE: {
      ssize_t i;
      if((i = vector_finds(&module_list, name)) == -1) {
        i = vector_size(&module_list, sizeof(char*));
        vector_pushstring(&module_list, name);
        // jsm_module_save();
      }

      val = JS_NewInt64(ctx, i);
      break;
    }

    case FIND_MODULE: {
      BuiltinModule* bltin;
      if((m = jsm_module_find(ctx, name, 0))) {
        val = module_value(ctx, m);
      } else {
        val = JS_NULL;
      }
      break;
    }

    case FIND_MODULE_INDEX: {
      int32_t start = 0, index;

      if(argc > 1)
        JS_ToInt32(ctx, &start, argv[1]);

      m = JS_IsModule(argv[0]) ? JS_VALUE_GET_PTR(argv[0]) : jsm_module_find(ctx, name, start);

      if((index = js_module_indexof(ctx, m)) != -1)
        if(start < 0)
          index -= list_size(&ctx->loaded_modules);

      val = JS_NewInt32(ctx, index);

      break;
    }

    case LOAD_MODULE: {
      const char* key = 0;
      if(argc > 1)
        key = JS_ToCString(ctx, argv[1]);

      if((m = jsm_module_load(ctx, name, key)))
        val = module_value(ctx, m);
      else
        val = JS_ThrowInternalError(ctx, "Failed loading module '%s'", name);

      if(key)
        JS_FreeCString(ctx, key);

      break;
    }

    case REQUIRE_MODULE: {
      if((m = jsm_module_loader(ctx, name, NULL)))
        val = module_exports(ctx, m);
      break;
    }

    case LOCATE_MODULE: {
      char* s;
      if((s = jsm_module_locate(ctx, name, 0))) {
        val = JS_NewString(ctx, s);
        js_free(ctx, s);
      }

      break;
    }

    case NORMALIZE_MODULE: {
      char *path, *file, *name;
      path = m ? module_namecstr(ctx, m) : JS_ToCString(ctx, argv[0]);
      name = JS_ToCString(ctx, argv[1]);

      if((file = jsm_module_normalize(ctx, path, name, 0))) {
        val = JS_NewString(ctx, file);
        js_free(ctx, file);
      }

      JS_FreeCString(ctx, path);
      JS_FreeCString(ctx, name);
      break;
    }

    case RESOLVE_MODULE: {
      val = JS_NewInt32(ctx, JS_ResolveModule(ctx, JS_MKPTR(JS_TAG_MODULE, m)));
      break;
    }

    case GET_MODULE_NAME: {
      val = module_name(ctx, m);
      break;
    }

    case GET_MODULE_OBJECT: {
      val = module_object(ctx, m);
      break;
    }

    case GET_MODULE_IMPORTS: {
      val = module_imports(ctx, m);
      break;
    }

    case GET_MODULE_EXPORTS: {
      val = module_exports(ctx, m);
      break;
    }

    case GET_MODULE_NAMESPACE: {
      val = JS_DupValue(ctx, m->module_ns);
      break;
    }

    case GET_MODULE_FUNCTION: {
      val = module_func(ctx, m);
      break;
    }

    case GET_MODULE_EXCEPTION: {
      if(m->eval_has_exception)
        val = JS_DupValue(ctx, m->eval_exception);
      else
        val = JS_NULL;
      break;
    }

    case GET_MODULE_META_OBJ: {
      val = JS_DupValue(ctx, m->meta_obj);
      break;
    }
  }
  if(name)
    js_free(ctx, name);

  return val;
}

static const JSCFunctionListEntry jsm_global_funcs[] = {
    JS_CFUNC_MAGIC_DEF("evalFile", 1, jsm_eval_script, 0),
    JS_CFUNC_MAGIC_DEF("evalScript", 1, jsm_eval_script, 1),
    JS_CGETSET_MAGIC_DEF("moduleList", js_modules_array, 0, 0),
    JS_CGETSET_MAGIC_DEF("moduleObject", js_modules_object, 0, 0),
    JS_CGETSET_MAGIC_DEF("moduleMap", js_modules_map, 0, 0),
    JS_CGETSET_MAGIC_DEF("scriptList", jsm_stack_get, 0, SCRIPT_LIST),
    JS_CGETSET_MAGIC_DEF("scriptFile", jsm_stack_get, 0, SCRIPT_FILE),
    JS_CGETSET_MAGIC_DEF("scriptDir", jsm_stack_get, 0, SCRIPT_DIRNAME),
    JS_CGETSET_MAGIC_DEF("__filename", jsm_stack_get, 0, SCRIPT_FILENAME),
    JS_CGETSET_MAGIC_DEF("__dirname", jsm_stack_get, 0, SCRIPT_DIRNAME),
    JS_CFUNC_MAGIC_DEF("findModule", 1, jsm_module_func, FIND_MODULE),
    JS_CFUNC_MAGIC_DEF("findModuleIndex", 1, jsm_module_func, FIND_MODULE_INDEX),
    JS_CFUNC_MAGIC_DEF("loadModule", 1, jsm_module_func, LOAD_MODULE),
    JS_CFUNC_MAGIC_DEF("addModule", 1, jsm_module_func, ADD_MODULE),
    JS_CFUNC_MAGIC_DEF("resolveModule", 1, jsm_module_func, RESOLVE_MODULE),
    JS_CFUNC_MAGIC_DEF("requireModule", 1, jsm_module_func, REQUIRE_MODULE),
    JS_CFUNC_MAGIC_DEF("normalizeModule", 2, jsm_module_func, NORMALIZE_MODULE),
    JS_CFUNC_MAGIC_DEF("locateModule", 1, jsm_module_func, LOCATE_MODULE),
    JS_CFUNC_MAGIC_DEF("getModuleName", 1, jsm_module_func, GET_MODULE_NAME),
    JS_CFUNC_MAGIC_DEF("getModuleObject", 1, jsm_module_func, GET_MODULE_OBJECT),
    JS_CFUNC_MAGIC_DEF("getModuleExports", 1, jsm_module_func, GET_MODULE_EXPORTS),
    JS_CFUNC_MAGIC_DEF("getModuleImports", 1, jsm_module_func, GET_MODULE_IMPORTS),
    JS_CFUNC_MAGIC_DEF("getModuleNamespace", 1, jsm_module_func, GET_MODULE_NAMESPACE),
    JS_CFUNC_MAGIC_DEF("getModuleFunction", 1, jsm_module_func, GET_MODULE_FUNCTION),
    JS_CFUNC_MAGIC_DEF("getModuleException", 1, jsm_module_func, GET_MODULE_EXCEPTION),
    JS_CFUNC_MAGIC_DEF("getModuleMetaObject", 1, jsm_module_func, GET_MODULE_META_OBJ),
};

/*void
jsm_import_parse(ImportDirective* imp, const char* spec) {
  BOOL ns;
  size_t len, eqpos, dotpos;
  memset(imp, 0, sizeof(ImportDirective));

  len = strlen(spec);
  eqpos = str_chr(spec, '=');

  if(eqpos < len) {
    imp->var = str_ndup(spec, eqpos);
    spec += eqpos + 1;
    len -= eqpos + 1;
  }

  dotpos = str_rchr(spec, '.');
  ns = dotpos + 1 < len && spec[dotpos + 1] == '*';
  imp->path = str_ndup(spec, dotpos);
  imp->ns = basename(imp->path);

  if(dotpos < len && !ns) {
    imp->spec = strdup(&spec[dotpos + 1]);
    imp->prop = strdup(&spec[dotpos + 1]);
    imp->ns = 0;
  } else {
    imp->spec = dotpos < len ? (ns ? "*" : 0) : "default";
  }

}*/

static void
jsm_signal_handler(void) {
  interactive = TRUE;
}

static void
jsm_start_interactive(JSContext* ctx) {
  char str[1024];
  const char* home;

  /*  if(!JS_IsUndefined(replObj))
      return;*/

  home = getenv("HOME");

  /* clang-format off */
  snprintf(str,
    sizeof(str),
    "import { REPL } from 'repl';\n"
    "import * as fs from 'fs';\n"
    "const history = '%s/.%s_history';\n"
    "globalThis.repl = new REPL((__filename ?? '%s').replace(/.*\\//g, '').replace(/\\.js$/g, ''), false);\n"
    "repl.loadSaveOptions();\n"
    "repl.historyLoad(null, fs);\n"
    "repl.directives.i ??= [\n"
    "  name => import(name).then(m => {\n"
    "    let id = name.slice(name.lastIndexOf('/') + 1).replace(/\\.[^\\/.]+$/g, '');\n"
    "    globalThis[id] = m;\n"
    "  }).catch(() => repl.printStatus(`ERROR: module '${name}' not found\\n`))"
    ",\n"
    " 'import a module'\n"
    "];\n"
     "repl.run();\n",
    home, exename, exename);
  /* clang-format on */

  JSValue ret;

  // ret = js_eval_binary(ctx, qjsc_repl, qjsc_repl_size, 0);

  ret = js_eval_buf(ctx, str, strlen(str), "<init>", JS_EVAL_TYPE_MODULE);

  // JS_FreeValue(ctx,module);
}

int
main(int argc, char** argv) {
  struct trace_malloc_data trace_data = {0};
  int optind;
  char* expr = 0;
  int dump_memory = 0;
  int trace_memory = 0;
  int empty_run = 0;
  int module = 1;
  int load_std = 1;
  int dump_unhandled_promise_rejection = 0;
  size_t memory_limit = 0;
  char* include_list[32];
  int i, include_count = 0;
#ifdef CONFIG_BIGNUM
  int load_jscalc;
#endif
  size_t stack_size = 0;

  package_json = JS_UNDEFINED;
  // replObj = JS_UNDEFINED;

  {
    const char* p;
    exename = argv[0];
    p = strrchr(exename, '/');
    if(p)
      exename = p + 1;
    /* load jscalc runtime if invoked as 'qjscalc' */
    load_jscalc = !strcmp(exename, "qjscalc");
  }

  /* cannot use getopt because we want to pass the command line to the script */
  optind = 1;
  while(optind < argc && *argv[optind] == '-') {
    char* arg = argv[optind] + 1;
    const char* longopt = "";
    const char* optarg;

    /* a single - is not an option, it also stops argument scanning */
    if(!*arg)
      break;

    if(arg[1]) {
      optarg = &arg[1];
    } else {
      optarg = argv[++optind];
    }

    if(*arg == '-') {
      longopt = arg + 1;
      arg += strlen(arg);
      /* -- stops argument scanning */
      if(!*longopt)
        break;
    }

    for(; *arg || *longopt; longopt = "") {
      char opt = *arg;
      if(opt)
        arg++;
      if(opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
        jsm_help();
        continue;
      }

      if(opt == 'e' || !strcmp(longopt, "eval")) {
        if(*arg) {
          expr = arg;
          break;
        }

        if(optind < argc) {
          expr = argv[optind++];
          break;
        }

        fprintf(stderr, "%s: missing expression for -e\n", exename);
        exit(2);
      }

      if(opt == 'I' || !strcmp(longopt, "include")) {
        if(optind >= argc) {
          fprintf(stderr, "expecting filename");
          exit(1);
        }

        if(include_count >= countof(include_list)) {
          fprintf(stderr, "too many included files");
          exit(1);
        }

        include_list[include_count++] = optarg;
        break;
      }

      if(opt == 'i' || !strcmp(longopt, "interactive")) {
        interactive++;
        break;
      }

      if(opt == 'm' || !strcmp(longopt, "module")) {
        const char* modules = optarg;
        size_t i, len;

        for(i = 0; modules[i]; i += len) {
          len = str_chr(&modules[i], ',');
          vector_pushstringlen(&module_list, &modules[i], len);

          if(modules[i + len] == ',')
            len++;
        }

        break;
      }

      if(opt == 'd' || !strcmp(longopt, "dump")) {
        dump_memory++;
        break;
      }

      if(opt == 'T' || !strcmp(longopt, "trace")) {
        trace_memory++;
        break;
      }

      if(!strcmp(longopt, "std")) {
        load_std = 1;
        break;
      }

      if(!strcmp(longopt, "unhandled-rejection")) {
        dump_unhandled_promise_rejection = 1;
        break;
      }

#ifdef CONFIG_BIGNUM
      if(!strcmp(longopt, "no-bignum")) {
        bignum_ext = 0;
        break;
      }

      if(!strcmp(longopt, "bignum")) {
        bignum_ext = 1;
        break;
      }

      if(!strcmp(longopt, "qjscalc")) {
        load_jscalc = 1;
        break;
      }

#endif
      if(opt == 'q' || !strcmp(longopt, "quit")) {
        empty_run++;
        break;
      }

      if(!strcmp(longopt, "memory-limit")) {
        if(optind >= argc) {
          fprintf(stderr, "expecting memory limit");
          exit(1);
        }

        memory_limit = (size_t)strtod(argv[optind++], 0);
        break;
      }

      if(!strcmp(longopt, "stack-size")) {
        if(optind >= argc) {
          fprintf(stderr, "expecting stack size");
          exit(1);
        }

        stack_size = (size_t)strtod(argv[optind++], 0);
        break;
      }

      if(opt) {
        fprintf(stderr, "%s: unknown option '-%c'\n", exename, opt);
      } else {
        fprintf(stderr, "%s: unknown option '--%s'\n", exename, longopt);
      }

      jsm_help();
    }

    optind++;
  }

  jsm_init_modules(ctx);

#ifdef HAVE_GET_MODULE_LOADER_FUNC
  module_loader = js_std_get_module_loader_func();
#endif

  {
    const char* modules;

    if((modules = getenv("DEBUG"))) {
      size_t i, len;
      for(i = 0; modules[i]; i += len) {
        len = str_chr(&modules[i], ',');
        vector_putptr(&module_debug, str_ndup(&modules[i], len));

        if(modules[i + len] == ',')
          len++;
      }

      debug_module_loader = vector_counts(&module_debug, "modules");
    }
  }

  if(load_jscalc)
    bignum_ext = 1;

  if(trace_memory) {
    jsm_trace_malloc_init(&trace_data);
    rt = JS_NewRuntime2(&trace_mf, &trace_data);
  } else {
    rt = JS_NewRuntime();
  }

  if(!rt) {
    fprintf(stderr, "%s: cannot allocate JS runtime\n", exename);
    exit(2);
  }

  if(memory_limit != 0)
    JS_SetMemoryLimit(rt, memory_limit);

  JS_SetMaxStackSize(rt, stack_size != 0 ? stack_size : 256 * 1048576);

  js_std_set_worker_new_context_func(jsm_context_new);

  js_std_init_handlers(rt);

  /* loader for ES6 modules */
  JS_SetModuleLoaderFunc(rt, jsm_module_normalize, jsm_module_loader, 0);

  ctx = jsm_context_new(rt);
  if(!ctx) {
    fprintf(stderr, "%s: cannot allocate JS context\n", exename);
    exit(2);
  }

  vector_init(&jsm_stack, ctx);

  if(dump_unhandled_promise_rejection) {
    JS_SetHostPromiseRejectionTracker(rt, js_std_promise_rejection_tracker, 0);
  }

  if(!empty_run) {
    DynBuf db;
    js_dbuf_init(ctx, &db);

#ifdef CONFIG_BIGNUM
    if(load_jscalc) {
      js_eval_binary(ctx, qjsc_qjscalc, qjsc_qjscalc_size, 0);
    }

#endif
    js_std_add_helpers(ctx, argc - optind, argv + optind);

    dbuf_putstr(&db, "import process from 'process';\nglobalThis.process = process;\n");

    // dbuf_putstr(&db, "import require from 'require';\nglobalThis.require = require;\n");

    JS_SetPropertyFunctionList(ctx, JS_GetGlobalObject(ctx), jsm_global_funcs, countof(jsm_global_funcs));

    if(load_std) {
      const char* str = "import * as std from 'std';\nimport * as os from 'os';\nglobalThis.std = "
                        "std;\nglobalThis.os = os;\nglobalThis.setTimeout = "
                        "os.setTimeout;\nglobalThis.clearTimeout = os.clearTimeout;\n";
      dbuf_putstr(&db, str);
    }

    if(db.size) {
      dbuf_0(&db);
      js_eval_str(ctx, db.buf, 0, JS_EVAL_TYPE_MODULE);
    }

    dbuf_free(&db);

    {
      char** ptr;
      vector_foreach_t(&module_list, ptr) {
        JSModuleDef* m;
        if(!(m = jsm_module_load(ctx, *ptr, 0))) {
          jsm_dump_error(ctx);
          return 1;
        }
      }
      vector_freestrings(&module_list);
    }

    for(i = 0; i < include_count; i++) {
      if(jsm_stack_load(ctx, include_list[i], FALSE, FALSE) == -1)
        goto fail;
    }

    js_eval_str(ctx,
                "import { Console } from 'console';\n"
                "import { out } from 'std';\n"
                "globalThis.console = new Console(out, { inspectOptions: { customInspect: true } });\n",
                0,
                JS_EVAL_TYPE_MODULE);

    if(!interactive) {
#ifdef SIGUSR1
      signal(SIGUSR1, jsm_signal_handler);
#endif
    }

    if(expr) {
      if(js_eval_str(ctx, expr, "<cmdline>", 0) == -1)
        goto fail;
    } else if(optind >= argc) {
      /* interactive mode */
      interactive = 1;
    } else {
      const char* filename;
      filename = argv[optind];

      if(jsm_stack_load(ctx, filename, module, TRUE) == -1)
        goto fail;
    }

    if(interactive || getenv("INTERACTIVE"))
      jsm_start_interactive(ctx);

    js_std_loop(ctx);
  }

  if(!JS_IsNull(ctx->rt->current_exception))
    jsm_dump_error(ctx);

  if(dump_memory) {
    JSMemoryUsage stats;
    JS_ComputeMemoryUsage(rt, &stats);
    JS_DumpMemoryUsage(stdout, &stats, rt);
  }

  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);

  if(empty_run && dump_memory) {
    clock_t t[5];
    double best[5];
    int i, j;
    for(i = 0; i < 100; i++) {
      t[0] = clock();
      rt = JS_NewRuntime();
      t[1] = clock();
      ctx = JS_NewContext(rt);
      t[2] = clock();
      JS_FreeContext(ctx);
      t[3] = clock();
      JS_FreeRuntime(rt);
      t[4] = clock();
      for(j = 4; j > 0; j--) {
        double ms = 1000.0 * (t[j] - t[j - 1]) / CLOCKS_PER_SEC;
        if(i == 0 || best[j] > ms)
          best[j] = ms;
      }
    }
    printf("\nInstantiation times (ms): %.3f = %.3f+%.3f+%.3f+%.3f\n", best[1] + best[2] + best[3] + best[4], best[1], best[2], best[3], best[4]);
  }

  return 0;
fail:
  js_std_free_handlers(rt);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return 1;
}
