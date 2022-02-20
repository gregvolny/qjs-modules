#include "defines.h"
#include "char-utils.h"
#include "buffer-utils.h"
#include "utils.h"
#if HAVE_TERMIOS_H
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#include "debug.h"

/**
 * \addtogroup buffer-utils
 * @{
 */
size_t
ansi_length(const char* str, size_t len) {
  size_t i, n = 0, p;
  for(i = 0; i < len;) {
    if(str[i] == 0x1b && (p = ansi_skip(&str[i], len - i)) > 0) {
      i += p;
      continue;
    }
    n++;
    i++;
  }
  return n;
}

size_t
ansi_skip(const char* str, size_t len) {
  size_t pos = 0;
  if(str[pos] == 0x1b) {
    if(++pos < len && str[pos] == '[') {
      while(++pos < len)
        if(is_alphanumeric_char(str[pos]))
          break;
      if(++pos < len && str[pos] == '~')
        ++pos;
      return pos;
    }
  }
  return 0;
}

size_t
ansi_truncate(const char* str, size_t len, size_t limit) {
  size_t i, n = 0, p;
  for(i = 0; i < len;) {
    if((p = ansi_skip(&str[i], len - i)) > 0) {
      i += p;
      continue;
    }
    n += is_escape_char(str[i]) ? 2 : 1;

    i++;
    if(n > limit)
      break;
  }
  return i;
}

int64_t
array_search(void* a, size_t m, size_t elsz, void* needle) {
  char* ptr = a;
  int64_t n, ret;
  n = m / elsz;
  for(ret = 0; ret < n; ret++) {
    if(!memcmp(ptr, needle, elsz))
      return ret;

    ptr += elsz;
  }
  return -1;
}

char*
str_escape(const char* s) {
  DynBuf dbuf;
  dbuf_init2(&dbuf, 0, 0);
  dbuf_put_escaped(&dbuf, s, strlen(s));
  dbuf_0(&dbuf);
  return (char*)dbuf.buf;
}

char*
byte_escape(const void* s, size_t n) {
  DynBuf dbuf;
  dbuf_init2(&dbuf, 0, 0);
  dbuf_put_escaped(&dbuf, s, n);
  dbuf_0(&dbuf);
  return (char*)dbuf.buf;
}

size_t
byte_findb(const void* haystack, size_t hlen, const void* what, size_t wlen) {
  size_t i, last;
  const char* s = (const char*)haystack;
  if(hlen < wlen)
    return hlen;
  last = hlen - wlen;
  for(i = 0; i <= last; i++) {
    if(byte_equal(s, wlen, what))
      return i;
    s++;
  }
  return hlen;
}

size_t
byte_finds(const void* haystack, size_t hlen, const char* what) {
  return byte_findb(haystack, hlen, what, strlen(what));
}

size_t
byte_equal(const void* s, size_t n, const void* t) {
  return memcmp(s, t, n) == 0;
}

void
byte_copy(void* out, size_t len, const void* in) {
  char* s = (char*)out;
  const char* t = (const char*)in;
  size_t i;
  for(i = 0; i < len; ++i) s[i] = t[i];
}

void
byte_copyr(void* out, size_t len, const void* in) {
  char* s = (char*)out + len;
  const char* t = (const char*)in;
  const char* u = t + len;

  for(;;) {
    if(t >= u)
      break;
    --u;
    --s;
    *s = *u;
  }
}

char*
dbuf_at_n(const DynBuf* db, size_t i, size_t* n, char sep) {
  size_t p, l = 0;
  for(p = 0; p < db->size; ++p) {
    if(l == i) {
      *n = byte_chr((const char*)&db->buf[p], db->size - p, sep);
      return (char*)&db->buf[p];
    }
    if(db->buf[p] == sep)
      ++l;
  }
  *n = 0;
  return 0;
}

const char*
dbuf_last_line(DynBuf* db, size_t* len) {
  size_t i;

  if((i = byte_rchr(db->buf, db->size, '\n')) < db->size)
    i++;
  else
    i = 0;

  if(len)
    *len = db->size - i;

  return (const char*)&db->buf[i];
}

