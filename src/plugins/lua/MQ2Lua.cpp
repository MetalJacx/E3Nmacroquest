/*
 * MacroQuest: The extension platform for EverQuest
 * Copyright (C) 2002-2021 MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "pch.h"
#include "LuaCommon.h"
#include "LuaThread.h"
#include "LuaEvent.h"
#include "LuaImGui.h"
#include "bindings/lua_MQTypeVar.h"
#include "bindings/lua_MQDataItem.h"
#include "bindings/lua_MQCommand.h"

#include <mq/Plugin.h>
#pragma comment(lib, "imgui")

#include <mq/utils/Args.h>
#include <fmt/format.h>
#include <fmt/chrono.h>

#include <imgui/ImGuiFileDialog.h>

#pragma warning( push )
#pragma warning( disable:4996 )
#include <yaml-cpp/yaml.h>
#pragma warning( pop )

#include <string>
#include <fstream>

PreSetup("MQ2Lua");
PLUGIN_VERSION(0.1);

// TODO: Add aggressive bind/event options that scriptwriters can set with functions
// TODO: Add OnExit callbacks (potentially both as an explicit argument to exit and a set callback)

// TODO: Add UI for start/stop/info/config

using MQ2Args = Args<&WriteChatf>;
using MQ2HelpArgument = HelpArgument;

namespace mq::lua {

// provide option strings here
static const std::string turboNum = "turboNum";
static const std::string luaDir = "luaDir";
static const std::string luaRequirePaths = "luaRequirePaths";
static const std::string dllRequirePaths = "dllRequirePaths";
static const std::string infoGC = "infoGC";
static const std::string squelchStatus = "squelchStatus";
static const std::string showMenu = "showMenu";

// configurable options, defaults provided where needed
static uint32_t s_turboNum = 500;
static std::string s_luaDir = "lua";
static std::string get_luaDir() { return (std::filesystem::path(gPathMQRoot) / s_luaDir).string(); }
static std::vector<std::string> s_luaRequirePaths;
static std::vector<std::string> s_dllRequirePaths;
static uint64_t s_infoGC = 3600000; // 1 hour
static bool s_squelchStatus = false;

// this is static and will never change
static std::string s_configPath = (std::filesystem::path(gPathConfig) / "MQ2Lua.yaml").string();
static YAML::Node s_configNode;

// this is for the imgui menu display
static bool s_showMenu = false;
static ImGuiFileDialog* s_scriptLaunchDialog = nullptr;
static ImGuiFileDialog* s_luaDirDialog = nullptr;

// use a vector for s_running because we need to iterate it every pulse, and find only if a command is issued
std::vector<std::shared_ptr<LuaThread>> s_running;

std::unordered_map<uint32_t, LuaThreadInfo> s_infoMap;

#pragma region Shared Function Definitions

void DebugStackTrace(lua_State* L)
{
	lua_Debug ar;
	lua_getstack(L, 1, &ar);
	lua_getinfo(L, "nSl", &ar);
	LuaError("%s: %s (%s)", ar.what, ar.name, ar.namewhat);
	LuaError("Line %i in %s", ar.currentline, ar.short_src);

	int top = lua_gettop(L);
	LuaError("---- Begin Stack (size: %i) ----", top);
	for (int i = top; i >= 1; i--)
	{
		int t = lua_type(L, i);
		switch (t)
		{
		case LUA_TSTRING:
			LuaError("%i -- (%i) ---- `%s'", i, i - (top + 1), lua_tostring(L, i));
			break;

		case LUA_TBOOLEAN:
			LuaError("%i -- (%i) ---- %s", i, i - (top + 1), lua_toboolean(L, i) ? "true" : "false");
			break;

		case LUA_TNUMBER:
			LuaError("%i -- (%i) ---- %g", i, i - (top + 1), lua_tonumber(L, i));
			break;

		case LUA_TUSERDATA:
			LuaError("%i -- (%i) ---- [%s]", i, i - (top + 1), luaL_tolstring(L, i, NULL));
			break;

		default:
			LuaError("%i -- (%i) ---- %s", i, i - (top + 1), lua_typename(L, t));
			break;
		}
	}
	LuaError("---- End Stack ----\n");
}

bool DoStatus()
{
	return !s_squelchStatus;
}

#pragma endregion

#pragma region TLO

class MQ2LuaInfoType* pLuaInfoType = nullptr;
class MQ2LuaInfoType : public MQ2Type
{
public:
	enum class Members
	{
		PID,
		Name,
		Path,
		Arguments,
		StartTime,
		EndTime,
		ReturnCount,
		Return,
		Status
	};

	MQ2LuaInfoType() : MQ2Type("luainfo")
	{
		ScopedTypeMember(Members, PID);
		ScopedTypeMember(Members, Name);
		ScopedTypeMember(Members, Path);
		ScopedTypeMember(Members, Arguments);
		ScopedTypeMember(Members, StartTime);
		ScopedTypeMember(Members, EndTime);
		ScopedTypeMember(Members, ReturnCount);
		ScopedTypeMember(Members, Return);
		ScopedTypeMember(Members, Status);
	};

	virtual bool GetMember(MQVarPtr VarPtr, const char* Member, char* Index, MQTypeVar& Dest) override
	{
		using namespace mq::datatypes;

		auto pMember = MQ2LuaInfoType::FindMember(Member);
		if (pMember == nullptr)
			return false;

		auto info = VarPtr.Get<LuaThreadInfo>();
		if (!info)
			return false;

		switch (static_cast<Members>(pMember->ID))
		{
		case Members::PID:
			Dest.Type = pIntType;
			Dest.Set(info->pid);
			return true;

		case Members::Name:
			Dest.Type = pStringType;
			strcpy_s(DataTypeTemp, MAX_STRING, info->name.c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::Path:
			Dest.Type = pStringType;
			strcpy_s(DataTypeTemp, MAX_STRING, info->path.c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::Arguments:
			Dest.Type = pStringType;
			strcpy_s(DataTypeTemp, MAX_STRING, join(info->arguments, ",").c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::StartTime:
			Dest.Type = pStringType;
			ctime_s(DataTypeTemp, MAX_STRING, &info->startTime);
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::EndTime:
			Dest.Type = pInt64Type;
			Dest.Set(info->endTime);
			return true;

		case Members::ReturnCount:
			Dest.Type = pIntType;
			Dest.Set(info->returnValues.size());
			return true;

		case Members::Return:
			Dest.Type = pStringType;
			if (info->returnValues.empty())
				return false;

			if (!Index || !Index[0])
			{
				strcpy_s(DataTypeTemp, MAX_STRING, join(info->returnValues, ",").c_str());
			}
			else
			{
				int index = GetIntFromString(Index, 0) - 1;
				if (index < 0 || index >= static_cast<int>(info->returnValues.size()))
					return false;

				strcpy_s(DataTypeTemp, MAX_STRING, info->returnValues.at(index).c_str());
			}

			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::Status:
			Dest.Type = pStringType;
			strcpy_s(DataTypeTemp, MAX_STRING, std::string(info->status_string()).c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		default:
			return false;
		}
	}

	bool ToString(MQVarPtr VarPtr, char* Destination) override
	{
		auto info = VarPtr.Get<LuaThreadInfo>();
		if (!info || info->returnValues.empty())
			return false;

		strcpy_s(Destination, MAX_STRING, join(info->returnValues, ",").c_str());
		return true;
	}

	virtual bool FromString(MQVarPtr& VarPtr, const char* Source) override
	{
		return false;
	}
};

//----------------------------------------------------------------------------

class MQ2LuaType* pLuaType = nullptr;
class MQ2LuaType : public MQ2Type
{
public:
	enum class Members
	{
		PIDs,
		Dir,
		Turbo,
		RequirePaths,
		CRequirePaths,
		Script
	};

	MQ2LuaType() : MQ2Type("lua")
	{
		ScopedTypeMember(Members, PIDs);
		ScopedTypeMember(Members, Dir);
		ScopedTypeMember(Members, Turbo);
		ScopedTypeMember(Members, RequirePaths);
		ScopedTypeMember(Members, CRequirePaths);
		ScopedTypeMember(Members, Script);
	}

	virtual bool GetMember(MQVarPtr VarPtr, const char* Member, char* Index, MQTypeVar& Dest) override
	{
		using namespace mq::datatypes;

		auto pMember = MQ2LuaType::FindMember(Member);
		if (pMember == nullptr)
			return false;

		switch (static_cast<Members>(pMember->ID))
		{
		case Members::PIDs:
		{
			Dest.Type = pStringType;
			std::vector<std::string> pids;
			std::transform(s_running.cbegin(), s_running.cend(), std::back_inserter(pids),
				[](const auto& thread) { return std::to_string(thread->pid); });
			strcpy_s(DataTypeTemp, join(pids, ",").c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;
		}
		case Members::Dir:
			Dest.Type = pStringType;
			strcpy_s(DataTypeTemp, get_luaDir().c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::Turbo:
			Dest.Type = pIntType;
			Dest.Set(s_turboNum);
			return true;

		case Members::RequirePaths:
			Dest.Type = pStringType;
			strcpy_s(DataTypeTemp, fmt::format("{}\\?.lua;{}", get_luaDir(),
				s_luaRequirePaths.empty() ? "" : join(s_luaRequirePaths, ";")).c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::CRequirePaths:
			Dest.Type = pStringType;
			strcpy_s(DataTypeTemp, fmt::format("{}\\?.dll;{}", get_luaDir(),
				s_dllRequirePaths.empty() ? "" : join(s_dllRequirePaths, ";")).c_str());
			Dest.Ptr = &DataTypeTemp[0];
			return true;

		case Members::Script:
		{
			Dest.Type = pLuaInfoType;
			if (!Index || !Index[0])
			{
				if (s_infoMap.empty())
					return false;

				// grab the latest start time that has an end time
				auto latest = s_infoMap.cbegin();
				for (auto it = s_infoMap.cbegin(); it != s_infoMap.cend(); ++it)
				{
					if (it->second.endTime > 0 && it->second.startTime > latest->second.startTime)
						latest = it;
				}

				if (latest->second.endTime > 0)
				{
					Dest.Set(latest->second);
					return true;
				}

				return false;
			}

			auto pid = GetIntFromString(Index, -1);
			auto it = s_infoMap.find(pid);
			if (it != s_infoMap.end())
			{
				Dest.Set(it->second);
				return true;
			}

			return false;
		}

		default:
			return false;
		}
	}

	bool ToString(MQVarPtr VarPtr, char* Destination) override
	{
		strcpy_s(Destination, MAX_STRING, "Lua");
		return true;
	}

	virtual bool FromString(MQVarPtr& VarPtr, const char* Source) override
	{
		return false;
	}

	static bool dataLua(const char* Index, MQTypeVar& Dest)
	{
		Dest.DWord = 1;
		Dest.Type = pLuaType;
		return true;
	}
};

#pragma endregion

#pragma region Commands

static uint32_t LuaRunCommand(const std::string& script, const std::vector<std::string>& args)
{
	namespace fs = std::filesystem;

	// Need to do this first to get the script path and compare paths instead of just the names
	// since there are multiple valid ways to name the same script
	auto script_path = fs::path{ get_luaDir() } / script;
	if (!script_path.has_extension()) script_path.replace_extension(".lua");

	std::error_code ec;
	if (!std::filesystem::exists(script_path, ec))
	{
		LuaError("Could not find script at path %s", script_path.c_str());
		return 0;
	}

	// methodology for duplicate scripts: 
	//   if a script with the same name is _currently_ running, inform and exit
	//   if a script with the same name _has previously_ run, drop from infoMap and run
	//   otherwise, run script as normal
	auto info_it = std::find_if(s_infoMap.begin(), s_infoMap.end(),
		[&script_path](const std::pair<uint32_t, mq::lua::LuaThreadInfo>& kv)
		{
			auto info_path = fs::path(kv.second.path);
			std::error_code ec;
			return fs::exists(info_path, ec) && fs::equivalent(info_path, script_path, ec);
		});

	if (info_it != s_infoMap.end() && info_it->second.status != LuaThreadStatus::Exited)
	{
		// script is currently running, inform and exit
		WriteChatStatus("Lua script %s is already running, not starting another instance.", script.c_str());
		return 0;
	}

	if (info_it != s_infoMap.end())
	{
		// script has previously run, simply erase it because we are going to get a new entry
		s_infoMap.erase(info_it);
	}

	auto entry = std::make_shared<LuaThread>(script, get_luaDir(), s_luaRequirePaths, s_dllRequirePaths);
	WriteChatStatus("Running lua script '%s' with PID %d", script.c_str(), entry->pid);
	s_running.emplace_back(entry); // this needs to be in the running vector before we run at all

	entry->RegisterLuaState(entry);
	auto result = entry->StartFile(get_luaDir(), s_turboNum, args);
	if (result)
	{
		result->status = LuaThreadStatus::Running;
		s_infoMap.emplace(result->pid, *result);
		return result->pid;
	}

	return 0;
}

static uint32_t LuaParseCommand(const std::string& script)
{
	auto info_it = std::find_if(s_infoMap.begin(), s_infoMap.end(),
		[](const std::pair<uint32_t, mq::lua::LuaThreadInfo>& kv)
		{ return kv.second.name == "lua parse"; });

	if (info_it != s_infoMap.end() && info_it->second.endTime == 0ULL)
	{
		// parsed script is currently running, inform and exit
		WriteChatStatus("Parsed Lua script is already running, not starting another instance.");
		return 0;
	}

	if (info_it != s_infoMap.end())
	{
		// always erase previous parse entries in the ps, it gets overcrowded otherwise
		s_infoMap.erase(info_it);
	}

	auto entry = std::make_shared<LuaThread>("lua parse", get_luaDir(), s_luaRequirePaths, s_dllRequirePaths);
	WriteChatStatus("Running lua string with PID %d", entry->pid);
	s_running.emplace_back(entry); // this needs to be in the running vector before we run at all

	// Create lua state with mq namespace already injected.
	entry->RegisterLuaState(entry, true);
	auto result = entry->StartString(s_turboNum, script);
	if (result)
	{
		result->status = LuaThreadStatus::Running;
		s_infoMap.emplace(result->pid, *result);
		return result->pid;
	}

	return 0;
}

static void LuaStopCommand(std::optional<std::string> script = std::nullopt)
{
	if (script)
	{
		auto thread_it = s_running.end();
		uint32_t pid = GetIntFromString(*script, 0UL);
		if (pid > 0UL)
		{
			thread_it = std::find_if(s_running.begin(), s_running.end(), [&pid](const auto& thread) -> bool
				{
					return thread->pid == pid;
				});
		}
		else
		{
			thread_it = std::find_if(s_running.begin(), s_running.end(), [&script](const auto& thread) -> bool
				{
					return thread->name == *script;
				});
		}

		if (thread_it != s_running.end())
		{
			// this will force the coroutine to yield, and removing this thread from the vector will cause it to gc
			(*thread_it)->YieldAt(0);
			(*thread_it)->thread.abandon();
			WriteChatStatus("Ending running lua script '%s' with PID %d", (*thread_it)->name.c_str(), (*thread_it)->pid);
		}
		else
		{
			WriteChatStatus("No lua script '%s' to end", *script);
		}
	}
	else
	{
		// kill all scripts
		for (auto& thread : s_running)
		{
			thread->YieldAt(0);
			thread->thread.abandon();
		}

		WriteChatStatus("Ending ALL lua scripts");
	}
}

static void LuaPauseCommand(std::optional<std::string> script = std::nullopt)
{
	if (script)
	{
		auto thread_it = s_running.end();
		uint32_t pid = GetIntFromString(*script, 0UL);
		if (pid > 0UL)
		{
			thread_it = std::find_if(s_running.begin(), s_running.end(), [&pid](const auto& thread) -> bool
				{
					return thread->pid == pid;
				});
		}
		else
		{
			thread_it = std::find_if(s_running.begin(), s_running.end(), [&script](const auto& thread) -> bool
				{
					return thread->name == *script;
				});
		}

		if (thread_it != s_running.end())
		{
			auto status = (*thread_it)->state->Pause(**thread_it, s_turboNum);
			auto info = s_infoMap.find((*thread_it)->pid);
			if (info != s_infoMap.end())
				info->second.status = status;
		}
		else
		{
			WriteChatStatus("No lua script '%s' to pause/resume", *script);
		}
	}
	else
	{
		// try to Get the user's intention here. If all scripts are running/paused, batch toggle state. If there are any running, assume we want to pause those only.
		if (std::find_if(s_running.cbegin(), s_running.cend(), [](const auto& thread) -> bool {
			return !thread->state->IsPaused();
			}) != s_running.cend())
		{
			// have at least one running script, so pause all running scripts
			for (auto& thread : s_running)
			{
				auto status = thread->state->Pause(*thread, s_turboNum);
				auto info = s_infoMap.find(thread->pid);
				if (info != s_infoMap.end())
					info->second.status = status;
			}

			WriteChatStatus("Pausing ALL running lua scripts");
		}
		else if (!s_running.empty())
		{
			// we have no running scripts, so restart all paused scripts
			for (auto& thread : s_running)
			{
				auto status = thread->state->Pause(*thread, s_turboNum);
				auto info = s_infoMap.find(thread->pid);
				if (info != s_infoMap.end())
					info->second.status = status;
			}

			WriteChatStatus("Resuming ALL paused lua scripts");
		}
		else
		{
			// there are no scripts running or paused, just inform the user of that
			WriteChatStatus("There are no running OR paused lua scripts to pause/resume");
		}
	}
}

static void WriteSettings()
{
	std::fstream file(s_configPath, std::ios::out);
	if (!s_configNode.IsNull())
	{
		YAML::Emitter y_out;
		y_out << s_configNode;

		file << y_out.c_str();
	}
}

static void ReadSettings()
{
	try
	{
		s_configNode = YAML::LoadFile(s_configPath);
	}
	catch (const YAML::ParserException& e)
	{
		// failed to parse, notify and return
		WriteChatf("Failed to parse YAML in %s with %s", s_configPath.c_str(), e.what());
		return;
	}
	catch (const YAML::BadFile&)
	{
		// if we can't read the file, then try to write it with an empty config
		WriteSettings();
		return;
	}

	s_turboNum = s_configNode[turboNum].as<uint32_t>(s_turboNum);

	s_luaDir = s_configNode[luaDir].as<std::string>(s_luaDir);

	std::error_code ec;
	if (!std::filesystem::exists(get_luaDir(), ec) && !std::filesystem::create_directories(get_luaDir(), ec))
	{
		WriteChatf("Failed to open or create directory at %s. Scripts will not run.", get_luaDir().c_str());
		WriteChatf("Error was %s", ec.message().c_str());
	}

	s_luaRequirePaths.clear();
	if (s_configNode[luaRequirePaths].IsSequence()) // if this is not a sequence, add nothing
	{
		for (const auto& path : s_configNode[luaRequirePaths])
		{
			auto fin_path = std::filesystem::path(gPathMQRoot) / std::filesystem::path(path.as<std::string>());
			s_luaRequirePaths.emplace_back(fin_path.string());
		}
	}

	s_dllRequirePaths.clear();
	if (s_configNode[dllRequirePaths].IsSequence()) // if this is not a sequence, add nothing
	{
		for (const auto& path : s_configNode[dllRequirePaths])
		{
			auto fin_path = std::filesystem::path(gPathMQRoot) / std::filesystem::path(path.as<std::string>());
			s_dllRequirePaths.emplace_back(fin_path.string());
		}
	}

	auto GC_interval = s_configNode[infoGC].as<std::string>(std::to_string(s_infoGC));
	trim(GC_interval);
	if (GC_interval.length() > 1 && GC_interval.find_first_not_of("0123456789") == std::string::npos)
		std::from_chars(GC_interval.data(), GC_interval.data() + GC_interval.size(), s_infoGC);
	else if (GC_interval.length() > 1 && GC_interval.compare(GC_interval.length() - 1, 1, "h") == 0)
	{
		auto result = 0ULL;
		std::from_chars(GC_interval.data(), GC_interval.data() + GC_interval.size() - 1, result);
		if (result >= 0) s_infoGC = result * 3600000;
	}
	else if (GC_interval.length() > 1 && GC_interval.compare(GC_interval.length() - 1, 1, "m") == 0)
	{
		auto result = 0ULL;
		std::from_chars(GC_interval.data(), GC_interval.data() + GC_interval.size() - 1, result);
		if (result >= 0) s_infoGC = result * 60000;
	}
	else if (GC_interval.length() > 2 && GC_interval.compare(GC_interval.length() - 2, 2, "ms") == 0)
		std::from_chars(GC_interval.data(), GC_interval.data() + GC_interval.size() - 2, s_infoGC);
	else if (GC_interval.length() > 1 && GC_interval.compare(GC_interval.length() - 1, 1, "s") == 0)
	{
		auto result = 0ULL;
		std::from_chars(GC_interval.data(), GC_interval.data() + GC_interval.size() - 1, result);
		if (result >= 0) s_infoGC = result * 1000;
	}

	s_squelchStatus = s_configNode[squelchStatus].as<bool>(s_squelchStatus);

	s_showMenu = s_configNode[showMenu].as<bool>(s_showMenu);

	WriteSettings();
}

static void LuaConfCommand(const std::string& setting, const std::string& value)
{
	if (!value.empty())
	{
		WriteChatStatus("Lua setting %s to %s and saving...", setting.c_str(), value.c_str());
		s_configNode[setting] = value;
		WriteSettings();
		ReadSettings();
	}
	else if (s_configNode[setting])
	{
		WriteChatStatus("Lua setting %s is set to %s.", setting.c_str(), s_configNode[setting].as<std::string>().c_str());
	}
	else
	{
		WriteChatStatus("Lua setting %s is not set (using default).", setting.c_str());
	}
}

static void LuaPSCommand(const std::vector<std::string>& filters = {})
{
	auto predicate = [&filters](const std::pair<uint32_t, LuaThreadInfo>& pair)
	{
		if (filters.empty())
			return pair.second.status == LuaThreadStatus::Running || pair.second.status == LuaThreadStatus::Paused;

		auto status = pair.second.status_string();

		return std::find(filters.begin(), filters.end(), status) != filters.end();
	};
	
	WriteChatStatus("|  PID  |    NAME    |    START    |     END     |   STATUS   |");

	for (const auto& info : s_infoMap)
	{
		if (predicate(info))
		{
			fmt::memory_buffer line;
			fmt::format_to(line, "|{:^7}|{:^12}|{:^13}|{:^13}|{:^12}|",
				info.first,
				info.second.name.length() > 12 ? info.second.name.substr(0, 9) + "..." : info.second.name,
				info.second.startTime,
				info.second.endTime,
				info.second.status);
			WriteChatStatus("%.*s", line.size(), line.data());
		}
	}
}

static void LuaInfoCommand(const std::optional<std::string>& script = std::nullopt)
{
	if (script)
	{
		auto thread_it = s_infoMap.end();
		uint32_t pid = GetIntFromString(*script, 0UL);
		if (pid > 0UL)
		{
			thread_it = std::find_if(s_infoMap.begin(), s_infoMap.end(), [&pid](const auto& kv) -> bool
				{
					return kv.first == pid;
				});
		}
		else
		{
			thread_it = std::find_if(s_infoMap.begin(), s_infoMap.end(), [&script](const auto& kv) -> bool
				{
					return kv.second.name == *script;
				});
		}

		if (thread_it != s_infoMap.end())
		{
			fmt::memory_buffer line;
			fmt::format_to(
				line,
				"pid: {}\nname: {}\npath: {}\narguments: {}\nstartTime: {}\nendTime: {}\nreturnValues: {}\nstatus: {}",
				thread_it->second.pid,
				thread_it->second.name,
				thread_it->second.path,
				join(thread_it->second.arguments, ", "),
				thread_it->second.startTime,
				thread_it->second.endTime,
				join(thread_it->second.returnValues, ", "),
				thread_it->second.status);

			WriteChatStatus("%.*s", line.size(), line.data());
		}
		else
		{
			WriteChatStatus("No lua script '%s'", *script);
		}
	}
	else
	{
		WriteChatStatus("|  PID  |    NAME    |    START    |     END     |   STATUS   |");

		for (const auto& info : s_infoMap)
		{
			fmt::memory_buffer line;
			fmt::format_to(line, "|{:^7}|{:^12}|{:^13}|{:^13}|{:^12}|",
				info.first,
				info.second.name.length() > 12 ? info.second.name.substr(0, 9) + "..." : info.second.name,
				info.second.startTime,
				info.second.endTime,
				info.second.status);
			WriteChatStatus("%.*s", line.size(), line.data());
		}
	}
}

static void LuaGuiCommand()
{
	s_showMenu = !s_showMenu;
	s_configNode[showMenu] = s_showMenu;
}

void LuaCommand(SPAWNINFO* pChar, char* Buffer)
{
	MQ2Args arg_parser("MQ2Lua: A lua script binding plugin.");
	arg_parser.Prog("/lua");
	arg_parser.RequireCommand(false);
	args::Group commands(arg_parser, "", args::Group::Validators::AtMostOne);

	args::Command run(commands, "run", "run lua script from file location",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::AllChildGroups);
			args::Positional<std::string> script(arguments, "script", "the name of the lua script to run. will automatically append .lua extension if no extension specified.");
			args::PositionalList<std::string> script_args(arguments, "args", "optional arguments to pass to the lua script.");
			MQ2HelpArgument h(arguments);
			parser.Parse();
			
			if (script) LuaRunCommand(script.Get(), script_args.Get());
		});

	args::Command parse(commands, "parse", "parse a lua string with an available mq namespace",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::DontCare);
			args::PositionalList<std::string> script(arguments, "script", "the text of the lua script to run");
			MQ2HelpArgument h(arguments);
			parser.Parse();

			if (script) LuaParseCommand(join(script.Get(), " "));
		});

	args::Command stop(commands, "stop", "stop one or all running lua scripts",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::AtMostOne);
			args::Positional<std::string> script(arguments, "process", "optional parameter to specify a PID or name of script to stop, if not specified will stop all running scripts.");
			MQ2HelpArgument h(arguments);
			parser.Parse();

			if (script) LuaStopCommand(script.Get());
			else LuaStopCommand();
		});
	stop.RequireCommand(false);

	args::Command pause(commands, "pause", "pause one or all running lua scripts",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::AtMostOne);
			args::Positional<std::string> script(arguments, "process", "optional parameter to specify a PID or name of script to pause, if not specified will pause all running scripts.");
			MQ2HelpArgument h(arguments);
			parser.Parse();

			if (script) LuaPauseCommand(script.Get());
			else LuaPauseCommand();
		});
	pause.RequireCommand(false);

	args::Command conf(commands, "conf", "set or view configuration variable",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::AtLeastOne);
			args::Positional<std::string> setting(arguments, "setting", "The setting to display/set");
			args::PositionalList<std::string> value(arguments, "value", "An optional parameter to specify the value to set");
			MQ2HelpArgument h(arguments);
			parser.Parse();

			if (setting) LuaConfCommand(setting.Get(), join(value.Get(), " "));
		});

	args::Command reloadconf(commands, "reloadconf", "reload configuration",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::DontCare);
			MQ2HelpArgument h(arguments);
			parser.Parse();

			WriteChatStatus("Reloading lua config.");
			ReadSettings();
		});

	args::Command ps(commands, "ps", "ps-like process listing",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::AtMostOne);
			args::PositionalList<std::string> filters(arguments, "filters", "optional parameters to specify status filters. Defaults to RUNNING or PAUSED.");
			MQ2HelpArgument h(arguments);
			parser.Parse();

			LuaPSCommand(filters.Get());
		});

	args::Command info(commands, "info", "info for a process",
		[](args::Subparser& parser)
		{
			args::Group arguments(parser, "", args::Group::Validators::AtMostOne);
			args::Positional<std::string> script(arguments, "process", "optional parameter to specify a PID or name of script to get info for, if not specified will return table of all scripts.");
			MQ2HelpArgument h(arguments);
			parser.Parse();

			if (script) LuaInfoCommand(script.Get());
			else LuaInfoCommand();
		});

	args::Command gui(commands, "gui", "toggle the lua GUI",
		[](args::Subparser& parser)
		{
			parser.Parse();
			LuaGuiCommand();
		});

	MQ2HelpArgument h(commands);

	auto args = allocate_args(Buffer);
	try
	{
		arg_parser.ParseArgs(args);
	}
	catch (const args::Help&)
	{
		arg_parser.Help();
	}
	catch (const args::Error& e)
	{
		WriteChatColor(e.what());
	}

	if (args.empty())
	{
		arg_parser.Help();
	}
}

#pragma endregion

} // namespace mq::lua


#pragma region GUI

static void DrawLuaSettings()
{
	using namespace mq::lua;

	ImGui::BeginChild("##luasettings", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4), false);

	bool squelch = s_configNode[squelchStatus].as<bool>(s_squelchStatus);
	if (ImGui::Checkbox("Suppress Lua Messages", &squelch))
	{
		s_squelchStatus = squelch;
		s_configNode[squelchStatus] = s_squelchStatus;
	}

	ImGui::SameLine();

	bool showgui = s_configNode[showMenu].as<bool>(s_showMenu);
	if (ImGui::Checkbox("Show Lua GUI", &showgui))
	{
		s_showMenu = showgui;
		s_configNode[showMenu] = s_showMenu;
	}

	ImGui::NewLine();

	ImGui::Text("Turbo Num:");
	uint32_t turbo_selected = s_configNode[turboNum].as<uint32_t>(s_turboNum), turbo_min = 100U, turbo_max = 1000U;
	if (ImGui::SliderScalar(
		"##turboNumslider",
		ImGuiDataType_U32,
		&turbo_selected,
		&turbo_min,
		&turbo_max,
		"%u Instructions per Frame",
		ImGuiSliderFlags_None))
	{
		s_turboNum = turbo_selected;
		s_configNode[turboNum] = s_turboNum;
	}

	ImGui::NewLine();

	ImGui::Text("Lua Directory:");
	auto dirDisplay = s_configNode[luaDir].as<std::string>(s_luaDir);
	ImGui::InputText("##luadirname", &dirDisplay[0], dirDisplay.size(), ImGuiInputTextFlags_ReadOnly);
	if (ImGui::Button("Choose...") && s_luaDirDialog != nullptr)
	{
		IGFD_OpenDialog2(
			s_luaDirDialog,
			"ChooseLuaDirKey",
			"Select Lua Directory",
			nullptr,
			(std::string(gPathMQRoot) + "/").c_str(),
			1,
			nullptr,
			ImGuiFileDialogFlags_None
		);
	}

	if (IGFD_DisplayDialog(s_luaDirDialog, "ChooseLuaDirKey", ImGuiWindowFlags_None, ImVec2(350, 350), ImVec2(FLT_MAX, FLT_MAX)))
	{
		if (IGFD_IsOk(s_luaDirDialog))
		{
			std::shared_ptr<char[]> selected_path(IGFD_GetCurrentPath(s_luaDirDialog), IGFD_DestroyString);

			std::error_code ec;
			if (selected_path && std::filesystem::exists(selected_path.get(), ec))
			{
				auto mq_path = std::filesystem::canonical(std::filesystem::path(gPathMQRoot), ec).string();
				auto lua_path = std::filesystem::canonical(std::filesystem::path(selected_path.get()), ec).string();

				auto [mqEnd, luaEnd] = std::mismatch(mq_path.begin(), mq_path.end(), lua_path.begin());

				auto clean_name = [](std::string_view s)
				{
					s.remove_prefix(std::min(s.find_first_not_of("\\"), s.size()));
					return std::string(s);
				};

				auto lua_name = mqEnd != mq_path.end()
					? lua_path
					: clean_name(std::string(luaEnd, lua_path.end()));

				s_luaDir = lua_name;
				s_configNode[luaDir] = s_luaDir;
			}
		}

		IGFD_CloseDialog(s_luaDirDialog);
	}

	ImGui::NewLine();

	ImGui::Text("Process Info Garbage Collect Time:");
	float gc_selected = s_configNode[infoGC].as<uint64_t>(s_infoGC) / 60000.f;
	if (ImGui::SliderFloat("##infoGCslider", &gc_selected, 0.f, 300.f, "%.3f minutes", ImGuiSliderFlags_None))
	{
		s_infoGC = static_cast<uint64_t>(gc_selected * 60000);
		s_configNode[infoGC] = s_infoGC;
	}

	ImGui::NewLine();

	if (ImGui::CollapsingHeader("Lua Require Paths:"))
	{
		if (ImGui::ListBoxHeader("##luarequirepaths"))
		{
			if (s_configNode[luaRequirePaths].IsSequence())
			{
				std::optional<size_t> to_remove = std::nullopt;
				size_t idx = 0;
				for (const auto& path : s_configNode[luaRequirePaths])
				{
					ImGui::Text(path.as<std::string>().c_str());
					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
						ImGui::TextUnformatted(path.as<std::string>().c_str());
						ImGui::PopTextWrapPos();
						ImGui::EndTooltip();
					}

					ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
					if (ImGui::Button((std::string("X##lua") + std::to_string(idx)).c_str(), ImVec2(0, ImGui::GetFrameHeight())))
					{
						to_remove = idx;
					}

					++idx;
				}

				if (to_remove)
				{
					s_luaRequirePaths.clear();
					s_configNode[luaRequirePaths].remove(*to_remove);
					for (const auto& path : s_configNode[luaRequirePaths])
					{
						auto fin_path = std::filesystem::path(gPathMQRoot) / std::filesystem::path(path.as<std::string>());
						s_luaRequirePaths.emplace_back(fin_path.string());
					}
				}
			}
			ImGui::ListBoxFooter();

			static char lua_req_buf[256] = { 0 };
			if (ImGui::InputText("##luarequireadd", lua_req_buf, 256, ImGuiInputTextFlags_EnterReturnsTrue) && strlen(lua_req_buf) > 0)
			{
				s_configNode[luaRequirePaths].push_back<std::string>(lua_req_buf);
				auto fin_path = std::filesystem::path(gPathMQRoot) / std::filesystem::path(lua_req_buf);
				s_luaRequirePaths.emplace_back(fin_path.string());
				memset(lua_req_buf, 0, 256);
			}
		}
	}

	ImGui::NewLine();

	if (ImGui::CollapsingHeader("DLL Require Paths:"))
	{
		if (ImGui::ListBoxHeader("##dllrequirepaths"))
		{
			if (s_configNode[dllRequirePaths].IsSequence())
			{
				std::optional<size_t> to_remove = std::nullopt;
				size_t idx = 0;
				for (const auto& path : s_configNode[dllRequirePaths])
				{
					ImGui::Text(path.as<std::string>().c_str());
					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
						ImGui::TextUnformatted(path.as<std::string>().c_str());
						ImGui::PopTextWrapPos();
						ImGui::EndTooltip();
					}

					ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
					if (ImGui::Button((std::string("X##dll") + std::to_string(idx)).c_str(), ImVec2(0, ImGui::GetFrameHeight())))
					{
						to_remove = idx;
					}

					++idx;
				}

				if (to_remove)
				{
					s_dllRequirePaths.clear();
					s_configNode[dllRequirePaths].remove(*to_remove);
					for (const auto& path : s_configNode[dllRequirePaths])
					{
						auto fin_path = std::filesystem::path(gPathMQRoot) / std::filesystem::path(path.as<std::string>());
						s_dllRequirePaths.emplace_back(fin_path.string());
					}
				}
			}
			ImGui::ListBoxFooter();

			static char dll_req_buf[256] = { 0 };
			if (ImGui::InputText("##dllrequireadd", dll_req_buf, 256, ImGuiInputTextFlags_EnterReturnsTrue) && strlen(dll_req_buf) > 0)
			{
				s_configNode[dllRequirePaths].push_back<std::string>(dll_req_buf);
				auto fin_path = std::filesystem::path(gPathMQRoot) / std::filesystem::path(dll_req_buf);
				s_dllRequirePaths.emplace_back(fin_path.string());
				memset(dll_req_buf, 0, 256);
			}
		}
	}

	ImGui::EndChild();

	if (ImGui::Button("Write Config"))
	{
		WriteSettings();
	}
}

#pragma endregion


/**
 * @fn InitializePlugin
 *
 * This is called once on plugin initialization and can be considered the startup
 * routine for the plugin.
 */
