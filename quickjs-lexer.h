#ifndef QUICKJS_LEXER_H
#define QUICKJS_LEXER_H

#include "list.h"
#include "utils.h"
#include "quickjs-predicate.h"
#include "vector.h"

enum token_types {
  TOKEN_ID_COMMENT = 0,
  TOKEN_ID_STRING_LITERAL,
  TOKEN_ID_TEMPLATE_LITERAL,
  TOKEN_ID_NUMERIC_LITERAL,
  TOKEN_ID_BOOLEAN_LITERAL,
  TOKEN_ID_NULL_LITERAL,
  TOKEN_ID_PUNCTUATOR,
  TOKEN_ID_KEYWORD,
  TOKEN_ID_IDENTIFIER,
  TOKEN_ID_REGEXP_LITERAL,
  TOKEN_ID_PROP_EOF
};

typedef struct {
  const char* file;
  uint32_t line;
  uint32_t column;
} Location;

typedef struct {
  uint32_t start;
  uint32_t byte_length;
} Line;

typedef struct {
  const uint8_t* data;
  uint32_t byte_length;
  uint32_t offset;
  Location loc;
  const char* message;
} SyntaxError;

typedef union Lexer {
  struct InputBuffer input;
  struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
    void (*free)(JSContext*, const char*);
    size_t start;
    Location loc;
    vector charlengths;
    /*size_t nkeywords;
    char** keywords;*/
    JSValue state_fn;
    size_t ref_count;
    struct list_head tokens;
   };
} Lexer;

typedef struct {
  struct list_head link;
  const uint8_t* data;
  uint32_t byte_length;
  uint32_t num_chars;
  uint32_t offset;
  enum token_types id;
  Location loc;
  Lexer* lexer;
} Token;

extern JSClassID js_syntax_error_class_id, js_token_class_id, js_lexer_class_id;

JSValue js_syntax_error_new(JSContext*, SyntaxError arg);

static inline Token*
js_syntax_error_data(JSContext* ctx, JSValueConst value) {
  return JS_GetOpaque2(ctx, value, js_syntax_error_class_id);
}

static inline Token*
js_token_data(JSContext* ctx, JSValueConst value) {
  return JS_GetOpaque2(ctx, value, js_token_class_id);
}

JSValue js_token_wrap(JSContext*, Token*);

static inline Lexer*
js_lexer_data(JSContext* ctx, JSValueConst value) {
  return JS_GetOpaque2(ctx, value, js_lexer_class_id);
}

JSValue js_lexer_wrap(JSContext*, Lexer*);

#endif /* defined(QUICKJS_LEXER_H) */