int
dbuf_prepend(DynBuf* s, const uint8_t* data, size_t len) {
  int ret;
  if(!(ret = dbuf_reserve_start(s, len)))
    memcpy(s->buf, data, len);

  return 0;
}

void
dbuf_put_colorstr(DynBuf* db, const char* str, const char* color, int with_color) {
  if(with_color)
    dbuf_putstr(db, color);

  dbuf_putstr(db, str);
  if(with_color)
    dbuf_putstr(db, COLOR_NONE);
}

void
dbuf_put_escaped_pred(DynBuf* db, const char* str, size_t len, int (*pred)(int)) {
  size_t i = 0, j;
  char c;
  while(i < len) {
    if((j = predicate_find(&str[i], len - i, pred))) {
      dbuf_append(db, (const uint8_t*)&str[i], j);
      i += j;
    }
    if(i == len)
      break;
    dbuf_putc(db, '\\');

    if(str[i] == 0x1b) {
      dbuf_append(db, (const uint8_t*)"x1b", 3);
    } else {
      int r = pred(str[i]);

      dbuf_putc(db, (r > 1 && r <= 127) ? r : (c = escape_char_letter(str[i])) ? c : str[i]);

      if(r == 'u' || r == 'x')
        dbuf_printf(db, r == 'u' ? "%04x" : "%02x", str[i]);
    }
    i++;
  }
}

void
dbuf_put_escaped_table(DynBuf* db, const char* str, size_t len, const char table[256]) {
  size_t i = 0, j;
  char c;
  while(i < len) {
    if((j = lookup_find(&str[i], len - i, table))) {
      dbuf_append(db, (const uint8_t*)&str[i], j);
      i += j;
    }
    if(i == len)
      break;
    dbuf_putc(db, '\\');

    if(str[i] == 0x1b) {
      dbuf_append(db, (const uint8_t*)"x1b", 3);
    } else {
      int r = table[(unsigned char)str[i]];

      dbuf_putc(db, (r > 1 && r <= 127) ? r : (c = escape_char_letter(str[i])) ? c : str[i]);

      if(r == 'u' || r == 'x')
        dbuf_printf(db, r == 'u' ? "%04x" : "%02x", str[i]);
    }
    i++;
  }
}

void
dbuf_put_unescaped_pred(DynBuf* db, const char* str, size_t len, int (*pred)(int)) {
  size_t i = 0, j;
  char c;
  int r;
  while(i < len) {
    if((j = byte_chr(&str[i], len - i, '\\'))) {
      dbuf_append(db, (const uint8_t*)&str[i], j);
      i += j;
    }
    if(i == len)
      break;

    if(!(r = pred(str[++i])))
      dbuf_putc(db, '\\');

    dbuf_putc(db, (r > 1 && r < 256) ? r : str[i]);
    i++;
  }
}

void
dbuf_put_escaped(DynBuf* db, const char* str, size_t len) {
  static const char tab[256] = {
      'x', 'x', 'x', 'x', 'x', 'x',  'x', 'x', 0x62, 0x74, 0x6e, 0x76, 0x66, 0x72, 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x',
      'x', 'x', 'x', 0,   0,   0,    0,   0,   0,    0,    0x27, 0,    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,    0,    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0x5c, 0,   0,   0,    0,    0,    0,    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,    'x',  0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,    0,    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,    0,    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,    0,    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,    0,   0,   0,    0,    0,    0,    0,    0,    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  };

  return dbuf_put_escaped_table(db, str, len, tab);
}

void
dbuf_put_value(DynBuf* db, JSContext* ctx, JSValueConst value) {
  const char* str;
  size_t len;
  str = JS_ToCStringLen(ctx, &len, value);
  dbuf_append(db, str, len);
  js_cstring_free(ctx, str);
}

int
dbuf_reserve_start(DynBuf* s, size_t len) {
  if(unlikely((s->size + len) > s->allocated_size)) {
    if(dbuf_realloc(s, s->size + len))
      return -1;
  }
  if(s->size > 0)
    memcpy(s->buf + len, s->buf, s->size);

  s->size += len;
  return 0;
}