PLUGIN_API void InitializePlugin()
{
	using namespace mq::lua;

	DebugSpewAlways("MQ2Lua::Initializing version %f", MQ2Version);

	ReadSettings();

	AddCommand("/lua", LuaCommand);

	pLuaInfoType = new MQ2LuaInfoType;
	pLuaType = new MQ2LuaType;
	AddMQ2Data("Lua", &MQ2LuaType::dataLua);

	s_scriptLaunchDialog = IGFD_Create();
	AddCascadeMenuItem("MQ2Lua", LuaGuiCommand, -1);

	s_luaDirDialog = IGFD_Create();
	AddSettingsPanel("plugins/MQ2Lua", DrawLuaSettings);
}

/**
 * @fn ShutdownPlugin
 *
 * This is called once when the plugin has been asked to shutdown.  The plugin has
 * not actually shut down until this completes.
 */
PLUGIN_API void ShutdownPlugin()
{
	using namespace mq::lua;

	DebugSpewAlways("MQ2Lua::Shutting down");

	RemoveCommand("/lua");

	RemoveMQ2Data("Lua");
	delete pLuaType;
	delete pLuaInfoType;

	RemoveCascadeMenuItem("MQ2Lua");
	if (s_scriptLaunchDialog != nullptr) IGFD_Destroy(s_scriptLaunchDialog);

	RemoveSettingsPanel("plugins/MQ2Lua");
	if (s_luaDirDialog != nullptr) IGFD_Destroy(s_luaDirDialog);
}

