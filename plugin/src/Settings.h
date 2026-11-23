#pragma once

// All user settings. Stored in Data/SKSE/Plugins/FollowersWithBrains.ini and edited from the MCM.
namespace Settings
{
	enum class FollowDistance : int
	{
		kClose = 0,
		kMedium = 1,
		kFar = 2
	};

	struct Values
	{
		// Movement
		int  followDistance = 0;  // out of combat
		int  combatDistance = 0;  // how far from the player followers fight
		bool catchUp = true;
		bool teleport = true;
		bool combatLeash = true;

		// Combat
		bool  awareness = true;
		bool  retarget = true;
		bool  focusSummoners = true;
		bool  meleeSwitch = true;
		bool  retreat = true;
		float rangedRetreatDistance = 1200.0f;
		bool  smartWeapons = true;   // switch between ranged and melee by distance and skill
		bool  matchWeapons = true;   // out of combat, draw ranged when the player does
		bool  reflexes = true;       // react to enemy attacks the moment they start
		bool  zigzag = true;         // weave when closing in on archers and casters

		// Misc
		bool debug = false;
	};

	Values& Get();
	void    Load();
	void    Save();

	// Generic access for Papyrus (keys are the field names above).
	int   GetInt(std::string_view a_key);
	void  SetInt(std::string_view a_key, int a_value);
	float GetFloat(std::string_view a_key);
	void  SetFloat(std::string_view a_key, float a_value);
}
