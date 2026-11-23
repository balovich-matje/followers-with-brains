#include "Tuning.h"

namespace Tuning
{
	namespace
	{
		Values g_values;

		struct Field
		{
			std::string_view name;
			float Values::*  ptr;
		};
		constexpr Field kFields[] = {
			{ "ChargeSpeed"sv, &Values::chargeSpeed },
			{ "Weave"sv, &Values::weave },
			{ "FlipMin"sv, &Values::flipMin },
			{ "FlipMax"sv, &Values::flipMax },
			{ "StepSpeed"sv, &Values::stepSpeed },
			{ "StepDiagonal"sv, &Values::stepDiagonal },
			{ "DodgeShare"sv, &Values::dodgeShare },
			{ "DodgeChance"sv, &Values::dodgeChance },
			{ "DodgeCooldown"sv, &Values::dodgeCooldown },
			{ "CloseSpeed"sv, &Values::closeSpeed },
		};

		std::string Trim(std::string_view a_text)
		{
			auto b = a_text.find_first_not_of(" \t\r\n");
			auto e = a_text.find_last_not_of(" \t\r\n");
			return b == std::string_view::npos ? std::string{} : std::string{ a_text.substr(b, e - b + 1) };
		}
	}

	namespace
	{
		std::filesystem::path g_extra;

		int Apply(const std::filesystem::path& a_path)
		{
			std::ifstream in{ a_path };
			if (!in) return 0;
			std::string line;
			int         applied = 0;
		while (std::getline(in, line)) {
			auto eq = line.find('=');
			if (eq == std::string::npos || line.empty() || line[0] == ';' || line[0] == '[') continue;
			auto key = Trim(std::string_view{ line }.substr(0, eq));
			auto value = Trim(std::string_view{ line }.substr(eq + 1));
			for (auto& f : kFields) {
				if (f.name == key) {
					g_values.*f.ptr = static_cast<float>(std::atof(value.c_str()));
					++applied;
				}
			}
		}
		return applied;
		}
	}

	const Values& Get() { return g_values; }

	void SetExtraFile(std::filesystem::path a_path) { g_extra = std::move(a_path); }

	void Load()
	{
		g_values = Values{};
		int applied = Apply("Data/SKSE/Plugins/FollowersWithBrains_Tuning.ini");
		if (!g_extra.empty()) applied += Apply(g_extra);
		logger::info("Tuning: {} value(s) applied", applied);
	}
}