/**
 * @fn OnPulse
 *
 * This is called each time MQ2 goes through its heartbeat (pulse) function.
 *
 * Because this happens very frequently, it is recommended to have a timer or
 * counter at the start of this Call to limit the amount of times the code in
 * this section is executed.
 */
PLUGIN_API void OnPulse()
{
	using namespace mq::lua;

	s_running.erase(std::remove_if(s_running.begin(), s_running.end(), [](const auto& thread) -> bool
		{
			auto result = thread->coroutine.status() == sol::call_status::yielded 
				? thread->Run(s_turboNum)
				: std::make_pair(static_cast<sol::thread_status>(thread->coroutine.status()), std::nullopt);

			if (result.first != sol::thread_status::yielded)
			{
				WriteChatStatus("Ending lua script %s with PID %d and status %d", thread->name.c_str(), thread->pid, static_cast<int>(result.first));
				auto fin_it = s_infoMap.find(thread->pid);
				if (fin_it != s_infoMap.end())
				{
					if (result.second)
						fin_it->second.SetResult(*result.second);
					else
						fin_it->second.EndRun();
				}

				return true;
			}

			return false;
		}), s_running.end());

	if (s_infoGC > 0)
	{
		auto now_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		static auto last_check_time = now_time;
		if (now_time >= last_check_time + static_cast<time_t>(s_infoGC))
		{
			// this doesn't need to be super tight, no one should be depending on this clearing objects at exactly the GC
			// interval, so just clear out anything that existed last time we checked.
			for (auto it = s_infoMap.begin(); it != s_infoMap.end();)
			{
				if (it->second.endTime > 0 && it->second.endTime <= last_check_time)
					it = s_infoMap.erase(it);
				else
					++it;
			}

			last_check_time = now_time;
		}
	}
}

