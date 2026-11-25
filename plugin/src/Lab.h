#pragma once

// Development lab (on only when Data/SKSE/Plugins/FollowersWithBrains_Lab.ini exists):
// - records the player, followers and enemies nearby ten times a second (track.csv) and logs hits,
//   deaths and combat start/end, so a fight can be replayed and read afterwards;
// - adds the console command "fwb <text>": the text is written to in.txt for a helper outside the
//   game, and any commands the helper writes to out.txt (spawn, fight, console, say, ...) are run.
namespace Lab
{
	void Install();             // kDataLoaded
	void Frame(float a_delta);  // every frame, main thread
	bool On();
}
