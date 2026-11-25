#pragma once

// Corpses in the way. A player walks through bodies and shoves them aside; an NPC (or a body the
// mod moves) gets stuck on them. So bodies in a follower's way are pushed aside.
namespace Bodies
{
	// A corpse within a_range of a_actor in the direction a_dir (flat, unit), or nullptr.
	RE::Actor* InTheWay(RE::Actor* a_actor, const RE::NiPoint3& a_dir, float a_range);
	// Shove a_body away from a_by. At most every half second per body.
	void Kick(RE::Actor* a_by, RE::Actor* a_body);
}