/**
 * @fn OnUpdateImGui
 *
 * This is called each time that the ImGui Overlay is rendered. Use this to render
 * and update plugin specific widgets.
 *
 * Because this happens extremely frequently, it is recommended to move any actual
 * work to a separate Call and use this only for updating the display.
 */
PLUGIN_API void OnUpdateImGui()
{
	using namespace mq::lua;

	// update any script-defined windows first
	for (const auto& thread : s_running)
	{
		thread->imguiProcessor->Pulse();
	}

	// now update the lua menu window
	ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_FirstUseEver);
	if (s_showMenu && ImGui::Begin("MQ2Lua", &s_showMenu, ImGuiWindowFlags_None))
	{
		static bool show_running = true;
		static bool show_paused = true;
		static bool show_exited = false;

		auto should_show = [](const LuaThreadInfo& info)
		{
			if (info.status == LuaThreadStatus::Exited)
				return show_exited;

			if (info.status == LuaThreadStatus::Paused)
				return show_paused;

			return show_running;
		};

		ImGui::BeginGroup();
		static uint32_t selected_pid = 0;
		{
			ImGui::BeginChild("process list", ImVec2(150, -ImGui::GetFrameHeightWithSpacing() - 4), true);
			std::vector<uint32_t> running;
			std::vector<uint32_t> paused;
			std::vector<uint32_t> exited;
			for (const auto& info : s_infoMap)
			{
				if (info.second.status == LuaThreadStatus::Exited)
					exited.emplace_back(info.first);
				else if (info.second.status == LuaThreadStatus::Paused)
					paused.emplace_back(info.first);
				else
					running.emplace_back(info.first);
			}

			if (ImGui::CollapsingHeader("RUNNING"))
			{
				show_running = true;
				for (auto pid : running)
				{
					const auto& info = s_infoMap[pid];
					if (ImGui::Selectable(info.name.c_str(), selected_pid == info.pid))
						selected_pid = info.pid;
				}
			}
			else
			{
				show_running = false;
			}

			if (ImGui::CollapsingHeader("PAUSED"))
			{
				show_paused = true;
				for (auto pid : paused)
				{
					const auto& info = s_infoMap[pid];
					if (ImGui::Selectable(info.name.c_str(), selected_pid == info.pid))
						selected_pid = info.pid;
				}
			}
			else
			{
				show_paused = false;
			}

			if (ImGui::CollapsingHeader("EXITED"))
			{
				show_exited = true;
				for (auto pid : exited)
				{
					const auto& info = s_infoMap[pid];
					if (ImGui::Selectable(info.name.c_str(), selected_pid == info.pid))
						selected_pid = info.pid;
				}
			}
			else
			{
				show_exited = false;
			}

			ImGui::EndChild();
		}
		ImGui::EndGroup();

		ImGui::SameLine();

		ImGui::BeginGroup();
		const auto& info = s_infoMap.find(selected_pid);
		if (info != s_infoMap.end() && should_show(info->second))
		{
			ImGui::BeginChild("process view", ImVec2(0, -2 * ImGui::GetFrameHeightWithSpacing() - 4)); // Leave room for 1 line below us
			if (ImGui::CollapsingHeader(" PID", ImGuiTreeNodeFlags_Leaf))
			{
				ImGui::Text(" %u", info->second.pid);
			}

			if (ImGui::CollapsingHeader(" Name", ImGuiTreeNodeFlags_Leaf))
			{
				ImGui::Text(" %s", info->second.name.c_str());
			}

			if (!info->second.arguments.empty() && ImGui::CollapsingHeader(" Arguments", ImGuiTreeNodeFlags_Leaf))
			{
				ImGui::Text(" %s", join(info->second.arguments, ", ").c_str());
			}

			if (ImGui::CollapsingHeader(" Status", ImGuiTreeNodeFlags_Leaf))
			{
				auto status = info->second.status_string();
				ImGui::Text(" %.*s", status.size(), status.data());
			}

			if (ImGui::CollapsingHeader(" Path", ImGuiTreeNodeFlags_Leaf))
			{
				ImGui::TextWrapped("%s", info->second.path.c_str());
			}

			if (ImGui::CollapsingHeader(" Start Time", ImGuiTreeNodeFlags_Leaf))
			{
				tm ts;
				localtime_s(&ts, &info->second.startTime);
				ImGui::Text(" %s", fmt::format("{:%a, %b %d @ %I:%M:%S %p}", ts).c_str());
			}

			if (info->second.endTime > 0 && ImGui::CollapsingHeader(" End Time", ImGuiTreeNodeFlags_Leaf))
			{
				tm ts;
				localtime_s(&ts, &info->second.endTime);
				ImGui::Text(" %s", fmt::format("{:%a, %b %d @ %I:%M:%S %p}", ts).c_str());
			}


			if (!info->second.returnValues.empty() && ImGui::CollapsingHeader(" Return Values", ImGuiTreeNodeFlags_Leaf))
			{
				ImGui::Text(" %s", join(info->second.returnValues, ", ").c_str());
			}

			ImGui::EndChild();

			if (info->second.status != LuaThreadStatus::Exited)
			{
				if (ImGui::Button("Stop"))
				{
					LuaStopCommand(fmt::format("{}", info->second.pid));
				}

				ImGui::SameLine();

				if (ImGui::Button(info->second.status == LuaThreadStatus::Paused ? "Resume" : "Pause"))
				{
					LuaPauseCommand(fmt::format("{}", info->second.pid));
				}
			}
			else
			{
				if (ImGui::Button("Restart"))
				{
					if (info->second.name == "lua parse")
					{
						std::string script(info->second.path);
						selected_pid = LuaParseCommand(script);
					}
					else
					{
						// need to copy these because the run command will junk the info
						std::string script(info->second.name);
						std::vector<std::string> args(info->second.arguments);
						selected_pid = LuaRunCommand(script, args);
					}
				}
			}

		}
		else
		{
			selected_pid = 0;
		}
		ImGui::EndGroup();

		ImGui::Spacing();

		static char args[MAX_STRING] = { 0 };
		auto args_entry = [](const char* vFilter, void* vUserDatas, bool* vCantContinue) -> void
		{
			ImGui::InputText("args", (char*)vUserDatas, MAX_STRING);
		};

		if (ImGui::Button("Launch Script...", ImVec2(-1, 0)) && s_scriptLaunchDialog != nullptr)
		{
			IGFD_OpenPaneDialog2(
				s_scriptLaunchDialog,
				"ChooseScriptKey",
				"Select Lua Script to Run",
				".lua",
				(get_luaDir() + "/").c_str(),
				args_entry,
				350,
				1,
				static_cast<void*>(args),
				ImGuiFileDialogFlags_None
			);
		}

		if (IGFD_DisplayDialog(s_scriptLaunchDialog, "ChooseScriptKey", ImGuiWindowFlags_NoCollapse, ImVec2(700, 350), ImVec2(FLT_MAX, FLT_MAX)))
		{
			if (IGFD_IsOk(s_scriptLaunchDialog))
			{
				auto selection = IGFD_GetSelection(s_scriptLaunchDialog);
				auto selected_file = selection.table->filePathName;

				std::error_code ec;
				if (selected_file != nullptr && std::filesystem::exists(selected_file, ec))
				{
					// make these both canonical to ensure we get a correct comparison
					auto lua_path = std::filesystem::canonical(std::filesystem::path(get_luaDir()), ec).string();
					auto script_path = std::filesystem::canonical(
						std::filesystem::path(selected_file), ec
					).replace_extension("").string();

					auto [rootEnd, scriptEnd] = std::mismatch(lua_path.begin(), lua_path.end(), script_path.begin());

					auto clean_name = [](std::string_view s)
					{
						s.remove_prefix(std::min(s.find_first_not_of("\\"), s.size()));
						return std::string(s);
					};

					auto script_name = rootEnd != lua_path.end()
						? script_path
						: clean_name(std::string(scriptEnd, script_path.end()));

					std::string args;
					auto user_datas = static_cast<const char*>(IGFD_GetUserDatas(s_scriptLaunchDialog));
					if (user_datas != nullptr)
						args = std::string(user_datas);

					LuaRunCommand(script_name, allocate_args(args));
				}

				IGFD_Selection_DestroyContent(&selection);
			}

			IGFD_CloseDialog(s_scriptLaunchDialog);
		}

		ImGui::End();

		s_configNode[showMenu] = s_showMenu;
	}
}


