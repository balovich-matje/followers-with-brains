#pragma once

// What each follower has learned about each kind of enemy, as a player learns a game: how well she
// reads its attacks, how quickly she reacts, and which way out works against it. Learned by
// fighting (faster with a higher weapon skill), fading when she hasn't met that kind for a while
// (but never back to nothing), and kept in the save.
//
// A kind of enemy is everything that moves the same way: the same animations (every bear, every
// troll), and for people also the kind of weapon they hold (sword and shield, greatsword, bow...).
namespace Experience
{
	constexpr const char* kWays[] = { "step back", "dodge left", "dodge right", "dodge back" };

	struct Reading
	{
		float chance = 1.0f;  // share of attacks she sees coming (0..1)
		float delay = 0.0f;   // seconds from the start of an attack to her reaction
	};

	std::uint32_t KindOf(RE::Actor* a_enemy);
	Reading       Read(RE::Actor* a_follower, RE::Actor* a_enemy);
	void          SawAttack(RE::Actor* a_follower, RE::Actor* a_enemy);  // every attack she faces teaches a little
	int           PickWay(RE::Actor* a_follower, RE::Actor* a_enemy);    // index into kWays
	void          Outcome(RE::FormID a_follower, std::uint32_t a_kind, int a_way, bool a_avoided);
	std::string   Summary(RE::Actor* a_follower, RE::Actor* a_enemy);    // for logs: how she does against it

	// How much that kind of enemy hurts her: a_share is the damage as a share of her health.
	void Hurt(RE::Actor* a_follower, RE::Actor* a_enemy, float a_share);
	// How readily she dodges this kind: hits above a_threshold of her health are worth avoiding.
	// relaxed: she knows it well and it barely hurts her, so she trades hits (big ones only matter).
	struct Stance
	{
		float threshold = 0.2f;
		bool  relaxed = false;
		float danger = 0.0f;  // share of her health a typical fight with one of these costs her
	};
	Stance StanceVs(RE::Actor* a_follower, RE::Actor* a_enemy);

	// co-save
	constexpr std::uint32_t kRecord = 'LERN';
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void Revert();
	void Apply();  // after a game is loaded: the loaded knowledge becomes current
	// across a hot reload of the brain (lab)
	void Stash();
	void Unstash();
}
