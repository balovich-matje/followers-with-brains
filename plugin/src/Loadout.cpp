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

		bool IsRangedWeapon(RE::TESObjectWEAP* a_weapon) { return a_weapon && (a_weapon->IsBow() || a_weapon->IsCrossbow()); }

		bool IsTwoHanded(RE::TESObjectWEAP* a_weapon)
		{
			return a_weapon->IsTwoHandedSword() || a_weapon->IsTwoHandedAxe();
		}
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
			if (auto weap = obj->As<RE::TESObjectWEAP>()) {
				if (weap->IsStaff() || !weap->GetPlayable()) continue;
				if (weap->IsBow()) {
					if (!bow || weap->GetAttackDamage() > bow->GetAttackDamage()) bow = weap;
				} else if (weap->IsCrossbow()) {
					if (!crossbow || weap->GetAttackDamage() > crossbow->GetAttackDamage()) crossbow = weap;
				} else if (weap->IsMelee() && !weap->IsHandToHandMelee()) {
					if (!out.melee || weap->GetAttackDamage() > out.melee->GetAttackDamage()) out.melee = weap;
				}
			} else if (auto ammo = obj->As<RE::TESAmmo>()) {
				auto& slot = ammo->IsBolt() ? bolts : arrows;
				if (!slot || ammo->data.damage > slot->data.damage) slot = ammo;
			}
		}
		// Ammo the follower already has nocked counts too (some followers carry it outside the inventory list).
		if (auto current = a_actor->GetCurrentAmmo()) {
			auto& slot = current->IsBolt() ? bolts : arrows;
			if (!slot) slot = current;
		}
		if (bow && arrows) {
			out.bow = bow;
			out.ammo = arrows;
		} else if (crossbow && bolts) {
			out.bow = crossbow;
			out.ammo = bolts;
		} else {
			out.unusableBow = bow ? bow : crossbow;
		}

		if (auto npc = a_actor->GetActorBase()) {
			if (auto list = npc->GetSpellList(); list && list->spells) {
				for (std::uint32_t i = 0; i < list->numSpells; ++i) ConsiderSpell(list->spells[i], out.spell);
			}
		}
		for (auto spell : a_actor->GetActorRuntimeData().addedSpells) ConsiderSpell(spell, out.spell);

		auto avo = a_actor->AsActorValueOwner();
		if (out.melee) {
			out.meleeSkill = avo->GetActorValue(IsTwoHanded(out.melee) ? RE::ActorValue::kTwoHanded : RE::ActorValue::kOneHanded);
		}
		float bowSkill = out.bow ? avo->GetActorValue(RE::ActorValue::kArchery) : 0.0f;
		float spellSkill = out.spell ? avo->GetActorValue(SpellSkill(out.spell)) : 0.0f;
		out.rangedSkill = std::max(bowSkill, spellSkill);
		if (out.bow && out.spell && spellSkill > bowSkill) out.bow = nullptr;  // use the better of the two
		if (out.bow && out.spell) out.spell = nullptr;
		return out;
	}

	bool HoldsRanged(RE::Actor* a_actor)
	{
		for (bool left : { false, true }) {
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
		if (!a_options.melee || a_actor->GetEquippedObject(false) == a_options.melee) return false;
		RE::ActorEquipManager::GetSingleton()->EquipObject(a_actor, a_options.melee);
		return true;
	}
}
