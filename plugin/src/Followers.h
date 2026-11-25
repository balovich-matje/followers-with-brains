#pragma once

// Follower brain: runs every frame from the player's update and reacts to game events.
namespace Followers
{
	void Install();      // hooks and event sinks (call once on kDataLoaded)
	void Shutdown();     // undoes everything Install and the followers' updates did (before a hot reload)
	void Reset();        // clears per-follower state (new game / load)
	void Tick(float a_delta);

	struct DebugInfo
	{
		int   pref = 0;  // weapon kind the mod wants (1 melee, 2 ranged)
		float dealt[2]{};
		int   hits[2]{};
		int   stepOuts = 0, blocks = 0, bashes = 0, taken = 0, punishes = 0, zigzags = 0;
		float damageTaken = 0.0f;
		int   enemyAttacks = 0, avoided = 0;
	};
	DebugInfo GetDebugInfo(RE::Actor* a_actor);

	// The followers and enemies seen by the last logic pass (for the lab recorder).
	void Tracked(std::vector<RE::Actor*>& a_followers, std::vector<RE::Actor*>& a_enemies);

	// Co-save: undoes temporary speed boosts that were active when the game was saved.
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void OnLoad(SKSE::SerializationInterface* a_intfc);
	void OnRevert(SKSE::SerializationInterface* a_intfc);
}
