#include "Tactics.h"
#include "Settings.h"

namespace Tactics
{
	namespace
	{
		struct Entry
		{
			RE::TESCombatStyle* original = nullptr;
			RE::TESCombatStyle* tuned = nullptr;
			RE::TESCombatStyle* ranged = nullptr;  // the archer/caster version: shoot, keep distance
		};
		std::unordered_map<RE::FormID, Entry> g_entries;  // by base NPC, for the whole session

		RE::TESCombatStyle* DefaultStyle()
		{
			// csHumanMeleeLvl1, else DefaultCombatstyle: used when an NPC has no style of its own
			auto style = RE::TESForm::LookupByID<RE::TESCombatStyle>(0x0003BE1B);
			return style ? style : RE::TESForm::LookupByID<RE::TESCombatStyle>(0x0000003D);
		}

		void Tune(RE::TESCombatStyle* s)
		{
			auto& cfg = Settings::Get();
			auto& g = s->generalData;
			auto& m = s->meleeData;
			auto& c = s->closeRangeData;
			auto& l = s->longRangeData;
			if (cfg.reflexes) {
				// avoidThreatChance is left alone: the game's own threat avoidance turns and runs, which
				// looks wrong in a duel; the plugin's reflexes do the dodging instead.
				m.attackIncapacitatedMult = std::max(m.attackIncapacitatedMult, 2.0f);  // hit staggered enemies
				m.powerAttackIncapacitatedMult = std::max(m.powerAttackIncapacitatedMult, 2.0f);
				m.bashPowerAttackMult = std::max(m.bashPowerAttackMult, 2.0f);  // bash power attacks (needs a block)
				m.bashMult = std::max(m.bashMult, 1.0f);
				m.bashRecoilMult = std::max(m.bashRecoilMult, 1.0f);
				c.circleMult = std::max(c.circleMult, 0.6f);                // circle instead of standing still (the
				                                                            // game's own footwork, no sliding)
			}
			if (cfg.zigzag && cfg.combatMovement) {
				l.strafeMult = std::max(l.strafeMult, 1.0f);                // strafe at range: no straight runs at archers
				s->flags.set(RE::TESCombatStyle::FLAG::kFlankingStyle);
				c.flankDistanceMult = std::max(c.flankDistanceMult, 1.0f);
			}
		}
	}

	// Holding a bow, crossbow or staff, a player keeps their distance and shoots: the style says so
	// (shooting over closing in, backing off from melee at close range, no circling or flanking).
	void TuneRanged(RE::TESCombatStyle* s)
	{
		auto& g = s->generalData;
		auto& c = s->closeRangeData;
		g.rangedScoreMult = std::max({ g.rangedScoreMult, g.meleeScoreMult, 1.0f }) * 1.5f;
		g.staffScoreMult = std::max({ g.staffScoreMult, g.meleeScoreMult, 1.0f }) * 1.5f;
		g.meleeScoreMult = std::min(g.meleeScoreMult, 0.2f);
		c.fallbackMult = std::max(c.fallbackMult, 1.0f);  // step back from someone in her face
		c.circleMult = std::min(c.circleMult, 0.2f);
		c.flankDistanceMult = 0.0f;
		s->flags.reset(RE::TESCombatStyle::FLAG::kFlankingStyle);
	}

	void Apply(RE::Actor* a_follower, bool a_ranged)
	{
		auto npc = a_follower ? a_follower->GetActorBase() : nullptr;
		if (!npc) return;
		auto& cfg = Settings::Get();
		// reflexes tune melee (circling, punishing, bashing); charging shooters needs the movement switch
		if (!cfg.reflexes && !(cfg.combatMovement && cfg.zigzag)) {
			Restore(a_follower);
			return;
		}
		auto& e = g_entries[npc->GetFormID()];
		if (!e.tuned) {
			auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESCombatStyle>();
			e.tuned = factory ? factory->Create() : nullptr;
			if (!e.tuned) return;
			e.original = npc->combatStyle;
			auto source = e.original ? e.original : DefaultStyle();
			if (source) {
				e.tuned->generalData = source->generalData;
				e.tuned->meleeData = source->meleeData;
				e.tuned->closeRangeData = source->closeRangeData;
				e.tuned->longRangeData = source->longRangeData;
				e.tuned->flightData = source->flightData;
				e.tuned->flags = source->flags;
			}
			Tune(e.tuned);
			e.ranged = factory->Create();
			if (e.ranged) {
				e.ranged->generalData = e.tuned->generalData;
				e.ranged->meleeData = e.tuned->meleeData;
				e.ranged->closeRangeData = e.tuned->closeRangeData;
				e.ranged->longRangeData = e.tuned->longRangeData;
				e.ranged->flightData = e.tuned->flightData;
				e.ranged->flags = e.tuned->flags;
				TuneRanged(e.ranged);
			}
			logger::info("{}: tuned combat style (avoid threats {:.2f}, strafe {:.2f}, fallback {:.2f}, circle {:.2f})",
				a_follower->GetDisplayFullName(), e.tuned->generalData.avoidThreatChance, e.tuned->longRangeData.strafeMult,
				e.tuned->closeRangeData.fallbackMult, e.tuned->closeRangeData.circleMult);
		}
		auto want = a_ranged && e.ranged ? e.ranged : e.tuned;
		if (npc->combatStyle != want) {
			npc->combatStyle = want;
			logger::info("{}: {} combat style", a_follower->GetDisplayFullName(), want == e.ranged ? "ranged" : "melee");
		}
		// a fight already running keeps its own copy of the style pointer
		if (auto controller = a_follower->GetActorRuntimeData().combatController; controller && controller->combatStyle != want) {
			controller->combatStyle = want;
		}
	}

	void Restore(RE::Actor* a_follower)
	{
		auto npc = a_follower ? a_follower->GetActorBase() : nullptr;
		if (!npc) return;
		auto it = g_entries.find(npc->GetFormID());
		if (it == g_entries.end()) return;
		auto ours = [&](RE::TESCombatStyle* a_style) { return a_style && (a_style == it->second.tuned || a_style == it->second.ranged); };
		if (ours(npc->combatStyle)) npc->combatStyle = it->second.original;
		if (auto controller = a_follower->GetActorRuntimeData().combatController; controller && ours(controller->combatStyle)) {
			controller->combatStyle = it->second.original;
		}
		g_entries.erase(it);  // the copy is left to the game (a few bytes)
	}

	// Base forms stay in memory across loads, so the tuned copies (and the originals we remember)
	// stay valid for the whole session: nothing to forget.
	void Forget() {}
}
