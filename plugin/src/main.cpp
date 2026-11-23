#include "Followers.h"
#include "Papyrus.h"
#include "Settings.h"
#include "Test.h"

namespace
{
	void SetupLog()
	{
		auto dir = logger::log_directory();
		if (!dir) {
			return;
		}
		auto path = *dir / "FollowersWithBrains.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Settings::Load();
			Followers::Install();
			Test::Install();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Followers::Reset();
			Test::OnGameLoaded();
			break;
		case SKSE::MessagingInterface::kNewGame:
			Followers::Reset();
			break;
		default:
			break;
		}
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
	logger::info("FollowersWithBrains {} loaded (game {})", SKSEPlugin_Version.GetVersion().string(),
		a_skse->RuntimeVersion().string());

	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	SKSE::GetPapyrusInterface()->Register(Papyrus::Register);

	auto serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID('FWBR');
	serialization->SetSaveCallback(Followers::OnSave);
	serialization->SetLoadCallback(Followers::OnLoad);
	serialization->SetRevertCallback(Followers::OnRevert);
	return true;
}
