#include "Loadout.h"

namespace Loadout
{
	namespace
	{
		bool IsAttackSpell(RE::SpellItem* a_spell)
		{
			if (!a_spell || a_spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell) return false;
			if (a_spell->GetDelivery() != RE::MagicSystem::Delivery::kAimed) return false;
			for (auto effect : a_spell->effects) {
				if (effect && effect->baseEffect && effect->baseEffect->IsHostile()) return true;
			}
			return false;
		}

		float SpellPower(RE::SpellItem* a_spell)
		{
			float total = 0.0f;
			for (auto effect : a_spell->effects) {
				if (effect && effect->baseEffect && effect->baseEffect->IsHostile()) total += effect->effectItem.magnitude;
			}
			return total;
		}

		RE::ActorValue SpellSkill(RE::SpellItem* a_spell)
		{
			for (auto effect : a_spell->effects) {
				if (effect && effect->baseEffect) return effect->baseEffect->data.associatedSkill;
			}
			return RE::ActorValue::kDestruction;
		}

		void ConsiderSpell(RE::SpellItem* a_spell, RE::SpellItem*& a_best)
		{
			if (IsAttackSpell(a_spell) && (!a_best || SpellPower(a_spell) > SpellPower(a_best))) a_best = a_spell;
		}

		// a spell that conjures a melee weapon (bound sword, flame blades...): the weapon it gives
		RE::TESObjectWEAP* BoundWeapon(RE::SpellItem* a_spell)
		{
			if (!a_spell || a_spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell) return nullptr;
			for (auto effect : a_spell->effects) {
				auto base = effect ? effect->baseEffect : nullptr;
				if (!base || base->GetArchetype() != RE::EffectSetting::Archetype::kBoundWeapon) continue;
				auto weap = base->data.associatedForm ? base->data.associatedForm->As<RE::TESObjectWEAP>() : nullptr;
				if (weap && weap->IsMelee() && !weap->IsHandToHandMelee()) return weap;
			}
			return nullptr;
		}

		bool IsRangedWeapon(RE::TESObjectWEAP* a_weapon) { return a_weapon && (a_weapon->IsBow() || a_weapon->IsCrossbow()); }

		bool IsTwoHanded(RE::TESObjectWEAP* a_weapon)
		{
			return a_weapon->IsTwoHandedSword() || a_weapon->IsTwoHandedAxe();
		}

		float HitDamage(RE::Actor* a_actor, RE::InventoryEntryData* a_entry)
		{
			return Loadout::HitDamage(a_actor, RE::PlayerCharacter::GetSingleton(), a_entry);
		}

		float EnchantDamage(RE::InventoryEntryData* a_entry, RE::TESObjectWEAP* a_weapon)
		{
			RE::EnchantmentItem* enchant = a_entry ? a_entry->GetEnchantment() : nullptr;
			if (!enchant && a_weapon) enchant = a_weapon->formEnchanting;
			if (!enchant) return 0.0f;
			float total = 0.0f;
			for (auto effect : enchant->effects) {
				if (effect && effect->baseEffect && effect->baseEffect->IsHostile()) total += effect->effectItem.magnitude;
			}
			return total;
		}

		// Rough damage per second: one hit (plus enchantment) times attacks per second.
		float WeaponDps(RE::Actor* a_actor, RE::TESObjectWEAP* a_weapon, RE::InventoryEntryData* a_entry, float a_ammoDamage)
		{
			float hit = HitDamage(a_actor, a_entry);
			if (hit <= 0.0f) hit = a_weapon->GetAttackDamage() + a_ammoDamage;
			hit += EnchantDamage(a_entry, a_weapon);
			float speed = std::max(0.3f, a_weapon->weaponData.speed);
			float perSecond = (a_weapon->IsBow() || a_weapon->IsCrossbow()) ? speed / 2.0f : speed * 0.8f;
			return hit * perSecond;
		}
	}

	float HitDamage(RE::Actor* a_attacker, RE::Actor* a_target, RE::InventoryEntryData* a_entry)
	{
		if (!a_entry || !a_target || a_target == a_attacker) return 0.0f;
		alignas(16) std::array<std::byte, sizeof(RE::HitData)> buffer{};
		auto hit = reinterpret_cast<RE::HitData*>(buffer.data());
		hit->Populate(a_attacker, a_target, a_entry);
		return std::max(0.0f, hit->totalDamage);
	}