/**
 * @fn OnWriteChatColor
 *
 * This is called each time WriteChatColor is called (whether by MQ2Main or by any
 * plugin).  This can be considered the "when outputting text from MQ" callback.
 *
 * This ignores filters on display, so if they are needed either implement them in
 * this section or see @ref OnIncomingChat where filters are already handled.
 *
 * If CEverQuest::dsp_chat is not called, and events are required, they'll need to
 * be implemented here as well.  Otherwise, see @ref OnIncomingChat where that is
 * already handled.
 *
 * For a list of Color values, see the constants for USERCOLOR_.  The default is
 * USERCOLOR_DEFAULT.
 *
 * @param Line const char* - The line that was passed to WriteChatColor
 * @param Color int - The type of chat text this is to be sent as
 * @param Filter int - (default 0)
 */
PLUGIN_API void OnWriteChatColor(const char* Line, int Color, int Filter)
{
	using namespace mq::lua;

	for (const auto& thread : s_running)
	{
		if (!thread->state->IsPaused())
			thread->eventProcessor->Process(Line);
	}
}

/**
 * @fn OnIncomingChat
 *
 * This is called each time a line of chat is shown.  It occurs after MQ filters
 * and chat events have been handled.  If you need to know when MQ2 has sent chat,
 * consider using @ref OnWriteChatColor instead.
 *
 * For a list of Color values, see the constants for USERCOLOR_. The default is
 * USERCOLOR_DEFAULT.
 *
 * @param Line const char* - The line of text that was shown
 * @param Color int - The type of chat text this was sent as
 *
 * @return bool - whether something was done based on the incoming chat
 */
PLUGIN_API bool OnIncomingChat(const char* Line, DWORD Color)
{
	using namespace mq::lua;

	for (const auto& thread : s_running)
	{
		if (!thread->state->IsPaused())
			thread->eventProcessor->Process(Line);
	}

	return false;
}