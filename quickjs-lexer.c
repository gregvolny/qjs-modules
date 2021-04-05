#define _GNU_SOURCE

#include "quickjs.h"
#include "libregexp.h"
#include "quickjs-lexer.h"
#include "vector.h"
#include <string.h>
#include <ctype.h>

#define ALPHA_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

static JSValue
js_position_tostring(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  int32_t line, column;
  DynBuf db;
  JSValue ret;
  line = js_object_propertystr_getint32(ctx, this_val, "line");
  column = js_object_propertystr_getint32(ctx, this_val, "column");

  dbuf_init2(&db, JS_GetRuntime(ctx), (DynBufReallocFunc*)js_realloc_rt);
  dbuf_printf(&db, "%" PRId32 ":%" PRId32, line, column);

  ret = JS_NewStringLen(ctx, db.buf, db.size);
  dbuf_free(&db);
  return ret;
}

static JSValue
js_position_new(JSContext* ctx, const Location* loc) {
  JSValue ret = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, ret, "line", JS_NewUint32(ctx, loc->line + 1));
  JS_SetPropertyStr(ctx, ret, "column", JS_NewUint32(ctx, loc->column + 1));

  JS_SetPropertyStr(ctx, ret, "toString", JS_NewCFunction(ctx, &js_position_tostring, "toString", 0));
  return ret;
}

VISIBLE JSClassID js_syntax_error_class_id;
JSValue syntax_error_proto, syntax_error_constructor, syntax_error_ctor;

JSValue
js_syntax_error_new(JSContext* ctx, SyntaxError arg) {
  SyntaxError* err;
  JSValue obj = JS_UNDEFINED;

  if(!(err = js_mallocz(ctx, sizeof(SyntaxError))))
    return JS_EXCEPTION;

  memcpy(err, &arg, sizeof(SyntaxError));

  obj = JS_NewObjectProtoClass(ctx, syntax_error_proto, js_syntax_error_class_id);
  JS_SetOpaque(obj, err);

  JS_SetPropertyStr(ctx, obj, "message", JS_NewString(ctx, err->message));

  return obj;
}

static JSValue
js_syntax_error_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
  SyntaxError* err;
  JSValue obj = JS_UNDEFINED;
  JSValue proto;

  if(!(err = js_mallocz(ctx, sizeof(SyntaxError))))
    return JS_EXCEPTION;

  /* using new_target to get the prototype is necessary when the
     class is extended. */
  proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if(JS_IsException(proto))
    goto fail;
  obj = JS_NewObjectProtoClass(ctx, proto, js_syntax_error_class_id);
  JS_FreeValue(ctx, proto);
  if(JS_IsException(obj))
    goto fail;
  JS_SetOpaque(obj, err);
  if(argc > 0)
    JS_SetPropertyStr(ctx, obj, "message", argv[0]);

  return obj;
fail:
  js_free(ctx, err);
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

static JSValue
js_syntax_error_get(JSContext* ctx, JSValueConst this_val, int magic) {
  SyntaxError* err;
  JSValue ret = JS_UNDEFINED;
  if(!(err = JS_GetOpaque2(ctx, this_val, js_syntax_error_class_id)))
    return JS_EXCEPTION;

  switch(magic) {
    case 0: {
      ret = js_position_new(ctx, &err->loc);
      break;
    }
  }
  return ret;
}

void
js_syntax_error_finalizer(JSRuntime* rt, JSValue val) {
  SyntaxError* err = JS_GetOpaque(val, js_syntax_error_class_id);
  if(err) {}
  // JS_FreeValueRT(rt, val);
}

JSClassDef js_syntax_error_class = {
    .class_name = "SyntaxError",
    .finalizer = js_syntax_error_finalizer,
};

static const JSCFunctionListEntry js_syntax_error_proto_funcs[] = {
    JS_CGETSET_ENUMERABLE_DEF("loc", js_syntax_error_get, 0, 0),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "SyntaxError", JS_PROP_CONFIGURABLE)};

VISIBLE JSClassID js_token_class_id;
JSValue token_proto, token_constructor, token_ctor;

enum token_methods { TO_STRING = 0 };
enum token_getters { PROP_LENGTH = 0, PROP_OFFSET, PROP_CHARS, PROP_LOC, PROP_ID, PROP_TYPE };

static inline int
keywords_cmp(const char** w1, const char** w2) {
  return strcmp(*w1, *w2);
}

static Line
lexer_line(Lexer* lex) {
  Line ret = {0, 0};
  const char* x = (const char*)lex->data;
  ret.start = lex->pos;
  while(ret.start > 0 && x[ret.start - 1] != '\n') ret.start--;
  ret.length = byte_chr(&x[ret.start], lex->size - ret.start, '\n');
  return ret;
}