size_t
dbuf_token_pop(DynBuf* db, char delim) {
  size_t n, p, len;
  len = db->size;
  for(n = db->size; n > 0;) {
    if((p = byte_rchr(db->buf, n, delim)) == n) {
      db->size = 0;
      break;
    }
    if(p > 0 && db->buf[p - 1] == '\\') {
      n = p - 1;
      continue;
    }
    db->size = p;
    break;
  }
  return len - db->size;
}

size_t
dbuf_token_push(DynBuf* db, const char* str, size_t len, char delim) {
  size_t pos;
  if(db->size)
    dbuf_putc(db, delim);

  pos = db->size;
  dbuf_put_escaped_pred(db, str, len, is_dot_char);
  return db->size - pos;
}

JSValue
dbuf_tostring_free(DynBuf* s, JSContext* ctx) {
  JSValue r;
  r = JS_NewStringLen(ctx, s->buf ? (const char*)s->buf : "", s->buf ? s->size : 0);
  dbuf_free(s);
  return r;
}

ssize_t
dbuf_load(DynBuf* s, const char* filename) {
  FILE* fp;
  size_t nbytes = 0;
  if((fp = fopen(filename, "rb"))) {
    char buf[4096];
    size_t r;
    while(!feof(fp)) {
      if((r = fread(buf, 1, sizeof(buf), fp)) == 0)
        return -1;
      dbuf_put(s, (uint8_t const*)buf, r);
      nbytes += r;
    }
    fclose(fp);
  }
  return nbytes;
}

int
dbuf_vprintf(DynBuf* s, const char* fmt, va_list ap) {
  char buf[128];
  int len;

  len = vsnprintf(buf, sizeof(buf), fmt, ap);
  if(len < sizeof(buf)) {
    /* fast case */
    return dbuf_put(s, (uint8_t*)buf, len);
  } else {
    if(dbuf_realloc(s, s->size + len + 1))
      return -1;
    vsnprintf((char*)(s->buf + s->size), s->allocated_size - s->size, fmt, ap);
    s->size += len;
  }
  return 0;
}

InputBuffer
js_input_buffer(JSContext* ctx, JSValueConst value) {
  InputBuffer ret = {0, 0, 0, &input_buffer_free_default, JS_UNDEFINED};
  int64_t offset = 0, length = INT64_MAX;

  offset_init(&ret.range);

  if(js_is_typedarray(value) || js_is_dataview(ctx, value)) {
    JSValue arraybuf, byteoffs, bytelen;
    arraybuf = JS_GetPropertyStr(ctx, value, "buffer");
    bytelen = JS_GetPropertyStr(ctx, value, "byteLength");
    if(JS_IsNumber(bytelen))
      JS_ToInt64(ctx, &length, bytelen);
    JS_FreeValue(ctx, bytelen);
    byteoffs = JS_GetPropertyStr(ctx, value, "byteOffset");
    if(JS_IsNumber(byteoffs))
      JS_ToInt64(ctx, &offset, byteoffs);
    JS_FreeValue(ctx, byteoffs);
    value = arraybuf;
  }

  if(js_value_isclass(ctx, value, JS_CLASS_ARRAY_BUFFER) || js_is_arraybuffer(ctx, value)) {
    ret.value = JS_DupValue(ctx, value);
    ret.data = JS_GetArrayBuffer(ctx, &ret.size, ret.value);
  } else {
    ret.value = JS_EXCEPTION;
    // JS_ThrowTypeError(ctx, "Invalid type for input buffer");
  }

  if(offset < 0)
    ret.range.offset = ret.size + offset % ret.size;
  else if(offset > ret.size)
    ret.range.offset = ret.size;
  else
    ret.range.offset = offset;

  if(length >= 0 && length < ret.size)
    ret.range.length = length;

  return ret;
}

#undef free

InputBuffer
js_input_chars(JSContext* ctx, JSValueConst value) {
  InputBuffer ret = {0, 0, 0, &input_buffer_free_default, JS_UNDEFINED};
  int64_t offset = 0, length = INT64_MAX;

  offset_init(&ret.range);

  if(JS_IsString(value)) {
    ret.data = (uint8_t*)JS_ToCStringLen(ctx, &ret.size, value);
    ret.value = JS_DupValue(ctx, value);
    ret.free = &input_buffer_free_default;
  } else {
    ret = js_input_buffer(ctx, value);
  }

  return ret;
}

