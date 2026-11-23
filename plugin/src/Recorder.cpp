#include "Recorder.h"

namespace Recorder
{
	namespace
	{
		bool          g_on = false;
		std::ofstream g_out;
		float         g_time = 0.0f;
		RE::NiPoint3  g_last;
		bool          g_hasLast = false;
	}

	void Install()
	{
		g_on = std::filesystem::exists("Data/SKSE/Plugins/FollowersWithBrains_Record.ini");
		if (!g_on) return;
		std::error_code ec;
		std::filesystem::create_directories("Data/SKSE/Plugins/FollowersWithBrains_Record", ec);
		g_out.open("Data/SKSE/Plugins/FollowersWithBrains_Record/record.csv", std::ios::trunc);
		g_out << "time,x,y,z,heading,speed,graphSpeed,graphDirection,sprinting,running,inCombat,attack\n";
		logger::info("Recorder on");
	}

	void Frame(float a_delta)
	{
		if (!g_on || a_delta <= 0.0f) return;
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) return;
		g_time += a_delta;
		auto  pos = player->GetPosition();
		float speed = 0.0f;
		if (g_hasLast) {
			RE::NiPoint3 d = pos - g_last;
			d.z = 0.0f;
			speed = d.Length() / a_delta;
		}
		g_last = pos;
		g_hasLast = true;
		float graphSpeed = -1.0f, graphDirection = -1.0f;
		player->GetGraphVariableFloat("Speed", graphSpeed);
		player->GetGraphVariableFloat("Direction", graphDirection);
		auto state = player->AsActorState();
		g_out << std::format("{:.3f},{:.1f},{:.1f},{:.1f},{:.4f},{:.1f},{:.1f},{:.3f},{},{},{},{}\n", g_time, pos.x, pos.y, pos.z,
			player->GetAngleZ(), speed, graphSpeed, graphDirection, state->IsSprinting() ? 1 : 0,
			state->actorState1.running ? 1 : 0, player->IsInCombat() ? 1 : 0, static_cast<int>(state->GetAttackState()));
		static int flush = 0;
		if (++flush % 60 == 0) g_out.flush();
	}
}
