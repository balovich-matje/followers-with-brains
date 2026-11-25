#include "Potions.h"

namespace Potions
{
	namespace
	{
		enum class Kind
		{
			kNone,
			kHeal,  // restores health, at once or over time
			kBuff   // lasting good effects (fortify, resist...)
		};

		// The game's own tag for healing potions (its AI picks potions by it). Looked up once by its editor
		// ID; checking an effect's keywords against it reads no names (reading the names of arbitrary
		// keywords crashed the game with RFAB's potions).
		RE::BGSKeyword* RestoreTag()
		{
			static RE::BGSKeyword* tag = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("MagicAlchRestoreHealth");
			return tag;
		}

		// Restores health: tagged so, or (untagged) a short restore of health. Overhauls build restore and
		// fortify potions alike (RFAB: both raise health for a while); the tag tells them apart.
		bool HealsHealth(const RE::Effect* a_effect)
		{
			auto base = a_effect ? a_effect->baseEffect : nullptr;
			if (!base || base->IsDetrimental()) return false;
			if (auto tag = RestoreTag(); tag && base->HasKeyword(tag)) return true;
			// untagged: raises health for a short while (restores are short, fortifies last minutes)
			using A = RE::EffectSetting::Archetype;
			auto type = base->GetArchetype();
			return base->data.primaryAV == RE::ActorValue::kHealth && (type == A::kValueModifier || type == A::kPeakValueModifier) &&
			       a_effect->effectItem.duration <= 60;
		}

		Kind KindOf(RE::AlchemyItem* a_potion)
		{
			if (!a_potion || a_potion->IsPoison() || a_potion->IsFood()) return Kind::kNone;
			bool heal = false;
			bool lasting = false;
			for (auto effect : a_potion->effects) {
				if (!effect || !effect->baseEffect) continue;
				if (effect->baseEffect->IsDetrimental()) return Kind::kNone;  // drawbacks: leave it to the player
				if (HealsHealth(effect)) heal = true;
				if (effect->effectItem.duration >= 30) lasting = true;
			}
			if (heal) return Kind::kHeal;
			return lasting ? Kind::kBuff : Kind::kNone;
		}

		// health it gives back in all
		float HealAmount(RE::AlchemyItem* a_potion)
		{
			float total = 0.0f;
			for (auto effect : a_potion->effects) {
				if (HealsHealth(effect)) total += effect->effectItem.magnitude * std::max<std::uint32_t>(1, effect->effectItem.duration);
			}
			return total;
		}

		// a potion's effect still running on her: from that potion, or (healing) any healing potion
		bool Running(RE::Actor* a_actor, RE::AlchemyItem* a_potion, Kind a_kind)
		{
			auto target = a_actor->AsMagicTarget();
			auto list = target ? target->GetActiveEffectList() : nullptr;
			if (!list) return false;
			for (auto ae : *list) {
				if (!ae || !ae->spell || ae->flags.any(RE::ActiveEffect::Flag::kDispelled, RE::ActiveEffect::Flag::kInactive)) continue;
				auto potion = ae->spell->As<RE::AlchemyItem>();
				if (!potion) continue;
				if (a_kind == Kind::kHeal) {
					if (HealsHealth(ae->effect) && ae->elapsedSeconds < ae->duration) return true;
				} else if (potion == a_potion || (ae->effect && a_potion->effects.size() && a_potion->effects[0] &&
				                                    ae->effect->baseEffect == a_potion->effects[0]->baseEffect)) {
					return true;
				}
			}
			return false;
		}

		struct State
		{
			float nextCheck = 0.0f;
			float lastDrink = -100.0f;
			bool  wasInCombat = false;
			float buffFrom = -1.0f;  // fight started: buffs from then on
			int   buffsLeft = 0;
			float lastCombat = -100.0f;
		};
		std::unordered_map<RE::FormID, State> g_state;

		void Drink(RE::Actor* a_actor, RE::AlchemyItem* a_potion, State& a_state, float a_now, std::string_view a_why)
		{
			RE::ActorEquipManager::GetSingleton()->EquipObject(a_actor, a_potion);
			a_state.lastDrink = a_now;
			auto name = a_potion->GetName();
			logger::info("{} drinks {} ({})", a_actor->GetDisplayFullName(), name && *name ? name : "a potion", a_why);
		}
	}