JSValue
js_token_new(JSContext* ctx, Token arg) {
  Token* tok;
  JSValue obj = JS_UNDEFINED;

  if(!(tok = js_mallocz(ctx, sizeof(Token))))
    return JS_EXCEPTION;

  memcpy(tok, &arg, sizeof(Token));

  obj = JS_NewObjectProtoClass(ctx, token_proto, js_token_class_id);
  JS_SetOpaque(obj, tok);
  return obj;
}

static JSValue
js_token_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
  Token* tok;
  JSValue obj = JS_UNDEFINED;
  JSValue proto;

  if(!(tok = js_mallocz(ctx, sizeof(Token))))
    return JS_EXCEPTION;

  /* using new_target to get the prototype is necessary when the
     class is extended. */
  proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if(JS_IsException(proto))
    goto fail;
  obj = JS_NewObjectProtoClass(ctx, proto, js_token_class_id);
  JS_FreeValue(ctx, proto);
  if(JS_IsException(obj))
    goto fail;
  JS_SetOpaque(obj, tok);

  return obj;
fail:
  js_free(ctx, tok);
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

static JSValue
js_token_tostring(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  Token* tok;
  DynBuf dbuf;
  JSValue ret;
  if(!(tok = JS_GetOpaque2(ctx, this_val, js_token_class_id)))
    return JS_EXCEPTION;
  dbuf_init2(&dbuf, JS_GetRuntime(ctx), (DynBufReallocFunc*)js_realloc_rt);

  dbuf_put(&dbuf, &tok->data[tok->offset], tok->length);

  // token_dump(tok, ctx, &dbuf);

  ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
  dbuf_free(&dbuf);

  return ret;
}

static JSValue
js_token_inspect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  Token* tok;
  DynBuf dbuf;
  JSValue ret;

  if(!(tok = JS_GetOpaque2(ctx, this_val, js_token_class_id)))
    return JS_EXCEPTION;

  dbuf_init2(&dbuf, JS_GetRuntime(ctx), (DynBufReallocFunc*)js_realloc_rt);

  dbuf_putstr(&dbuf, "Token { loc: ");
  location_dump(&dbuf, &tok->loc);

  dbuf_printf(&dbuf,
              ", offset: %3" PRIu32 ", length: %3" PRIu32 ", type: Token.%-15s, chars: '",
              tok->offset,
              tok->length,
              token_type(tok));

  dbuf_put_escaped(&dbuf, &tok->data[tok->offset], tok->length);
  dbuf_putstr(&dbuf, "' }");

  ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
  dbuf_free(&dbuf);
  return ret;
}

static JSValue
js_token_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
  Token* tok;
  JSValue ret = JS_UNDEFINED;

  if(!(tok = JS_GetOpaque2(ctx, this_val, js_token_class_id)))
    return JS_EXCEPTION;

  switch(magic) {}
  return ret;
}

static JSValue
js_token_get(JSContext* ctx, JSValueConst this_val, int magic) {
  Token* tok;
  JSValue ret = JS_UNDEFINED;
  if(!(tok = JS_GetOpaque2(ctx, this_val, js_token_class_id)))
    return JS_EXCEPTION;

  switch(magic) {
    case PROP_LENGTH: {
      ret = JS_NewInt64(ctx, tok->length);
      break;
    }

    case PROP_OFFSET: {
      ret = JS_NewInt64(ctx, tok->offset);
      break;
    }

    case PROP_CHARS: {
      ret = JS_NewStringLen(ctx, &tok->data[tok->offset], tok->length);
      break;
    }

    case PROP_LOC: {
      ret = js_position_new(ctx, &tok->loc);
      break;
    }
    case PROP_ID: {
      ret = JS_NewInt32(ctx, tok->id);
      break;
    }  case PROP_TYPE: {
      ret = JS_NewString(ctx, token_type(tok));
      break;
    }
  }
  return ret;
}

static JSValue
js_token_set(JSContext* ctx, JSValueConst this_val, JSValueConst value, int magic) {
  Token* tok;

  if(!(tok = JS_GetOpaque2(ctx, this_val, js_token_class_id)))
    return JS_EXCEPTION;

  switch(magic) {}
  return JS_UNDEFINED;
}

static JSValue
js_token_funcs(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
  return JS_UNDEFINED;
}

void
js_token_finalizer(JSRuntime* rt, JSValue val) {
  Token* tok = JS_GetOpaque(val, js_token_class_id);
  if(tok) {}
  // JS_FreeValueRT(rt, val);
}