InputBuffer
js_input_args(JSContext* ctx, int argc, JSValueConst argv[]) {
  InputBuffer input = js_input_chars(ctx, argv[0]);

  if(argc > 1)
    js_offset_length(ctx, input.size, argc - 1, argv + 1, &input.range);

  return input;
}

InputBuffer
js_output_args(JSContext* ctx, int argc, JSValueConst argv[]) {
  InputBuffer output = js_input_buffer(ctx, argv[0]);

  if(argc > 1)
    js_offset_length(ctx, output.size, argc - 1, argv + 1, &output.range);

  return output;
}

BOOL
input_buffer_valid(const InputBuffer* in) {
  return !JS_IsException(in->value);
}

InputBuffer
input_buffer_clone(const InputBuffer* in, JSContext* ctx) {
  InputBuffer ret = js_input_buffer(ctx, in->value);

  ret.pos = in->pos;
  ret.size = in->size;
  ret.free = in->free;

  return ret;
}

void
input_buffer_dump(const InputBuffer* in, DynBuf* db) {
  dbuf_printf(db, "(InputBuffer){ .data = %p, .size = %zu, .pos = %zu, .free = %p }", in->data, in->size, in->pos, in->free);
}

void
input_buffer_free(InputBuffer* in, JSContext* ctx) {
  if(in->data) {
    in->free(ctx, (const char*)in->data, in->value);
    in->data = 0;
    in->size = 0;
    in->pos = 0;
    in->value = JS_UNDEFINED;
  }
}

const uint8_t*
input_buffer_peek(InputBuffer* in, size_t* lenp) {
  input_buffer_peekc(in, lenp);
  return input_buffer_data(in) + in->pos;
}

const uint8_t*
input_buffer_get(InputBuffer* in, size_t* lenp) {
  size_t n;
  const uint8_t* ret;
  if(lenp == 0)
    lenp = &n;
  ret = input_buffer_peek(in, lenp);
  in->pos += *lenp;
  return ret;
}

const char*
input_buffer_currentline(InputBuffer* in, size_t* len) {
  size_t i;

  if((i = byte_rchr(input_buffer_data(in), in->pos, '\n')) < in->pos)
    i++;

  if(len)
    *len = in->pos - i;

  return (const char*)&input_buffer_data(in)[i];
}

size_t
input_buffer_column(InputBuffer* in, size_t* len) {
  size_t i;

  if((i = byte_rchr(input_buffer_data(in), in->pos, '\n')) < in->pos)
    i++;

  return in->pos - i;
}

int
js_offset_length(JSContext* ctx, int64_t size, int argc, JSValueConst argv[], OffsetLength* off_len_p) {
  int ret = 0;
  int64_t off = 0, len = size;

  if(argc >= 1 && JS_IsNumber(argv[0])) {
    if(!JS_ToInt64(ctx, &off, argv[0]))
      ret = 1;
  }
  if(argc >= 2 && JS_IsNumber(argv[1])) {
    if(!JS_ToInt64(ctx, &len, argv[1]))
      ret = 2;
  }

  /* if(off >= 0)
     off = MIN_NUM(off, size);
   else
  */
  if(size)
    off = ((off % size) + size) % size;

  if(len >= 0)
    len = MIN_NUM(len, size - off);
  else
    len = size - off;

  if(off_len_p) {
    off_len_p->offset = off;
    off_len_p->length = len;
  }
  return ret;
}

int
screen_size(int size[2]) {
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO csbi;

  GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
  size[0] = csbi.srWindow.Right - csbi.srWindow.Left + 1;
  size[1] = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
  return 0;

#elif defined(HAVE_TERMIOS_H)
  {
    struct winsize w = {.ws_col = -1, .ws_row = -1};

    if(isatty(STDIN_FILENO))
      ioctl(STDIN_FILENO, TIOCGWINSZ, &w);
    else if(isatty(STDOUT_FILENO))
      ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    else if(isatty(STDERR_FILENO))
      ioctl(STDERR_FILENO, TIOCGWINSZ, &w);

    size[0] = w.ws_col;
    size[1] = w.ws_row;
    return 0;
  }
#else
  size[0] = 80;
  size[1] = 25;
  return 0;
#endif
  return -1;
}

/**
 * @}
 */
