#include "luaT.h"
#include "ringbuffer.h"
#include "serialize.h"
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <lualib.h>
#include "error.h"
#ifdef _OPENMP
#include <omp.h>
#endif

#define MAX_ARG_SIZE (16*1024)

typedef struct map_thread_t {
   pthread_t thread;
   ringbuffer_t *rb;
   int ret;
} map_thread_t;

typedef struct map_t {
   map_thread_t *threads;
   uint32_t num_threads;
} map_t;

#ifdef __APPLE__
/*
  Super lame, but with a low ulimit on files spawning a 100s
  of threads will crash in the require system with too many
  open files. So we only allow a single thread to be in the
  require system at any given time.
*/
static pthread_once_t _safe_require_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t _safe_require_mutex;

static void _safe_require_one_time_init_inner() {
   pthread_mutexattr_t mutex_attr;

   pthread_mutexattr_init(&mutex_attr);
   pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&_safe_require_mutex, &mutex_attr);
}

static void _safe_require_one_time_init() {
   pthread_once(&_safe_require_once, _safe_require_one_time_init_inner);
}

static int _safe_require(lua_State *L) {
   pthread_mutex_lock(&_safe_require_mutex);
   int n = lua_gettop(L);
   lua_getglobal(L, "_old_require");
   for (int i = 1; i <= n; i++) {
      lua_pushvalue(L, i);
   }
   int ret = lua_pcall(L, n, LUA_MULTRET, 0);
   pthread_mutex_unlock(&_safe_require_mutex);
   if (ret) {
      return luaL_error(L, lua_tostring(L, 1));
   } else {
      return lua_gettop(L) - n;
   }
}

static void _replace_require(lua_State *L) {
   lua_getglobal(L, "require");
   lua_setglobal(L, "_old_require");
   lua_register(L, "require", _safe_require);
}
#endif

typedef int (*ThreadInitFunc) (lua_State *L);
ThreadInitFunc _ipc_static_init_thread = NULL;

static void* thread_func(void *arg) {
#ifdef _OPENMP
   // prevent MKL/BLAS from crashing on the reader threads
   // its use of open-mp eats up way too many threads
   omp_set_num_threads(1);
#endif
   map_thread_t *map_thread = (map_thread_t *)arg;
   lua_State *L = luaL_newstate();
   luaL_openlibs(L);
#ifdef STATIC_TH
   if (_ipc_static_init_thread) {
      _ipc_static_init_thread(L);
   }
#else
#ifdef __APPLE__
   _replace_require(L);
#endif
   // in order to deserialize arguments we need torch and libipc
   if (luaL_loadstring(L, "require 'torch'; require 'libipc'")) {
      lua_close(L);
      return NULL;
   }
   if (lua_pcall(L, 0, 0, 0)) {
      lua_close(L);
      return NULL;
   }
#endif
   int top = lua_gettop(L);
   int i = 0;
   while (ringbuffer_peek(map_thread->rb)) {
      rb_load(L, map_thread->rb);
      i++;
   }
   map_thread->ret = lua_pcall(L, i - 1, LUA_MULTRET, 0);
   int k = lua_gettop(L) - top;
   for (int i = 1; i <= k; i++) {
      rb_save(L, top + i, map_thread->rb, 0);
   }
   lua_close(L);
   return 0;
}

int map_open(lua_State *L) {
#ifdef __APPLE__
   _safe_require_one_time_init();
#endif
   uint32_t num_threads = lua_tonumber(L, 1);
   if (lua_type(L, 2) != LUA_TFUNCTION) return LUA_HANDLE_ERROR_STR(L, "map arg #2 expected a function");
   map_thread_t *threads = (map_thread_t *)calloc(num_threads, sizeof(map_thread_t));
   int k = lua_gettop(L);
   for (uint32_t i = 0; i < num_threads; i++) {
      threads[i].rb = ringbuffer_create(MAX_ARG_SIZE);
      for (int j = 2; j <= k; j++) {
         rb_save(L, j, threads[i].rb, 0);
      }
      lua_pushinteger(L, i + 1);
      rb_save(L, k + 1, threads[i].rb, 0);
      lua_pop(L, 1);
      pthread_create(&threads[i].thread, NULL, thread_func, threads + i);
   }
   map_t *map = (map_t *)lua_newuserdata(L, sizeof(map_t));
   map->num_threads = num_threads;
   map->threads = threads;
   luaL_getmetatable(L, "ipc.map");
   lua_setmetatable(L, -2);
   return 1;
}

int map_join(lua_State *L) {
   int rc = 0;
   int err_rc = -1;
   map_t *map = (map_t *)lua_touserdata(L, 1);
   for (uint32_t i = 0; i < map->num_threads; i++) {
      if (map->threads[i].rb) {
         pthread_join(map->threads[i].thread, NULL);
         if (map->threads[i].ret) {
            err_rc = rc;
         }
         while (ringbuffer_peek(map->threads[i].rb)) {
            rb_load(L, map->threads[i].rb);
            rc++;
         }
         ringbuffer_destroy(map->threads[i].rb);
      }
   }
   free(map->threads);
   map->threads = NULL;
   map->num_threads = 0;
   if (err_rc >= 0) {
      return LUA_HANDLE_ERROR_STR(L, lua_tostring(L, err_rc - rc));
   }
   return rc;
}

int map_check_errors(lua_State *L) {
   map_t *map = (map_t *)lua_touserdata(L, 1);
   for (uint32_t i = 0; i < map->num_threads; i++) {
      if (map->threads[i].ret) {
         pthread_join(map->threads[i].thread, NULL);
         while (ringbuffer_peek(map->threads[i].rb)) {
            rb_load(L, map->threads[i].rb);
         }
         ringbuffer_destroy(map->threads[i].rb);
         map->threads[i].rb = NULL;
         return LUA_HANDLE_ERROR_STR(L, lua_tostring(L, -1));
      }
   }
   return 0;
}
