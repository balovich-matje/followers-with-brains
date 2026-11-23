#pragma once

// What a follower can fight with, and switching between ranged and melee.
namespace Loadout
{
	struct Options
	{
		RE::TESObjectWEAP* melee = nullptr;     // best melee weapon carried
		RE::TESObjectWEAP* bow = nullptr;       // best bow/crossbow carried (only with ammo)
		RE::TESAmmo*       ammo = nullptr;
		RE::TESObjectWEAP* unusableBow = nullptr;  // carried, but no ammo for it
		RE::SpellItem*     spell = nullptr;     // best aimed attack spell known
		float              meleeSkill = 0.0f;
		float              rangedSkill = 0.0f;
		float              meleeScore = 0.0f;   // estimated damage per second
		float              rangedScore = 0.0f;

		[[nodiscard]] bool HasRanged() const { return bow || spell; }
		[[nodiscard]] bool HasMelee() const { return melee != nullptr; }
		// a_rangedBias > 1 favours ranged (close combat distance), < 1 favours melee (far).
		[[nodiscard]] bool PrefersRanged(float a_rangedBias = 1.0f) const
		{
			return HasRanged() && (!HasMelee() || rangedScore * a_rangedBias > meleeScore * 1.1f);
		}
	};

	Options Read(RE::Actor* a_actor);

	// Damage of one hit as the game works it out (skill, perks, buffs, tempering, target's armor).
	float HitDamage(RE::Actor* a_attacker, RE::Actor* a_target, RE::InventoryEntryData* a_entry);
	bool    HoldsRanged(RE::Actor* a_actor);
	bool    IsDrawingBow(RE::Actor* a_actor);

	// Return true when something was equipped.
	bool EquipRanged(RE::Actor* a_actor, const Options& a_options);
	bool EquipMelee(RE::Actor* a_actor, const Options& a_options);
}
