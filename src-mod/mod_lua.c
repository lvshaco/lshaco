#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <string.h>
#include "shaco.h"
#include "lua_packer.h"

struct lua {
    lua_State *L;
};

struct lua *
lua_create() {
    struct lua *l = shaco_malloc(sizeof(*l));
    memset(l, 0, sizeof(*l));
    return l;
}

void
lua_free(struct lua *self) {
    if (self->L) {
        lua_close(self->L);
        self->L = NULL;
    }
    shaco_free(self);
}

static int                                        
_traceback(lua_State *L) {                        
    //const char *msg = lua_tostring(L, 1);
    const char *msg = luaL_tolstring(L, 1, NULL);
    if (msg) {
        luaL_traceback(L, L, msg, 1);
    } else {
        lua_pushliteral(L, "(no error message)"); 
    }                                     
    return 1;
}

int
lua_init(struct shaco_context *ctx, struct lua *self, const char *args) {
    const char *packagepath = shaco_optstr("packagepath", "./lib-lua/?.lso");

    lua_State *L = lua_newstate(shaco_lalloc, NULL);
    luaL_openlibs(L);
    if (lua_packer(ctx, L, packagepath)) return 1;
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "shaco_context");
    self->L = L;
    lua_pushcfunction(L, _traceback);

    const char *path = shaco_optstr("luapath", "./lua-shaco/?.lua"); 
    lua_pushstring(L, path);
    lua_setglobal(L, "LUA_PATH");
    const char *cpath = shaco_optstr("luacpath", "./lib-l/?.so");
    lua_pushstring(L, cpath);
    lua_setglobal(L, "LUA_CPATH");
    const char *modpath = shaco_optstr("luamodpath", "./lua-mod/?.lua");
    lua_pushstring(L, modpath);
    lua_setglobal(L, "LUA_MODPATH");
    
    const char *loader = shaco_optstr("lualoader", "./lua-shaco/loader.lua");
    int r = lua_loadfile(L, loader, packagepath, NULL, 0);
    if (r != LUA_OK) {
        shaco_error(ctx, "%s", lua_tostring(L, -1));
        lua_pop(L, 2);
        return 1;
    }
    lua_pushstring(L, args);
    r = lua_pcall(L, 1, 0, 1);
    if (r != LUA_OK) {
        shaco_error(ctx, "%s", lua_tostring(L, -1));
        lua_pop(L, 2);
        return 1;
    } else {
        lua_pop(L, 1);
        return 0;
    }
}
