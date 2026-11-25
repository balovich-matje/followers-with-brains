#pragma once

// Potions, as a player drinks them: a healing one when hurt (one at a time: a heal over time that is
// still running isn't stacked), and long-lasting buffs at the start of a fight that is worth it.
// Potions are told apart by what they do, so overhauls that change them (RFAB's heal-over-time
// potions) work as they are.
namespace Potions
{
	// a_fodder: she knows the enemy she is fighting to be harmless (no buffs wasted on it)
	void Update(RE::Actor* a_follower, bool a_inCombat, bool a_fodder, float a_now);
	void Forget();
	std::string Describe(RE::Actor* a_actor);  // lab: what she carries and how each is read
}
