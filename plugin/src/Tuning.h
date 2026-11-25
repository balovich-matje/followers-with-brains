#pragma once

// Movement and reflex numbers. Defaults are built in; Data/SKSE/Plugins/FollowersWithBrains_Tuning.ini
// (key = value) overrides them and is re-read whenever a game is loaded, so they can be tuned without
// rebuilding the plugin.
namespace Tuning
{
	struct Values
	{
		// charging a shooter (measured from a player's running weave)
		float chargeSpeed = 510.0f;   // units/s
		float weave = 1.0f;           // sideways share of the run: 1.0 = 45 degrees off the line
		float flipMin = 0.25f;        // seconds between side changes
		float flipMax = 0.6f;
		// stepping out of a big hit
		float stepSpeed = 450.0f;     // units/s (a player's diagonal backpedal)
		float stepDiagonal = 0.5f;    // sideways share of the backpedal: 1 = 45 degrees, 0.5 = ~27, 0 = straight back
		float dodgeShare = 0.2f;      // dodge hits above this share of current health (power attacks always)
		float dodgeChance = 75.0f;    // percent
		float dodgeCooldown = 2.5f;   // seconds between dodges
		float finishMargin = 1.1f;    // keep attacking instead of dodging when her hits before the enemy's
		                              // swing lands come to this much of its health (1.1 = 10% to spare)
		float learnScale = 40.0f;     // attacks faced to learn ~63% of an enemy kind (higher = slower learning)
		float practice = 0.0f;        // test switch: 1 = followers read every attack, instantly (no learning curve)
		// closing back in to strike
		float closeSpeed = 380.0f;    // units/s
	};

	const Values& Get();
	void          Load();
	void          SetExtraFile(std::filesystem::path a_path);  // read after the normal file (test runs, lab)
	bool          IsKey(std::string_view a_key);
}