	Options Read(RE::Actor* a_actor)
	{
		Options out;
		RE::TESAmmo* arrows = nullptr;
		RE::TESAmmo* bolts = nullptr;
		RE::TESObjectWEAP* bow = nullptr;
		RE::TESObjectWEAP* crossbow = nullptr;

		auto inventory = a_actor->GetInventory([](RE::TESBoundObject& a_obj) {
			return a_obj.IsWeapon() || a_obj.IsAmmo();
		});
		for (auto& [obj, data] : inventory) {
			if (data.first <= 0) continue;
			if (auto ammo = obj->As<RE::TESAmmo>()) {
				auto& slot = ammo->IsBolt() ? bolts : arrows;
				if (!slot || ammo->data.damage > slot->data.damage) slot = ammo;
			}
		}
		if (auto current = a_actor->GetCurrentAmmo()) {
			auto& slot = current->IsBolt() ? bolts : arrows;
			if (!slot) slot = current;
		}
		float bowDps = 0.0f, crossbowDps = 0.0f, staffDps = 0.0f;
		RE::TESObjectWEAP* staff = nullptr;
		for (auto& [obj, data] : inventory) {
			auto weap = data.first > 0 ? obj->As<RE::TESObjectWEAP>() : nullptr;
			if (!weap || !weap->GetPlayable()) continue;
			auto entry = data.second.get();
			if (weap->IsStaff()) {
				// a staff shoots its enchantment: about one cast a second and a half
				float dps = EnchantDamage(entry, weap) / 1.5f;
				if (dps > staffDps) { staffDps = dps; staff = weap; }
				continue;
			}
			if (weap->IsBow()) {
				float dps = WeaponDps(a_actor, weap, entry, arrows ? arrows->data.damage : 0.0f);
				if (dps > bowDps) { bowDps = dps; bow = weap; }
			} else if (weap->IsCrossbow()) {
				float dps = WeaponDps(a_actor, weap, entry, bolts ? bolts->data.damage : 0.0f);
				if (dps > crossbowDps) { crossbowDps = dps; crossbow = weap; }
			} else if (weap->IsMelee() && !weap->IsHandToHandMelee()) {
				float dps = WeaponDps(a_actor, weap, entry, 0.0f);
				if (dps > out.meleeScore) { out.meleeScore = dps; out.melee = weap; }
			}
		}
		bool useCrossbow = crossbow && bolts && (!bow || !arrows || crossbowDps > bowDps);
		if (useCrossbow) {
			out.bow = crossbow;
			out.ammo = bolts;
			out.rangedScore = crossbowDps;
		} else if (bow && arrows) {
			out.bow = bow;
			out.ammo = arrows;
			out.rangedScore = bowDps;
		} else {
			out.unusableBow = bow ? bow : crossbow;
		}
		if (staff && staffDps > out.rangedScore) {
			out.bow = staff;
			out.ammo = nullptr;
			out.rangedScore = staffDps;
		}

		if (auto npc = a_actor->GetActorBase()) {
			if (auto list = npc->GetSpellList(); list && list->spells) {
				for (std::uint32_t i = 0; i < list->numSpells; ++i) ConsiderSpell(list->spells[i], out.spell);
			}
		}
		for (auto spell : a_actor->GetActorRuntimeData().addedSpells) ConsiderSpell(spell, out.spell);

		// a conjured weapon as her melee weapon, when it hits at least as hard as what she carries
		// (one-handed ones conjured in both hands)
		auto considerBound = [&](RE::SpellItem* a_spell) {
			auto weap = BoundWeapon(a_spell);
			if (!weap) return;
			RE::InventoryEntryData entry(weap, 1);
			float dps = WeaponDps(a_actor, weap, &entry, 0.0f) * (IsTwoHanded(weap) ? 1.0f : 1.6f);
			if (dps < 1.0f) dps = std::max(out.meleeScore, 1.0f);  // its damage comes from scripts/effects: can't tell, assume good
			// (what a conjured blade adds on top, fire waves and the like, doesn't show in its base damage:
			// half as good on paper as a carried weapon is good enough)
			if (dps >= out.meleeScore * 0.5f && (!out.bound || dps > out.meleeScore)) {
				out.bound = a_spell;
				out.boundWeapon = weap;
				out.melee = weap;
				out.meleeScore = std::max(dps, out.meleeScore);
			}
		};
		if (auto npc = a_actor->GetActorBase()) {
			if (auto list = npc->GetSpellList(); list && list->spells) {
				for (std::uint32_t i = 0; i < list->numSpells; ++i) considerBound(list->spells[i]);
			}
		}
		for (auto spell : a_actor->GetActorRuntimeData().addedSpells) considerBound(spell);

		auto avo = a_actor->AsActorValueOwner();
		if (out.melee) {
			out.meleeSkill = avo->GetActorValue(IsTwoHanded(out.melee) ? RE::ActorValue::kTwoHanded : RE::ActorValue::kOneHanded);
		}
		float bowSkill = out.bow ? avo->GetActorValue(RE::ActorValue::kArchery) : 0.0f;
		float spellSkill = out.spell ? avo->GetActorValue(SpellSkill(out.spell)) : 0.0f;
		// spell: magnitude grows with skill; one cast per charge time plus recovery
		float spellDps = out.spell ? SpellPower(out.spell) * (0.5f + spellSkill / 100.0f) / (out.spell->GetChargeTime() + 0.8f) : 0.0f;
		if (out.bow && out.spell) {
			if (spellDps > out.rangedScore) {
				out.bow = nullptr;
				out.ammo = nullptr;
			} else {
				out.spell = nullptr;
			}
		}
		if (out.spell) out.rangedScore = spellDps;
		out.rangedSkill = out.spell ? spellSkill : bowSkill;
		return out;
	}

