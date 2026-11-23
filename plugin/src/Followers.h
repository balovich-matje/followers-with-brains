#pragma once

// Follower brain: runs every frame from the player's update and reacts to game events.
namespace Followers
{
	void Install();      // hooks and event sinks (call once on kDataLoaded)
	void Reset();        // clears per-follower state (new game / load)
	void Tick(float a_delta);

	// Co-save: undoes temporary speed boosts that were active when the game was saved.
	void OnSave(SKSE::SerializationInterface* a_intfc);
	void OnLoad(SKSE::SerializationInterface* a_intfc);
	void OnRevert(SKSE::SerializationInterface* a_intfc);
}
