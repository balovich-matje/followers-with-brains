#include "Bodies.h"

namespace Bodies
{
	namespace
	{
		std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_lastKick;

		void RunConsole(const std::string& a_command)
		{
			auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto script = factory ? factory->Create() : nullptr;
			if (!script) return;
			script->SetCommand(a_command);
			script->CompileAndRun(nullptr);
			delete script;
		}
	}

	RE::Actor* InTheWay(RE::Actor* a_actor, const RE::NiPoint3& a_dir, float a_range)
	{
		auto tes = RE::TES::GetSingleton();
		if (!a_actor || !tes) return nullptr;
		auto       pos = a_actor->GetPosition();
		RE::Actor* found = nullptr;
		float      best = a_range;
		tes->ForEachReferenceInRange(a_actor, a_range, [&](RE::TESObjectREFR& a_ref) {
			auto body = a_ref.As<RE::Actor>();
			if (!body || body == a_actor || !body->IsDead()) return RE::BSContainer::ForEachResult::kContinue;
			RE::NiPoint3 to = body->GetPosition() - pos;
			to.z = 0.0f;
			float d = to.Length();
			if (d < 1.0f || d >= best) return RE::BSContainer::ForEachResult::kContinue;
			if ((to.x * a_dir.x + to.y * a_dir.y) / d > 0.3f) {
				best = d;
				found = body;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return found;
	}

	void Kick(RE::Actor* a_by, RE::Actor* a_body)
	{
		if (!a_by || !a_body || !a_body->IsDead()) return;
		auto now = std::chrono::steady_clock::now();
		auto& last = g_lastKick[a_body->GetFormID()];
		if (now - last < std::chrono::milliseconds(500)) return;
		last = now;
		// the game's own shove (the force a shout or a big hit gives a ragdoll), small
		RunConsole(std::format("{:08X}.pushactoraway {:08X} 3", a_by->GetFormID(), a_body->GetFormID()));
		logger::info("{} shoves {}'s body aside", a_by->GetDisplayFullName(), a_body->GetDisplayFullName());
	}
}
