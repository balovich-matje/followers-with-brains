// Followers With Brains loader: the SKSE plugin itself. It stays small and stable; everything the
// mod does lives in the brain (SKSE/Plugins/FollowersWithBrains/FWBBrain.dll), which the loader
// loads, forwards the game's messages, saves and MCM calls to, and can swap for a new build while
// the game runs (lab mode: when the brain file changes, or with the console command "fwb reload").

extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableW(const wchar_t* a_name, const wchar_t* a_value);

namespace
{
	const std::filesystem::path kBrain{ "Data/SKSE/Plugins/FollowersWithBrains/FWBBrain.dll" };
	const std::filesystem::path kLabConfig{ "Data/SKSE/Plugins/FollowersWithBrains_Lab.ini" };

	bool                       g_dataLoaded = false;
	std::filesystem::path      g_labDir;  // lab mode when set
	int                        g_copies = 0;

	// ------------------------------------------------------------------ the brain
	struct Brain
	{
		SKSE::WinAPI::HMODULE module = nullptr;
		void (*init)(const SKSE::TaskInterface*, RE::BSTEventSource<SKSE::ActionEvent>*) = nullptr;
		void (*dataLoaded)() = nullptr;
		void (*postLoadGame)() = nullptr;
		void (*newGame)() = nullptr;
		void (*resume)() = nullptr;
		void (*shutdown)() = nullptr;
		void (*onSave)(SKSE::SerializationInterface*) = nullptr;
		void (*onLoad)(SKSE::SerializationInterface*) = nullptr;
		void (*onRevert)(SKSE::SerializationInterface*) = nullptr;
		int (*getInt)(const char*) = nullptr;
		void (*setInt)(const char*, int) = nullptr;
		float (*getFloat)(const char*) = nullptr;
		void (*setFloat)(const char*, float) = nullptr;
		void (*saveSettings)() = nullptr;
	};

	Brain g_brain;

	template <class T>
	bool Find(Brain& a_brain, T& a_fn, const char* a_name)
	{
		a_fn = reinterpret_cast<T>(SKSE::WinAPI::GetProcAddress(a_brain.module, a_name));
		if (!a_fn) logger::error("brain: missing {}", a_name);
		return a_fn != nullptr;
	}

	// Loads a brain DLL. In lab mode a copy is loaded, so the file itself can be rebuilt any time.
	bool Open(Brain& a_brain, std::string& a_error)
	{
		std::filesystem::path path = kBrain;
		std::error_code       ec;
		if (!std::filesystem::exists(kBrain, ec)) {
			a_error = "brain not found: " + kBrain.string();
			return false;
		}
		if (!g_labDir.empty()) {
			auto dir = g_labDir / "brain";
			std::filesystem::create_directories(dir, ec);
			path = dir / std::format("FWBBrain-{}.dll", ++g_copies);
			std::filesystem::copy_file(kBrain, path, std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				a_error = "can't copy the brain: " + ec.message();
				return false;
			}
		}
		a_brain.module = LoadLibrary(path.wstring().c_str());
		if (!a_brain.module) {
			a_error = "can't load " + path.string();
			return false;
		}
		int (*api)() = nullptr;
		bool ok = Find(a_brain, api, "FWB_ApiVersion") && api() == 2 && Find(a_brain, a_brain.init, "FWB_Init") &&
		          Find(a_brain, a_brain.dataLoaded, "FWB_DataLoaded") && Find(a_brain, a_brain.postLoadGame, "FWB_PostLoadGame") &&
		          Find(a_brain, a_brain.newGame, "FWB_NewGame") && Find(a_brain, a_brain.resume, "FWB_Resume") &&
		          Find(a_brain, a_brain.shutdown, "FWB_Shutdown") && Find(a_brain, a_brain.onSave, "FWB_OnSave") &&
		          Find(a_brain, a_brain.onLoad, "FWB_OnLoad") && Find(a_brain, a_brain.onRevert, "FWB_OnRevert") &&
		          Find(a_brain, a_brain.getInt, "FWB_GetInt") && Find(a_brain, a_brain.setInt, "FWB_SetInt") &&
		          Find(a_brain, a_brain.getFloat, "FWB_GetFloat") && Find(a_brain, a_brain.setFloat, "FWB_SetFloat") &&
		          Find(a_brain, a_brain.saveSettings, "FWB_SaveSettings");
		if (!ok) {
			SKSE::WinAPI::FreeLibrary(a_brain.module);
			a_brain = {};
			a_error = "the brain doesn't match this loader (restart the game)";
			return false;
		}
		a_brain.init(SKSE::GetTaskInterface(), SKSE::GetActionEventSource());
		return true;
	}