	// Fighting at range: by the main (right) hand. A dagger with a spell in the off hand is a spellsword
	// up close (counting the off-hand spell made her back off from blades with a dagger in hand, and
	// put her stabs into the ranged damage tally).
	bool HoldsRanged(RE::Actor* a_actor)
	{
		for (bool left : { false }) {
			auto obj = a_actor->GetEquippedObject(left);
			if (!obj) continue;
			if (auto weap = obj->As<RE::TESObjectWEAP>()) {
				if (IsRangedWeapon(weap) || weap->IsStaff()) return true;
			} else if (auto spell = obj->As<RE::SpellItem>(); spell && IsAttackSpell(spell)) {
				return true;
			}
		}
		return false;
	}

	bool IsDrawingBow(RE::Actor* a_actor)
	{
		auto state = a_actor->AsActorState()->GetAttackState();
		return state >= RE::ATTACK_STATE_ENUM::kBowDraw && state <= RE::ATTACK_STATE_ENUM::kBowReleasing;
	}

	bool EquipRanged(RE::Actor* a_actor, const Options& a_options)
	{
		auto manager = RE::ActorEquipManager::GetSingleton();
		if (a_options.bow) {
			if (a_actor->GetEquippedObject(false) == a_options.bow) return false;
			manager->EquipObject(a_actor, a_options.bow);
			if (a_options.ammo) manager->EquipObject(a_actor, a_options.ammo, nullptr, 1);
			return true;
		}
		if (a_options.spell) {
			if (a_actor->GetEquippedObject(false) == a_options.spell) return false;
			manager->EquipSpell(a_actor, a_options.spell, nullptr);
			return true;
		}
		return false;
	}

	bool EquipMelee(RE::Actor* a_actor, const Options& a_options)
	{
		if (a_options.bound && a_options.boundWeapon) {
			if (a_actor->GetEquippedObject(false) == a_options.boundWeapon) return false;
			// paid in magicka like any cast; short of it, her carried weapon
			auto  avo = a_actor->AsActorValueOwner();
			float cost = a_options.bound->CalculateMagickaCost(a_actor);
			bool  both = !IsTwoHanded(a_options.boundWeapon);
			if (avo->GetActorValue(RE::ActorValue::kMagicka) >= cost * (both ? 2.0f : 1.0f)) {
				using S = RE::MagicSystem::CastingSource;
				for (auto source : both ? std::vector{ S::kRightHand, S::kLeftHand } : std::vector{ S::kRightHand }) {
					if (auto caster = a_actor->GetMagicCaster(source)) {
						caster->CastSpellImmediate(a_options.bound, false, a_actor, 1.0f, false, 0.0f, a_actor);
						avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -cost);
					}
				}
				auto name = a_options.bound->GetName();
				logger::info("{} conjures {}{}", a_actor->GetDisplayFullName(), name && *name ? name : "a weapon", both ? " in both hands" : "");
				return true;
			}
		}
		if (!a_options.melee || a_options.melee == a_options.boundWeapon || a_actor->GetEquippedObject(false) == a_options.melee) return false;
		RE::ActorEquipManager::GetSingleton()->EquipObject(a_actor, a_options.melee);
		return true;
	}
}
