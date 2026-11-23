#include "Settings.h"

namespace Settings
{
	namespace
	{
		Values g_values;

		const std::filesystem::path kPath{ "Data/SKSE/Plugins/FollowersWithBrains.ini" };

		struct IntField
		{
			std::string_view name;
			int Values::*    ptr;
		};
		struct BoolField
		{
			std::string_view name;
			bool Values::*   ptr;
		};
		struct FloatField
		{
			std::string_view name;
			float Values::*  ptr;
		};

		constexpr IntField kInts[] = {
			{ "FollowDistance"sv, &Values::followDistance },
			{ "CombatDistance"sv, &Values::combatDistance },
		};
		constexpr BoolField kBools[] = {
			{ "CatchUp"sv, &Values::catchUp },
			{ "Teleport"sv, &Values::teleport },
			{ "CombatLeash"sv, &Values::combatLeash },
			{ "Awareness"sv, &Values::awareness },
			{ "Retarget"sv, &Values::retarget },
			{ "FocusSummoners"sv, &Values::focusSummoners },
			{ "MeleeSwitch"sv, &Values::meleeSwitch },
			{ "Retreat"sv, &Values::retreat },
			{ "SmartWeapons"sv, &Values::smartWeapons },
			{ "MatchWeapons"sv, &Values::matchWeapons },
			{ "Reflexes"sv, &Values::reflexes },
			{ "Zigzag"sv, &Values::zigzag },
			{ "Debug"sv, &Values::debug },
		};
		constexpr FloatField kFloats[] = {
			{ "RangedRetreatDistance"sv, &Values::rangedRetreatDistance },
		};

		std::string Lower(std::string_view a_text)
		{
			std::string out{ a_text };
			std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		std::string Trim(std::string_view a_text)
		{
			auto b = a_text.find_first_not_of(" \t\r\n");
			auto e = a_text.find_last_not_of(" \t\r\n");
			return b == std::string_view::npos ? std::string{} : std::string{ a_text.substr(b, e - b + 1) };
		}
	}

	Values& Get() { return g_values; }

	int GetInt(std::string_view a_key)
	{
		auto key = Lower(a_key);
		for (auto& f : kInts) {
			if (Lower(f.name) == key) return g_values.*f.ptr;
		}
		for (auto& f : kBools) {
			if (Lower(f.name) == key) return g_values.*f.ptr ? 1 : 0;
		}
		for (auto& f : kFloats) {
			if (Lower(f.name) == key) return static_cast<int>(g_values.*f.ptr);
		}
		logger::warn("Unknown setting {}", a_key);
		return 0;
	}

	void SetInt(std::string_view a_key, int a_value)
	{
		auto key = Lower(a_key);
		for (auto& f : kInts) {
			if (Lower(f.name) == key) {
				g_values.*f.ptr = a_value;
				return;
			}
		}
		for (auto& f : kBools) {
			if (Lower(f.name) == key) {
				g_values.*f.ptr = a_value != 0;
				return;
			}
		}
		for (auto& f : kFloats) {
			if (Lower(f.name) == key) {
				g_values.*f.ptr = static_cast<float>(a_value);
				return;
			}
		}
		logger::warn("Unknown setting {}", a_key);
	}

	float GetFloat(std::string_view a_key)
	{
		auto key = Lower(a_key);
		for (auto& f : kFloats) {
			if (Lower(f.name) == key) return g_values.*f.ptr;
		}
		return static_cast<float>(GetInt(a_key));
	}

	void SetFloat(std::string_view a_key, float a_value)
	{
		auto key = Lower(a_key);
		for (auto& f : kFloats) {
			if (Lower(f.name) == key) {
				g_values.*f.ptr = a_value;
				return;
			}
		}
		SetInt(a_key, static_cast<int>(a_value));
	}

	void Load()
	{
		std::ifstream in{ kPath };
		if (!in) {
			logger::info("No settings file, using defaults");
			return;
		}
		std::string line;
		while (std::getline(in, line)) {
			auto text = Trim(line);
			if (text.empty() || text[0] == ';' || text[0] == '#' || text[0] == '[') continue;
			auto eq = text.find('=');
			if (eq == std::string::npos) continue;
			auto key = Trim(std::string_view{ text }.substr(0, eq));
			auto value = Trim(std::string_view{ text }.substr(eq + 1));
			try {
				SetFloat(key, std::stof(value));
			} catch (...) {
				logger::warn("Bad value for {}: {}", key, value);
			}
		}
		g_values.followDistance = std::clamp(g_values.followDistance, 0, 2);
		g_values.combatDistance = std::clamp(g_values.combatDistance, 0, 2);
		g_values.rangedRetreatDistance = std::clamp(g_values.rangedRetreatDistance, 300.0f, 4000.0f);
		logger::info("Settings loaded");
	}

	void Save()
	{
		std::error_code ec;
		std::filesystem::create_directories(kPath.parent_path(), ec);
		std::ofstream out{ kPath, std::ios::trunc };
		if (!out) {
			logger::error("Could not write {}", kPath.string());
			return;
		}
		out << "; Followers With Brains settings. Change them in the MCM.\n[Settings]\n";
		for (auto& f : kInts) out << f.name << " = " << g_values.*f.ptr << '\n';
		for (auto& f : kBools) out << f.name << " = " << (g_values.*f.ptr ? 1 : 0) << '\n';
		for (auto& f : kFloats) out << f.name << " = " << g_values.*f.ptr << '\n';
	}
}