JSClassDef js_token_class = {
    .class_name = "Token",
    .finalizer = js_token_finalizer,
};

static const JSCFunctionListEntry js_token_proto_funcs[] = {
    JS_CGETSET_MAGIC_DEF("length", js_token_get, NULL, PROP_LENGTH),
    JS_CGETSET_MAGIC_DEF("offset", js_token_get, NULL, PROP_OFFSET),
    JS_CGETSET_MAGIC_DEF("loc", js_token_get, NULL, PROP_LOC),
    JS_CGETSET_MAGIC_DEF("id", js_token_get, NULL, PROP_ID),
    JS_CGETSET_MAGIC_DEF("type", js_token_get, NULL, PROP_TYPE),
    JS_CFUNC_DEF("toString", 0, js_token_tostring),
    JS_CFUNC_DEF("inspect", 0, js_token_inspect),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Token", JS_PROP_CONFIGURABLE)};

static const JSCFunctionListEntry js_token_static_funcs[] = {
    JS_PROP_INT32_DEF("COMMENT", COMMENT, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("STRING_LITERAL", STRING_LITERAL, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("TEMPLATE_LITERAL", TEMPLATE_LITERAL, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("NUMERIC_LITERAL", NUMERIC_LITERAL, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("BOOLEAN_LITERAL", BOOLEAN_LITERAL, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("NULL_LITERAL", NULL_LITERAL, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("PUNCTUATOR", PUNCTUATOR, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("KEYWORD", KEYWORD, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("IDENTIFIER", IDENTIFIER, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("REGEXP_LITERAL", REGEXP_LITERAL, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("EOF", EOF, JS_PROP_ENUMERABLE)};

VISIBLE JSClassID js_lexer_class_id = 0;
JSValue lexer_proto, lexer_constructor, lexer_ctor;
static const char punctuator_chars[] = "=.-%}>,*[<!/]~&(;?|):+^{@";

enum lexer_methods {
  METHOD_PEEKC = 0,
  METHOD_GETC,
  METHOD_SKIPC,
  METHOD_IGNORE,
  METHOD_GET_RANGE,
  METHOD_CURRENT_LINE,
  METHOD_ACCEPT,
  METHOD_ACCEPT_RUN,
  METHOD_BACKUP,
  METHOD_SKIPUNTIL,
  METHOD_ADD_TOKEN,
  METHOD_ERROR
};
enum lexer_functions { STATIC_FROM = 0, STATIC_OF };
enum lexer_getters {
  LEXER_SIZE = 0,
  LEXER_POS,
  LEXER_START,
  LEXER_LINE,
  LEXER_COLUMN,
  LEXER_LINESTART,
  LEXER_LINELEN,
  LEXER_LINEEND,
  LEXER_LINEPOS,
  LEXER_EOF,
  LEXER_TOKEN,
  LEXER_COLUMN_INDEX,
  LEXER_CURRENT_LINE,
  LEXER_KEYWORDS,
  LEXER_STATEFN,
  LEXER_POSITION
};

enum lexer_ctype {
  IS_ALPHA_CHAR = 0,
  IS_DECIMAL_DIGIT,
  IS_HEX_DIGIT,
  IS_IDENTIFIER_CHAR,
  IS_IDENTIFIER_FIRST_CHAR,
  IS_LINE_TERMINATOR,
  IS_OCTAL_DIGIT,
  IS_PUNCTUATOR,
  IS_PUNCTUATOR_CHAR,
  IS_QUOTE_CHAR,
  IS_REGEXP_CHAR,
  IS_WHITESPACE
};

JSValue
js_lexer_new(JSContext* ctx, JSValueConst proto, JSValueConst value) {
  Lexer *lex, *ptr2;
  JSValue obj = JS_UNDEFINED;
  if(!(lex = js_mallocz(ctx, sizeof(Lexer))))
    return JS_EXCEPTION;

  lexer_init(lex, ctx);

  obj = JS_NewObjectProtoClass(ctx, proto, js_lexer_class_id);

  if(JS_IsException(obj))
    goto fail;
  JS_SetOpaque(obj, lex);

  lex->input = js_input_buffer(ctx, value);
  lex->state_fn = JS_UNDEFINED;

  return obj;
fail:
  js_free(ctx, lex);
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

JSValue
js_lexer_wrap(JSContext* ctx, Lexer* lex) {
  JSValue obj;
  Lexer* ret;

  lex->ref_count++;

  obj = JS_NewObjectProtoClass(ctx, lexer_proto, js_lexer_class_id);
  JS_SetOpaque(obj, lex);
  return obj;
}

static JSValue
js_lexer_inspect(JSContext* ctx, JSValueConst this_val, BOOL color) {
  Lexer* lex;
  DynBuf dbuf;
  JSValue ret;

  if(!(lex = JS_GetOpaque2(ctx, this_val, js_lexer_class_id)))
    return JS_EXCEPTION;

  dbuf_init2(&dbuf, JS_GetRuntime(ctx), (DynBufReallocFunc*)js_realloc_rt);

  ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);
  dbuf_free(&dbuf);
  return ret;
}

static JSValue
js_lexer_toarray(JSContext* ctx, Lexer* lex) {
  size_t i;
  JSValue array = JS_NewArray(ctx);
  return array;
}

static JSValue
js_lexer_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst* argv) {
  JSValue proto;
  /* using new_target to get the prototype is necessary when the
     class is extended. */
  proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if(JS_IsException(proto))
    proto = JS_DupValue(ctx, lexer_proto);

  return js_lexer_new(ctx, proto, argc > 0 ? argv[0] : JS_UNDEFINED);
}

static JSValue
js_lexer_method(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
  Lexer* lex;
  JSValue ret = JS_UNDEFINED;
  if(!(lex = JS_GetOpaque2(ctx, this_val, js_lexer_class_id)))
    return JS_EXCEPTION;

  switch(magic) {
    case METHOD_PEEKC: {
      if(lexer_remain(lex)) {
        size_t len;
        uint8_t* buf = lexer_peek(lex, &len);
        ret = JS_NewStringLen(ctx, buf, len);
      }
      break;
    }

    case METHOD_GETC: {
      if(lexer_remain(lex)) {
        size_t len;
        uint8_t* buf = lexer_get(lex, &len);
        ret = JS_NewStringLen(ctx, buf, len);
      }
      break;
    }

    case METHOD_SKIPC: {
      if(lexer_remain(lex)) {
        int32_t ntimes = 1;
        uint8_t* p;
        size_t n;
        if(argc > 0)
          JS_ToInt32(ctx, &ntimes, argv[0]);
        while(ntimes-- > 0) { p = lexer_get(lex, &n); }
        ret = JS_NewStringLen(ctx, p, n);
      }
      break;
    }

    case METHOD_SKIPUNTIL: {
      if(lexer_remain(lex)) {
        JSValueConst pred;
        if(!JS_IsFunction(ctx, argv[0]))
          return JS_ThrowTypeError(ctx, "argument 1 is not a function");
        pred = argv[0];
        while(lex->pos < lex->size) {
          size_t n;
          uint8_t* p = lexer_peek(lex, &n);
          JSValue str = JS_NewStringLen(ctx, p, n);
          JSValue ret = JS_Call(ctx, pred, this_val, 1, &str);
          BOOL b = JS_ToBool(ctx, ret);
          JS_FreeValue(ctx, ret);
          if(b) {
            ret = str;
            break;
          }
          JS_FreeValue(ctx, str);
          lexer_get(lex, 0);
        }
      }
      break;
    }

    case METHOD_IGNORE: {
      lexer_ignore(lex);
      break;
    }

    case METHOD_GET_RANGE: {
      size_t start, end;
      start = lex->start;
      end = lex->pos;
      if(argc > 0) {
        js_value_to_size(ctx, &start, argv[0]);
        if(argc > 1)
          js_value_to_size(ctx, &end, argv[1]);
      }
      ret = JS_NewStringLen(ctx, (const char*)&lex->data[start], end - start);
      break;
    }

    case METHOD_CURRENT_LINE: {
      size_t start, end;
      start = lex->start;
      end = lex->pos;
      while(start > 0 && lex->data[start - 1] != '\n') start--;
      while(end < lex->size && lex->data[end] != '\n') end++;
      ret = JS_NewStringLen(ctx, (const char*)&lex->data[start], end - start);
      break;
    }

    case METHOD_ACCEPT: {
      if(!lexer_eof(lex)) {
        JSValueConst pred = argv[0];
        size_t started = lex->pos;
        JSValue ch, r;
        uint8_t* p;
        size_t len;
        BOOL b;
        p = lexer_peek(lex, &len);
        ch = JS_NewStringLen(ctx, p, len);
        b = predicate_call(ctx, pred, 1, &ch);
        JS_FreeValue(ctx, ch);
        ret = JS_NewBool(ctx, b);
        if(b)
          lexer_get(lex, 0);
      }
      break;
    }

    case METHOD_ACCEPT_RUN: {
      if(!lexer_eof(lex)) {
        JSValueConst pred = argv[0];
        size_t started = lex->pos;
        while(lexer_remain(lex)) {
          JSValue ch, r;
          uint8_t* p;
          size_t len;
          BOOL b;
          p = lexer_peek(lex, &len);
          ch = JS_NewStringLen(ctx, p, len);
          b = predicate_call(ctx, pred, 1, &ch);
          // printf("acceptRun %.*s = %i\n", len, p, b);
          JS_FreeValue(ctx, ch);
          if(!b)
            break;
          lexer_get(lex, 0);
        }
        ret = JS_NewUint32(ctx, lex->pos - started);
      }
      break;
    }

    case METHOD_BACKUP: {
      int32_t ntimes = 1;
      uint8_t c, *p;
      size_t len;
      if(argc > 0)
        JS_ToInt32(ctx, &ntimes, argv[0]);
      while(ntimes-- > 0 && lex->pos > 0) {
        size_t n = *(size_t*)vector_back(&lex->charlengths, sizeof(size_t));
        vector_pop(&lex->charlengths, sizeof(size_t));
        lex->pos -= n;
      }
      p = js_input_buffer_peek(&lex->input, &len);
      ret = JS_NewStringLen(ctx, p, len);
      break;
    }

    case METHOD_ADD_TOKEN: {
      int32_t tokId;
      Token* tok;
      JS_ToInt32(ctx, &tokId, argv[0]);
      if((tok = js_mallocz(ctx, sizeof(Token)))) {
        tok->data = lex->data;
        tok->offset = lex->start;
        tok->length = lex->pos - lex->start;
        tok->loc = lex->loc;
        tok->id = tokId;
        list_add_tail(&tok->link, &lex->tokens);
      }
      // printf("lexer addToken %s «%.*s»\n", token_type(tok), lex->pos - lex->start, &lex->data[lex->start]);
      lexer_ignore(lex);
      break;
    }

    case METHOD_ERROR: {
      SyntaxError error;
      const char* str;
      size_t len;
      str = JS_ToCStringLen(ctx, &len, argv[0]);
      error.message = js_strndup(ctx, str, len);
      JS_FreeCString(ctx, str);
      error.offset = lex->start;
      error.length = lex->pos - lex->start;
      error.loc = lex->loc;
      // printf("lexer SyntaxError('%s', %u:%u)\n", error.message, lex->loc.line + 1, lex->loc.column + 1);
      ret = js_syntax_error_new(ctx, error);
      break;
    }
  }
  return ret;
}

static JSValue
js_lexer_get(JSContext* ctx, JSValueConst this_val, int magic) {
  Lexer* lex;
  JSValue ret = JS_UNDEFINED;
  if(!(lex = JS_GetOpaque2(ctx, this_val, js_lexer_class_id)))
    return JS_EXCEPTION;

  switch(magic) {
    case LEXER_POS: ret = JS_NewInt64(ctx, lex->pos); break;
    case LEXER_SIZE: ret = JS_NewInt64(ctx, lex->size); break;
    case LEXER_START: ret = JS_NewInt64(ctx, lex->start); break;
    case LEXER_LINE: ret = JS_NewUint32(ctx, lex->loc.line + 1); break;
    case LEXER_COLUMN: ret = JS_NewUint32(ctx, lex->loc.column + 1); break;
    case LEXER_LINESTART: ret = JS_NewUint32(ctx, lexer_line(lex).start); break;
    case LEXER_LINELEN: ret = JS_NewUint32(ctx, lexer_line(lex).length); break;
    case LEXER_LINEEND: {
      Line ln = lexer_line(lex);
      ret = JS_NewUint32(ctx, ln.start + ln.length);
      break;
    }

    case LEXER_LINEPOS: {
      Line ln = lexer_line(lex);
      ret = JS_NewUint32(ctx, lex->pos - ln.start);
      break;
    }

    case LEXER_EOF: ret = JS_NewBool(ctx, lex->pos >= lex->size); break;
    case LEXER_TOKEN: {
      size_t size = lex->pos - lex->start;
      ret = JS_NewStringLen(ctx, (const char*)&lex->data[lex->start], size);
      break;
    }

    case LEXER_CURRENT_LINE: {
      Line ln = lexer_line(lex);
      ret = JS_NewStringLen(ctx, (const char*)&lex->data[ln.start], ln.length);
      break;
    }

    case LEXER_COLUMN_INDEX: {
      size_t index;
      for(index = lex->pos; index > 0; index--) {
        if(lex->data[index - 1] == '\n')
          break;
      }
      ret = JS_NewInt32(ctx, lex->pos - index);
      break;
    }
      /*    case LEXER_KEYWORDS: {
            ret = js_strvec_to_array(ctx, lex->keywords);
            break;
          }*/
    case LEXER_STATEFN: {
      ret = JS_DupValue(ctx, lex->state_fn);
      break;
    }

    case LEXER_POSITION: {
      ret = js_position_new(ctx, &lex->loc);
      break;
    }
  }
  return ret;
}

static JSValue
js_lexer_iterator(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  return this_val;
}

static JSValue
js_lexer_set(JSContext* ctx, JSValueConst this_val, JSValueConst value, int magic) {
  Lexer* lex;

  if(!(lex = JS_GetOpaque2(ctx, this_val, js_lexer_class_id)))
    return JS_EXCEPTION;

  switch(magic) {
    case LEXER_POS: js_value_to_size(ctx, &lex->pos, value); break;
    case LEXER_SIZE: js_value_to_size(ctx, &lex->size, value); break;
    case LEXER_START: js_value_to_size(ctx, &lex->start, value); break;
    /*case LEXER_KEYWORDS: {
      js_strvec_free(ctx, lex->keywords);
      lex->nkeywords = js_array_length(ctx, value);
      lex->keywords = js_array_to_strvec(ctx, value);
      qsort(lex->keywords, lex->nkeywords, sizeof(char*), (int (*)(const void*, const void*)) & keywords_cmp); break;
    }*/
    case LEXER_STATEFN: {
      if(!JS_IsUndefined(lex->state_fn))
        JS_FreeValue(ctx, lex->state_fn);
      lex->state_fn = JS_DupValue(ctx, value);
      break;
    }
  }
  return JS_UNDEFINED;
}

JSValue
js_lexer_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, BOOL* pdone, int magic) {
  JSValue ret = JS_UNDEFINED;
  Lexer* lex;
  size_t pos;

  if(!(lex = JS_GetOpaque2(ctx, this_val, js_lexer_class_id)))
    return JS_EXCEPTION;

  *pdone = FALSE;

  while(list_empty(&lex->tokens) && lex->start < lex->size) {
    pos = lex->pos;
    for(;;) {
      JSValue except;
      ret = JS_Call(ctx, lex->state_fn, this_val, 0, 0);
      except = JS_GetException(ctx);
      if(JS_IsObject(except)) {
        printf("except = %s\n", JS_ToCString(ctx, except));
        JS_SetPropertyStr(ctx, this_val, "exception", except);
        *pdone = FALSE;
        return JS_NULL;
      }
      if(JS_IsFunction(ctx, ret)) {
        const char* name;
        JS_FreeValue(ctx, lex->state_fn);
        lex->state_fn = ret;
        name = js_function_name(ctx, ret);
        // printf("lexer state_fn='%s' @=<%.*s>\n", name ? name : "<anonymous>", 10, &lex->data[lex->start]);
        if(name)
          JS_FreeCString(ctx, name);
        if(pos == lex->pos)
          continue;
      }
      break;
    }
    if(pos == lex->pos) {
      *pdone = TRUE;
      return JS_ThrowRangeError(ctx, "Lexer pos before=%zu after=%zu", pos, lex->pos);
    }
  }

  if(!list_empty(&lex->tokens)) {
    Token* tok = (Token*)lex->tokens.next;
    list_del(&tok->link);
    return js_token_new(ctx, *tok);
  }

  *pdone = TRUE;
  return ret;
}

static JSValue
js_lexer_ctype(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
  JSValue b;
  int c;
  int32_t result;

  if(JS_IsString(argv[0])) {
    const char* str;
    str = JS_ToCString(ctx, argv[0]);
    c = *str;
    JS_FreeCString(ctx, str);
  } else {
    int32_t num = 0;
    JS_ToInt32(ctx, &num, argv[0]);
    c = num;
  }

  switch(magic) {
    case IS_ALPHA_CHAR: result = str_contains(ALPHA_CHARS, c); break;
    case IS_WHITESPACE: result = (c == 9 || c == 0xb || c == 0xc || c == ' '); break;
    case IS_DECIMAL_DIGIT: result = str_contains("0123456789", c); break;
    case IS_HEX_DIGIT: result = str_contains("0123456789ABCDEFabcdef", c); break;
    case IS_OCTAL_DIGIT: result = c >= '0' && c <= '7'; break;
    case IS_LINE_TERMINATOR: result = c == '\r' || c == '\n'; break;
    case IS_QUOTE_CHAR: result = c == '"' || c == '\'' || c == '`'; break;
    case IS_REGEXP_CHAR: result = c == '/'; break;
    case IS_PUNCTUATOR_CHAR: result = (str_contains("=.-%}>,*[<!/]~&(;?|):+^{@", c)); break;
    case IS_IDENTIFIER_CHAR: result = (str_contains(ALPHA_CHARS "0123456789$_", c)); break;
    case IS_IDENTIFIER_FIRST_CHAR: result = (str_contains(ALPHA_CHARS "$_", c)); break;
    default:
      result = -1;
      JS_ThrowRangeError(ctx, "js_lexer_ctype invalid magic: %d", magic);
      break;
  }
  b = JS_NewInt32(ctx, result);
  return b;
}

void
js_lexer_finalizer(JSRuntime* rt, JSValue val) {
  Lexer* lex;

  if((lex = JS_GetOpaque(val, js_lexer_class_id))) {
    if(--lex->ref_count == 0)
      lexer_free(lex, rt);
  }
  // JS_FreeValueRT(rt, val);
}

JSClassDef js_lexer_class = {
    .class_name = "Lexer",
    .finalizer = js_lexer_finalizer,
};

static const JSCFunctionListEntry js_lexer_proto_funcs[] = {
    JS_ITERATOR_NEXT_DEF("next", 0, js_lexer_next, 0),
    JS_CFUNC_MAGIC_DEF("getc", 0, js_lexer_method, METHOD_GETC),
    JS_CFUNC_MAGIC_DEF("peek", 0, js_lexer_method, METHOD_PEEKC),
    JS_CFUNC_MAGIC_DEF("skip", 0, js_lexer_method, METHOD_SKIPC),
    JS_CFUNC_MAGIC_DEF("skipUntil", 0, js_lexer_method, METHOD_SKIPUNTIL),
    JS_CFUNC_MAGIC_DEF("ignore", 0, js_lexer_method, METHOD_IGNORE),
    JS_CFUNC_MAGIC_DEF("getRange", 0, js_lexer_method, METHOD_GET_RANGE),
    JS_CFUNC_MAGIC_DEF("currentLine", 0, js_lexer_method, METHOD_CURRENT_LINE),
    JS_CFUNC_MAGIC_DEF("accept", 1, js_lexer_method, METHOD_ACCEPT),
    JS_CFUNC_MAGIC_DEF("acceptRun", 1, js_lexer_method, METHOD_ACCEPT_RUN),
    JS_CFUNC_MAGIC_DEF("backup", 0, js_lexer_method, METHOD_BACKUP),
    JS_CFUNC_MAGIC_DEF("addToken", 0, js_lexer_method, METHOD_ADD_TOKEN),
    JS_CFUNC_MAGIC_DEF("error", 1, js_lexer_method, METHOD_ERROR),
    JS_CGETSET_MAGIC_DEF("size", js_lexer_get, js_lexer_set, LEXER_SIZE),
    JS_CGETSET_MAGIC_DEF("pos", js_lexer_get, js_lexer_set, LEXER_POS),
    JS_CGETSET_MAGIC_DEF("start", js_lexer_get, js_lexer_set, LEXER_START),
    /* JS_CGETSET_MAGIC_DEF("line", js_lexer_get, js_lexer_set, LEXER_LINE),
     JS_CGETSET_MAGIC_DEF("column", js_lexer_get, js_lexer_set, LEXER_COLUMN),*/
    JS_CGETSET_MAGIC_DEF("position", js_lexer_get, 0, LEXER_POSITION),
    JS_CGETSET_MAGIC_DEF("lineStart", js_lexer_get, 0, LEXER_LINESTART),
    JS_CGETSET_MAGIC_DEF("lineLength", js_lexer_get, 0, LEXER_LINELEN),
    JS_CGETSET_MAGIC_DEF("lineEnd", js_lexer_get, 0, LEXER_LINEEND),
    // JS_CGETSET_MAGIC_DEF("linePos", js_lexer_get, 0, LEXER_LINEPOS),
    JS_CGETSET_MAGIC_DEF("columnIndex", js_lexer_get, 0, LEXER_COLUMN_INDEX),
    JS_CGETSET_MAGIC_DEF("currentLine", js_lexer_get, 0, LEXER_CURRENT_LINE),
    JS_CGETSET_MAGIC_DEF("eof", js_lexer_get, 0, LEXER_EOF),
    JS_CGETSET_MAGIC_DEF("token", js_lexer_get, 0, LEXER_TOKEN),
    JS_CGETSET_MAGIC_DEF("keywords", js_lexer_get, js_lexer_set, LEXER_KEYWORDS),
    JS_CGETSET_MAGIC_DEF("stateFn", js_lexer_get, js_lexer_set, LEXER_STATEFN),
    JS_CFUNC_DEF("[Symbol.iterator]", 0, js_lexer_iterator),

    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "Lexer", JS_PROP_C_W_E)};

static const JSCFunctionListEntry js_lexer_static_funcs[] = {
    JS_CFUNC_MAGIC_DEF("isAlphaChar", 1, js_lexer_ctype, IS_ALPHA_CHAR),
    JS_CFUNC_MAGIC_DEF("isDecimalDigit", 1, js_lexer_ctype, IS_DECIMAL_DIGIT),
    JS_CFUNC_MAGIC_DEF("isHexDigit", 1, js_lexer_ctype, IS_HEX_DIGIT),
    JS_CFUNC_MAGIC_DEF("isIdentifierChar", 1, js_lexer_ctype, IS_IDENTIFIER_CHAR),
    JS_CFUNC_MAGIC_DEF("isIdentifierFirstChar", 1, js_lexer_ctype, IS_IDENTIFIER_FIRST_CHAR),
    JS_CFUNC_MAGIC_DEF("isLineTerminator", 1, js_lexer_ctype, IS_LINE_TERMINATOR),
    JS_CFUNC_MAGIC_DEF("isOctalDigit", 1, js_lexer_ctype, IS_OCTAL_DIGIT),
    JS_CFUNC_MAGIC_DEF("isPunctuator", 1, js_lexer_ctype, IS_PUNCTUATOR),
    JS_CFUNC_MAGIC_DEF("isPunctuatorChar", 1, js_lexer_ctype, IS_PUNCTUATOR_CHAR),
    JS_CFUNC_MAGIC_DEF("isQuoteChar", 1, js_lexer_ctype, IS_QUOTE_CHAR),
    JS_CFUNC_MAGIC_DEF("isRegExpChar", 1, js_lexer_ctype, IS_REGEXP_CHAR),
    JS_CFUNC_MAGIC_DEF("isWhitespace", 1, js_lexer_ctype, IS_WHITESPACE),
};

static int
js_lexer_init(JSContext* ctx, JSModuleDef* m) {
  JS_NewClassID(&js_syntax_error_class_id);
  JS_NewClass(JS_GetRuntime(ctx), js_syntax_error_class_id, &js_syntax_error_class);

  syntax_error_proto = JS_NewError(ctx);
  JS_SetPropertyFunctionList(ctx,
                             syntax_error_proto,
                             js_syntax_error_proto_funcs,
                             countof(js_syntax_error_proto_funcs));
  JS_SetClassProto(ctx, js_syntax_error_class_id, syntax_error_proto);

  syntax_error_ctor = JS_NewCFunction2(ctx, js_syntax_error_constructor, "SyntaxError", 1, JS_CFUNC_constructor, 0);

  JS_SetConstructor(ctx, syntax_error_ctor, syntax_error_proto);

  if(m)
    JS_SetModuleExport(ctx, m, "SyntaxError", syntax_error_ctor);

  JS_NewClassID(&js_token_class_id);
  JS_NewClass(JS_GetRuntime(ctx), js_token_class_id, &js_token_class);

  token_proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, token_proto, js_token_proto_funcs, countof(js_token_proto_funcs));
  JS_SetClassProto(ctx, js_token_class_id, token_proto);

  token_ctor = JS_NewCFunction2(ctx, js_token_constructor, "Token", 1, JS_CFUNC_constructor, 0);

  JS_SetConstructor(ctx, token_ctor, token_proto);
  JS_SetPropertyFunctionList(ctx, token_ctor, js_token_static_funcs, countof(js_token_static_funcs));

  if(m) {
    JS_SetModuleExport(ctx, m, "Token", token_ctor);
  }
  JS_NewClassID(&js_lexer_class_id);
  JS_NewClass(JS_GetRuntime(ctx), js_lexer_class_id, &js_lexer_class);

  lexer_proto = JS_NewObject(ctx);
  JS_SetPropertyFunctionList(ctx, lexer_proto, js_lexer_proto_funcs, countof(js_lexer_proto_funcs));
  JS_SetClassProto(ctx, js_lexer_class_id, lexer_proto);

  lexer_ctor = JS_NewCFunction2(ctx, js_lexer_constructor, "Lexer", 1, JS_CFUNC_constructor, 0);

  JS_SetConstructor(ctx, lexer_ctor, lexer_proto);
  JS_SetPropertyFunctionList(ctx, lexer_ctor, js_lexer_static_funcs, countof(js_lexer_static_funcs));

  if(m) {
    JS_SetModuleExport(ctx, m, "Lexer", lexer_ctor);
  }

  return 0;
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_lexer
#endif

VISIBLE JSModuleDef*
JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
  JSModuleDef* m;
  m = JS_NewCModule(ctx, module_name, &js_lexer_init);
  if(!m)
    return NULL;
  JS_AddModuleExport(ctx, m, "SyntaxError");
  JS_AddModuleExport(ctx, m, "Token");
  JS_AddModuleExport(ctx, m, "Lexer");
  return m;
}