	void Print(std::string_view a_text)
	{
		if (auto console = RE::ConsoleLog::GetSingleton()) console->Print("[FWB] %s", std::string{ a_text }.c_str());
	}

	std::filesystem::file_time_type g_brainTime;

	// Swaps the running brain for the current build. Main thread.
	void Reload()
	{
		if (!g_dataLoaded) return;
		Brain       fresh;
		std::string error;
		if (!Open(fresh, error)) {
			logger::error("reload failed: {}", error);
			Print("reload failed: " + error);
			RE::DebugNotification(("FWB reload failed: " + error).c_str());
			return;
		}
		std::error_code ec;
		g_brainTime = std::filesystem::last_write_time(kBrain, ec);
		Brain old = g_brain;
		if (old.shutdown) old.shutdown();
		g_brain = fresh;
		g_brain.resume();
		logger::info("brain reloaded (copy {})", g_copies);
		Print(std::format("brain reloaded (build {})", g_copies));
		RE::DebugNotification("Followers With Brains: reloaded");
		// unload the old brain a little later, once nothing can still be running inside it
		if (old.module) {
			auto module = old.module;
			std::thread([module] {
				std::this_thread::sleep_for(std::chrono::seconds(5));
				SKSE::GetTaskInterface()->AddTask([module] {
					SKSE::WinAPI::FreeLibrary(module);
					logger::info("old brain unloaded");
				});
			}).detach();
		}
	}

	// Lab mode: reload when a new build of the brain lands (checked twice a second).
	void WatchBrain()
	{
		std::thread([] {
			std::filesystem::file_time_type seen;
			int                             stable = 0;
			static std::atomic<bool>        queued{ false };
			for (;;) {
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				std::error_code ec;
				auto            t = std::filesystem::last_write_time(kBrain, ec);
				if (ec || t == g_brainTime) {
					stable = 0;
					continue;
				}
				stable = t == seen ? stable + 1 : 0;  // the file stopped changing (copy finished)
				seen = t;
				if (stable == 2 && !queued) {
					queued = true;
					SKSE::GetTaskInterface()->AddTask([] {
						Reload();
						queued = false;
					});
				}
			}
		}).detach();
	}

