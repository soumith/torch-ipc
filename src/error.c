#include "luaT.h"
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>

#define ABS(a) (((a) >= 0) ? (a) : -(a))

int _ipc_log_error(int ret, const char* file, int line) {
   int pos_ret = ABS(ret);
   fprintf(stderr, "ERROR: (%s, %d): (%d, %s)\n", file, line, pos_ret, strerror(pos_ret));
   return pos_ret;
}

int _ipc_lua_error(lua_State *L, int ret, const char* file, int line) {
   int pos_ret = ABS(ret);
   return luaL_error(L, "ERROR: (%s, %d): (%d, %s)\n", file, line, pos_ret, strerror(pos_ret));
}

int _ipc_lua_error_str(lua_State *L, const char *str, const char* file, int line) {
   return luaL_error(L, "ERROR: (%s, %d): (%s)\n", file, line, str);
}

double _ipc_seconds() {
   struct timeval tv;
   gettimeofday(&tv, NULL);
   return tv.tv_sec + tv.tv_usec / 1e6;
}
