#pragma once

// Development lab (on only when Data/SKSE/Plugins/FollowersWithBrains_Lab.ini exists):
// - records the player, followers and enemies nearby ten times a second (track.csv) and logs hits,
//   deaths and combat start/end, so a fight can be replayed and read afterwards;
// - runs the commands a helper outside the game writes to out.txt (spawn, fight, console, say, ...).
//   The "fwb <text>" console command that talks to the helper lives in the loader.
namespace Lab
{
	void Install();             // kDataLoaded
	void Shutdown();            // before a hot reload
	void Frame(float a_delta);  // every frame, main thread
	bool On();
}
