#pragma once

// Movement recorder (development aid). While Data/SKSE/Plugins/FollowersWithBrains_Record.ini exists,
// the player's movement is written every frame to FollowersWithBrains_Record/record.csv, so player
// moves (a zig-zag, a dodge) can be measured and reproduced for followers.
namespace Recorder
{
	void Install();
	void Frame(float a_delta);
}
