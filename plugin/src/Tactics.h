#pragma once

// Each follower fights with a private, tuned copy of their combat style: the game's own combat AI
// then dodges, strafes, circles, falls back, punishes staggered enemies and bashes power attacks,
// all with its normal in-combat movement. No stats, perks or skills are touched.
namespace Tactics
{
	void Apply(RE::Actor* a_follower);    // idempotent
	void Restore(RE::Actor* a_follower);  // back to the original style
	void Forget();                        // after a load: the copies are gone with the old game
}
