#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include "vector.h"
#include "debug.h"

/**
 * \defgroup ringbuffer Ring Buffer implementation
 * @{
 */
typedef union ringbuffer {
  struct {
    uint8_t* data;
    size_t size, capacity;
    BOOL error;
    DynBufReallocFunc* realloc_func;
    void* opaque;
    volatile uint32_t tail, head;
  };
  DynBuf dbuf;
  Vector vec;

} RingBuffer;

#define RINGBUFFER_INIT() \
  { \
    { 0, 0, 0, 0, &ringbuffer_default_realloc, 0 } \
  }

#define ringbuffer_init(rb, ctx) \
  do { \
    vector_init(&(rb)->vec, ctx); \
    vector_allocate(&(rb)->vec, 1, 1023); \
  } while(0)
#define ringbuffer_init_rt(rb, rt) \
  do { \
    vector_init_rt(&(rb)->vec, rt); \
    vector_allocate(&(rb)->vec, 1, 1023); \
  } while(0)
#define RINGBUFFER(ctx) \
  (RingBuffer) { \
    { 0, 0, 0, 0, (DynBufReallocFunc*)&js_realloc, ctx, 0, 0 } \
  }
#define RINGBUFFER_RT(rt) \
  (RingBuffer) { \
    { 0, 0, 0, 0, (DynBufReallocFunc*)&js_realloc_rt, rt } \
  }
#define ringbuffer_free(rb) vector_free(&(rb)->vec)
#define ringbuffer_begin(rb) (void*)&ringbuffer_tail(rb)
#define ringbuffer_end(rb) (void*)&ringbuffer_head(rb)
#define ringbuffer_head(rb) (rb)->data[(rb)->head]
#define ringbuffer_tail(rb) (rb)->data[(rb)->tail]

#define ringbuffer_empty(rb) ((rb)->tail == (rb)->head)
#define ringbuffer_full(rb) ((rb)->size == (rb)->head - (rb)->tail)
#define ringbuffer_wrapped(rb) ((rb)->head < (rb)->tail)
#define ringbuffer_headroom(rb) ((rb)->size - (rb)->head)
#define ringbuffer_avail(rb) ((rb)->size - ringbuffer_length(rb))
#define ringbuffer_length(rb) (ringbuffer_wrapped(rb) ? ((rb)->size - (rb)->tail) + (rb)->head : (rb)->head - (rb)->tail)
#define ringbuffer_is_continuous(rb) ((rb)->head >= (rb)->tail)
#define ringbuffer_wrap(rb, idx) ((idx) % (rb)->size)
#define ringbuffer_next(rb, ptr) (void*)(ringbuffer_wrap(rb, ((uint8_t*)(ptr + 1)) - (rb)->data) + (rb)->data)

void ringbuffer_reset(RingBuffer*);
void ringbuffer_queue(RingBuffer*, uint8_t data);
BOOL ringbuffer_dequeue(RingBuffer*, uint8_t* data);
ssize_t ringbuffer_write(RingBuffer*, const void* x, size_t len);
ssize_t ringbuffer_read(RingBuffer*, void* x, size_t len);
uint8_t* ringbuffer_peek(RingBuffer*, size_t index);
void ringbuffer_normalize(RingBuffer*);
BOOL ringbuffer_resize(RingBuffer*, size_t);
BOOL ringbuffer_allocate(RingBuffer*, size_t);
uint8_t* ringbuffer_reserve(RingBuffer* rb, size_t min_bytes);

/*static inline size_t
ringbuffer_length(RingBuffer* rb) {
  if(!ringbuffer_wrapped(rb))
    return rb->head - rb->tail;

  return (rb->size - rb->tail) + rb->head;
}*/

static inline size_t
ringbuffer_continuous(RingBuffer* rb) {
  if(ringbuffer_wrapped(rb))
    return rb->size - rb->tail;

  return ringbuffer_length(rb);
}

/*static inline uint32_t
ringbuffer_avail(RingBuffer* rb) {
  return rb->size - ringbuffer_length(rb);
}*/

/**
 * @}
 */
#endif /* defined(RINGBUFFER_H) */