	void Update(RE::Actor* a_follower, bool a_inCombat, bool a_fodder, float a_now)
	{
		auto& st = g_state[a_follower->GetFormID()];
		// a new fight: not the game flicking her in and out of combat as one starts (that refilled the
		// allowance: four buffs at once)
		if (a_inCombat && !st.wasInCombat && a_now - st.lastCombat > 20.0f) {
			st.buffFrom = a_now + 1.0f;  // a moment to see what the fight is
			st.buffsLeft = 2;
		}
		if (a_inCombat) st.lastCombat = a_now;
		st.wasInCombat = a_inCombat;
		if (!a_inCombat || a_now < st.nextCheck || a_now - st.lastDrink < 1.5f) return;
		st.nextCheck = a_now + 0.5f;
		auto state = a_follower->AsActorState();
		if (state->IsBleedingOut()) return;

		std::vector<std::pair<RE::AlchemyItem*, Kind>> potions;
		for (auto& [obj, data] : a_follower->GetInventory([](RE::TESBoundObject& o) { return o.Is(RE::FormType::AlchemyItem); })) {
			if (data.first <= 0) continue;
			auto potion = obj->As<RE::AlchemyItem>();
			if (auto kind = KindOf(potion); kind != Kind::kNone) potions.emplace_back(potion, kind);
		}
		if (potions.empty()) return;

		// hurt: one healing potion, unless one is still working
		auto  avo = a_follower->AsActorValueOwner();
		float maxHp = std::max(1.0f, avo->GetPermanentActorValue(RE::ActorValue::kHealth));
		float hp = avo->GetActorValue(RE::ActorValue::kHealth);
		if (hp / maxHp < 0.4f) {
			RE::AlchemyItem* best = nullptr;
			float            bestScore = std::numeric_limits<float>::max();
			float            missing = maxHp - hp;
			for (auto& [potion, kind] : potions) {
				if (kind != Kind::kHeal) continue;
				if (Running(a_follower, potion, kind)) return;  // already healing: wait it out
				float amount = HealAmount(potion);
				// the smallest that covers what she lost, else the biggest
				float score = amount >= missing ? amount : 1e6f - amount;
				if (score < bestScore) {
					bestScore = score;
					best = potion;
				}
			}
			if (best) {
				Drink(a_follower, best, st, a_now, std::format("{:.0f}% health", hp / maxHp * 100.0f));
				return;
			}
		}

		// a fight worth it: long-lasting buffs, once, at the start
		// (the first 15 s of a fight: who is in it shows up over the first moments, a boss after its summons)
		if (st.buffsLeft > 0 && a_now >= st.buffFrom && st.buffFrom >= 0.0f) {
			if (a_now > st.buffFrom + 15.0f) {
				st.buffsLeft = 0;
				logger::info("{}: no buffs this fight ({})", a_follower->GetDisplayFullName(), a_fodder ? "only fodder" : "none to drink");
				return;
			}
			if (a_fodder) return;
			for (auto& [potion, kind] : potions) {
				if (kind != Kind::kBuff || Running(a_follower, potion, kind)) continue;
				Drink(a_follower, potion, st, a_now, "start of a fight");
				--st.buffsLeft;
				return;
			}
			st.buffsLeft = 0;
		}
	}

	void Forget() { g_state.clear(); }

	std::string Describe(RE::Actor* a_actor)
	{
		std::string out;
		for (auto& [obj, data] : a_actor->GetInventory([](RE::TESBoundObject& o) { return o.Is(RE::FormType::AlchemyItem); })) {
			auto potion = obj ? obj->As<RE::AlchemyItem>() : nullptr;
			if (!potion || data.first <= 0) continue;
			auto name = potion->GetName();
			auto kind = KindOf(potion);
			out += std::format("{}{} {:08X} x{} ({}{})", out.empty() ? "" : "; ", name && *name ? name : "?", potion->GetFormID(), data.first,
				kind == Kind::kHeal ? "heal" : kind == Kind::kBuff ? "buff" : "ignored",
				kind == Kind::kHeal ? std::format(", {:.0f} hp", HealAmount(potion)) : std::string{});
			{
				for (auto e : potion->effects) {
					if (e && e->baseEffect)
						out += std::format(" [av{} arch{} {}{} x{}s]", static_cast<int>(e->baseEffect->data.primaryAV),
							static_cast<int>(e->baseEffect->GetArchetype()), e->baseEffect->IsDetrimental() ? "bad " : "",
							e->effectItem.magnitude, e->effectItem.duration);
				}
			}
		}
		return std::format("{} (restore tag {})", out.empty() ? "no potions" : out, RestoreTag() ? "found" : "not found");
	}
}
