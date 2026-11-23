#pragma once

// Automated test runs. Active only when Data/SKSE/Plugins/FollowersWithBrains_Test.ini exists:
// loads a save from the main menu, spawns a scenario, traces the follower, writes a report, repeats,
// then quits the game.
namespace Test
{
	bool Active();
	void Install();      // on kDataLoaded
	void OnGameLoaded(); // on kPostLoadGame
	void Tick(float a_delta);
}