	// ------------------------------------------------------------------ forwarding
	void RegisterCommand();

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			g_dataLoaded = true;
			if (g_brain.dataLoaded) g_brain.dataLoaded();
			if (!g_labDir.empty()) {
				// taken over only now: done at plugin load (before the game had set up its own console
				// commands), it hung the game's startup more often than not
				RegisterCommand();
				WatchBrain();
			}
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			if (g_brain.postLoadGame) g_brain.postLoadGame();
			break;
		case SKSE::MessagingInterface::kNewGame:
			if (g_brain.newGame) g_brain.newGame();
			break;
		default:
			break;
		}
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		if (g_brain.onSave) g_brain.onSave(a_intfc);
	}
	void OnLoad(SKSE::SerializationInterface* a_intfc)
	{
		if (g_brain.onLoad) g_brain.onLoad(a_intfc);
	}
	void OnRevert(SKSE::SerializationInterface* a_intfc)
	{
		if (g_brain.onRevert) g_brain.onRevert(a_intfc);
	}

	// the MCM's native functions (FWB_Native.psc)
	std::int32_t GetInt(RE::StaticFunctionTag*, RE::BSFixedString a_key) { return g_brain.getInt ? g_brain.getInt(a_key.c_str()) : 0; }
	void SetInt(RE::StaticFunctionTag*, RE::BSFixedString a_key, std::int32_t a_value)
	{
		if (g_brain.setInt) g_brain.setInt(a_key.c_str(), a_value);
	}
	float GetFloat(RE::StaticFunctionTag*, RE::BSFixedString a_key) { return g_brain.getFloat ? g_brain.getFloat(a_key.c_str()) : 0.0f; }
	void SetFloat(RE::StaticFunctionTag*, RE::BSFixedString a_key, float a_value)
	{
		if (g_brain.setFloat) g_brain.setFloat(a_key.c_str(), a_value);
	}
	void SaveSettings(RE::StaticFunctionTag*)
	{
		if (g_brain.saveSettings) g_brain.saveSettings();
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		constexpr auto kClass = "FWB_Native"sv;
		a_vm->RegisterFunction("GetInt"sv, kClass, GetInt);
		a_vm->RegisterFunction("SetInt"sv, kClass, SetInt);
		a_vm->RegisterFunction("GetFloat"sv, kClass, GetFloat);
		a_vm->RegisterFunction("SetFloat"sv, kClass, SetFloat);
		a_vm->RegisterFunction("Save"sv, kClass, SaveSettings);
		return true;
	}

	// ------------------------------------------------------------------ lab: "fwb" console command
	std::string Stamp()
	{
		auto now = std::chrono::system_clock::now();
		auto t = std::chrono::system_clock::to_time_t(now);
		std::tm tm{};
		localtime_s(&tm, &t);
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
		return std::format("{:02}:{:02}:{:02}.{:03}", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
	}

	RE::SCRIPT_PARAMETER g_params[80];  // one per word of a message

	bool Exec(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*, RE::TESObjectREFR*, RE::TESObjectREFR*,
		RE::Script* a_script, RE::ScriptLocals*, double&, std::uint32_t&)
	{
		std::string text = a_script ? a_script->GetCommand() : "";
		auto        space = text.find_first_of(" \t");
		text = space == std::string::npos ? "" : text.substr(space + 1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) text.pop_back();
		if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
		if (text.empty()) {
			Print("fwb <message>: sends the message to Claude (and marks this moment in the recording); fwb reload: load the latest build");
			return true;
		}
		if (text == "reload") {
			Reload();
			return true;
		}
		std::ofstream out{ g_labDir / "in.txt", std::ios::app };
		out << Stamp() << '\t' << text << '\n';
		logger::info("USER: {}", text);
		Print(std::format("sent: {}", text));
		return true;
	}

	void RegisterCommand()
	{
		static constexpr std::string_view kCandidates[] = { "ToggleHeapTracking"sv, "DumpNiUpdates"sv, "TestSeenData"sv,
			"ShowRenderPasses"sv };
		for (auto name : kCandidates) {
			auto fn = RE::SCRIPT_FUNCTION::LocateConsoleCommand(name);
			if (!fn) continue;
			for (auto& p : g_params) {
				p.paramName = "word";
				p.paramType = RE::SCRIPT_PARAM_TYPE::kChar;
				p.optional = true;
			}
			fn->functionName = "fwb";
			fn->shortName = "";
			fn->helpString = "Followers With Brains lab: fwb <message> | fwb reload";
			fn->referenceFunction = false;
			fn->SetParameters(g_params);
			fn->executeFunction = &Exec;
			fn->editorFilter = false;
			logger::info("console command fwb (in place of {})", name);
			return;
		}
		logger::warn("no console command slot for fwb");
	}

	void ReadLabConfig()
	{
		std::ifstream in{ kLabConfig };
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.starts_with("Dir=")) g_labDir = line.substr(4);
		}
		if (g_labDir.empty()) return;
		// one recording session per launch, kept across hot reloads
		auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm tm{};
		localtime_s(&tm, &now);
		auto stamp = std::format(L"{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		SetEnvironmentVariableW(L"FWB_SESSION", stamp.c_str());
		// old brain copies from earlier launches
		std::error_code ec;
		std::filesystem::remove_all(g_labDir / "brain", ec);
	}

	void SetupLog()
	{
		auto dir = logger::log_directory();
		if (!dir) return;
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "FollowersWithBrains.log").string(), true);
		auto log = std::make_shared<spdlog::logger>("loader", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}
}

SKSEPluginInfo(
	.Version = { PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_PATCH, 0 },
	.Name = "FollowersWithBrains"sv,
	.Author = "balovich-matje"sv,
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	logger::info("FollowersWithBrains {} loaded (game {})", SKSEPlugin_Version.GetVersion().string(), a_skse->RuntimeVersion().string());

	if (std::filesystem::exists(kLabConfig)) {
		ReadLabConfig();
	}
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	SKSE::GetPapyrusInterface()->Register(RegisterPapyrus);
	auto serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID('FWBR');
	serialization->SetSaveCallback(OnSave);
	serialization->SetLoadCallback(OnLoad);
	serialization->SetRevertCallback(OnRevert);

	std::string error;
	if (!Open(g_brain, error)) {
		// stay loaded: a fixed brain can still be reloaded (lab)
		logger::critical("{}", error);
		return true;
	}
	std::error_code ec;
	g_brainTime = std::filesystem::last_write_time(kBrain, ec);
	return true;
}
