/*
 * The Lua binding of the session (ui/docs/lua-api.md): the bus_replay module, which every
 * Lua state CEL makes can require. ui_main sets the process's session before CEL runs.
 */

#ifndef UI_CEL_LUA_SESSION_H
#define UI_CEL_LUA_SESSION_H

#include <lua.h>

#include "ui/session/session.h"


void lua_session_set(struct session *s);
int luaopen_bus_replay(lua_State *L);

#endif
