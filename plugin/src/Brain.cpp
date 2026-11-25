// The brain: everything the mod decides and does, in a DLL of its own that the loader
// (FollowersWithBrains.dll) loads, and can swap for a newer build while the game runs.
// These are the only entry points; the loader calls them on the main thread.
#include "Experience.h"
#include "Followers.h"
#include "Host.h"
#include "Settings.h"
#include "Test.h"

#define FWB_EXPORT extern "C" __declspec(dllexport)

namespace
{
	void SetupLog()
	{
		auto dir = logger::log_directory();
		if (!dir) return;
		// the loader created (and emptied) the log at launch: add to it
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "FollowersWithBrains.log").string(), false);
		auto log = std::make_shared<spdlog::logger>("brain", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}
}

FWB_EXPORT int FWB_ApiVersion() { return 2; }

FWB_EXPORT void FWB_Init(const SKSE::TaskInterface* a_tasks, RE::BSTEventSource<SKSE::ActionEvent>* a_actionEvents)
{
	Host::g_tasks = a_tasks;
	Host::g_actionEvents = a_actionEvents;
	SetupLog();
}

FWB_EXPORT void FWB_DataLoaded()
{
	Settings::Load();
	Followers::Install();
	Test::Install();
}

FWB_EXPORT void FWB_PostLoadGame()
{
	Followers::Reset();
	Test::OnGameLoaded();
}

FWB_EXPORT void FWB_NewGame() { Followers::Reset(); }

// Hot reload: a new brain picks up a game in progress (instead of FWB_DataLoaded; no load event
// comes). Automated test runs are not resumed.
FWB_EXPORT void FWB_Resume()
{
	Settings::Load();
	Followers::Install();
	Followers::Reset();
	Experience::Unstash();
	logger::info("Brain reloaded into the running game");
}

// Before the loader unloads this DLL: put back every game function and event sink we took over,
// and undo what we changed on actors.
FWB_EXPORT void FWB_Shutdown()
{
	Followers::Shutdown();
	logger::info("Brain shut down");
	spdlog::default_logger()->flush();
}

FWB_EXPORT void FWB_OnSave(SKSE::SerializationInterface* a_intfc) { Followers::OnSave(a_intfc); }
FWB_EXPORT void FWB_OnLoad(SKSE::SerializationInterface* a_intfc) { Followers::OnLoad(a_intfc); }
FWB_EXPORT void FWB_OnRevert(SKSE::SerializationInterface* a_intfc) { Followers::OnRevert(a_intfc); }

// settings, for the MCM's native functions (registered by the loader)
FWB_EXPORT int   FWB_GetInt(const char* a_key) { return Settings::GetInt(a_key); }
FWB_EXPORT void  FWB_SetInt(const char* a_key, int a_value) { Settings::SetInt(a_key, a_value); }
FWB_EXPORT float FWB_GetFloat(const char* a_key) { return Settings::GetFloat(a_key); }
FWB_EXPORT void  FWB_SetFloat(const char* a_key, float a_value) { Settings::SetFloat(a_key, a_value); }
FWB_EXPORT void  FWB_SaveSettings() { Settings::Save(); }
