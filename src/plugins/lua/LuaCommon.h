#pragma once

#include <sol/sol.hpp>

namespace mq::lua {
void DebugStackTrace(lua_State* L);
std::string join(sol::this_state L, std::string delim, sol::variadic_args va);
void delay(sol::object delayObj, sol::object conditionObj, sol::this_state s);
void doevents(sol::this_state s);
void addevent(std::string_view name, std::string_view expression, sol::function function, sol::this_state s);
void removeevent(std::string_view name, sol::this_state s);
}