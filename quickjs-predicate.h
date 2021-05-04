#ifndef QUICKJS_PREDICATE_H
#define QUICKJS_PREDICATE_H

#include "predicate.h"

extern JSClassID js_predicate_class_id;

static inline Predicate*
js_predicate_data(JSContext* ctx, JSValueConst value) {
  return JS_GetOpaque(value, js_predicate_class_id);
}

JSValue js_predicate_wrap(JSContext*, Predicate);
JSValue js_predicate_new(JSContext*, JSValue proto, JSValue);
int32_t predicate_call(JSContext*, JSValue, int argc, JSValueConst* argv);

#endif /* defined(QUICKJS_PREDICATE_H) */
