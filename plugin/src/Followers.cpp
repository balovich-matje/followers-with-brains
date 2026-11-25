#include "Followers.h"
#include "Driver.h"
#include "Tuning.h"
#include "Recorder.h"
#include "Lab.h"
#include "Bodies.h"
#include "Experience.h"
#include "Potions.h"
#include "Host.h"
#include "Loadout.h"
#include "Papyrus.h"
#include "Settings.h"
#include "Tactics.h"
#include "Test.h"

namespace Followers
{
	namespace
	{

		// ------------------------------------------------------------------ tuning
		struct ModeTuning
		{
			float back;           // keep-offset distance behind the player (0 = game's own follow)
			float side;           // sideways spacing between followers
			float catchUpRadius;  // keep-offset: run to catch up past this
			float followRadius;   // keep-offset: stop inside this
			float boostDistance;  // speed boost past this distance
			float teleportDistance;
			float combatLeash;
		};

		constexpr ModeTuning kTuning[3] = {
			{ 130.0f, 70.0f, 150.0f, 50.0f, 350.0f, 1500.0f, 900.0f },     // close
			{ 320.0f, 110.0f, 500.0f, 180.0f, 700.0f, 2200.0f, 1500.0f },  // medium
			{ 0.0f, 0.0f, 0.0f, 0.0f, 2200.0f, 6400.0f, 5000.0f },         // far
		};

		constexpr float kSpeedBoost = 50.0f;
		constexpr float kLogicInterval = 0.1f;
		constexpr float kTeleportDelay = 3.0f;
		constexpr float kEnemyRange = 7000.0f;
		constexpr float kSummonerReach = 2500.0f;
		constexpr float kWatchSeconds = 1.2f;
		constexpr float kMarkSeconds = 15.0f;
		constexpr float kHealGrace = 4.0f;

		// ------------------------------------------------------------------ state
		struct FollowerState
		{
			int   offsetMode = -1;  // keep-offset currently applied (-1 none)
			int   offsetSlot = -1;
			float offsetApplied = -100.0f;
			float pathUntil = 0.0f;   // let the game's own (pathfinding) follow work until then
			bool  pathNoted = false;
			int   blockedCount = 0;
			float nextHop = 0.0f;
			int   hopFails = 0;
			float stallSince = -1.0f;
			RE::NiPoint3 followSample;
			float followSampleTime = -100.0f;
			float nextTrail = 0.0f;
			float nextGrind = 0.0f;        // choosing whom to cut down next
			float hazardUntil = 0.0f;      // hurt by a trap: the game's own movement until then
			RE::NiPoint3 bodyCheckPos;     // running in place (a body in the way)?
			float slowSince = -1.0f;
			float footstepsAt = -100.0f;   // last time she set off in the player's footsteps
			int   footstepFails = 0;       // footsteps runs blocked right away (then the game's pathing)
			float looseness = 1.0f;
			float otherSpaceSince = -1.0f;
			float driveStarted = -100.0f;
			bool  formationDrive = false;
			float speedBoost = 0.0f;
			std::map<RE::ActorValue, float> avMods;  // temporary actor value changes we made
			float recallUntil = 0.0f;                 // heading back to the player until then
			float healedUntil = 0.0f;                 // player healed this follower recently
			Loadout::Options loadout;
			float loadoutTime = -100.0f;
			float lastSwap = -100.0f;
			bool  swappedToRanged = false;
			int   overrides = 0;              // times the combat AI undid our weapon choice
			float respectAIUntil = 0.0f;
			float unreachableUntil = 0.0f;    // melee could not reach its target recently
			// can't reach the target: sticking close to the player until the way to it opens up
			bool  tagAlong = false;
			RE::ActorHandle tagTarget;
			float tagRetryAt = 0.0f;
			float engagingUntil = 0.0f;       // just told to attack: the game may not show combat yet
			float lastFinish = -100.0f;
			RE::ActorHandle chargeTarget;
			bool  chargeBlockNoted = false;
			// progress toward the combat target: no closer in 5 s = can't reach it (on a wall, across a gap)
			RE::ActorHandle progressTarget;
			float bestTargetDist = 0.0f;
			float bestTargetTime = 0.0f;
			float lastSprintKick = -100.0f;
			bool  mirroringSprint = false;
			std::deque<float> pressure;            // melee hits on her and backpedals forced on her (bow in hand)
			float meleeUntil = 0.0f;               // switched to melee under pressure: keep it until then
			float shotBlockedAt = -100.0f;         // her last arrow hit scenery (low cover in the way)
			bool  wasKnocked = false;
			float weaponStuckSince = -1.0f;        // weapon half drawn or half sheathed
			RE::ActorHandle addsFirstFor;          // a summoner too tough to rush: its summons first
			RE::NiPoint3 sprintCheckPos;           // sprinting on the spot (the AI sprinting into something)
			float sprintCheckTime = -1.0f;
			// measured damage while holding each kind of weapon (0 melee, 1 ranged)
			float dealt[2]{};
			float heldTime[2]{};
			int   hits[2]{};
			RE::FormID measuredWith[2]{};  // the weapon those numbers were measured with
			float prevTargetDist = -1.0f;
			float targetDistTime = -100.0f;
			bool  closing = false;
			float farSince = -1.0f;
			float lastStop = -100.0f;
			float lastRetarget = -100.0f;
			float lastLeash = -100.0f;
			bool  drewForMark = false;

			// melee: swings that did not land
			int   missedSwings = 0;
			float firstMissTime = 0.0f;
			RE::ActorHandle lastAttacker;
			float lastAttackedTime = -100.0f;
			float lastMeleeHitTime = -100.0f;  // what dodges are judged by: a tracking spell landing mid-dodge isn't a failed dodge
			RE::ActorHandle lastMeleeAttacker;

			// melee: stuck short of the target
			RE::NiPoint3 samplePos;
			float sampleTime = -100.0f;
			float stuckSince = -1.0f;
			float lastRepath = -100.0f;

			// summons
			RE::ActorHandle redirectedFrom;
			int   summonHits = 0;
			float summonHitStart = 0.0f;
			RE::ActorHandle ignoredSummon;
			float ignoreSummonUntil = 0.0f;
		};

		enum class EventType
		{
			kPlayerAttack,  // swing / bow release / offensive spell
			kPlayerHeal,  // b = healed follower (may be empty)
			kPlayerCast,
			kFollowerSwing,
			kHit,
			kMeleeHit,  // a weapon or claw that connected (not a spell, an arrow or a lingering effect)
			kShotBlocked,  // a = follower whose arrow or bolt hit scenery
			kDeath,
			kHazard  // b = an actor hurt by something that isn't an actor (a trap, fire, a fall)
		};

		struct QueuedEvent
		{
			EventType       type;
			RE::ActorHandle a;
			RE::ActorHandle b;
		};

		struct Sample
		{
			float        time;
			float        enemyDistance;
			RE::NiPoint3 playerPos;
		};

		std::mutex                                 g_lock;
		std::vector<QueuedEvent>                   g_queue;
		std::unordered_map<RE::FormID, FollowerState> g_state;
		std::vector<std::tuple<RE::FormID, RE::ActorValue, float>> g_pendingUndo;

		struct DamageRecord
		{
			RE::FormID attacker;
			float      amount;
		};
		std::vector<DamageRecord> g_damage;  // filled by the health damage hook, read in RunLogic

		// Weapon kind each follower should fight with (1 melee, 2 ranged). Read by the combat AI hook,
		// which can run on worker threads.
		std::mutex                                 g_prefLock;
		std::unordered_map<RE::FormID, int>        g_weaponPref;
		std::atomic<int>                           g_scoreCalls{ 0 };
		std::atomic<int>                           g_equipVetoes{ 0 };

		int GetWeaponPref(RE::CombatController* a_controller)
		{
			if (!a_controller) return 0;
			auto attacker = a_controller->attackerHandle.get();
			if (!attacker) return 0;
			std::scoped_lock lock{ g_prefLock };
			auto it = g_weaponPref.find(attacker->GetFormID());
			return it == g_weaponPref.end() ? 0 : it->second;
		}

		void SetWeaponPref(RE::Actor* a, int a_pref)
		{
			std::scoped_lock lock{ g_prefLock };
			if (a_pref == 0) {
				g_weaponPref.erase(a->GetFormID());
			} else {
				g_weaponPref[a->GetFormID()] = a_pref;
			}
		}
		int g_lastPlayerRanged = -1;

		float g_time = 0.0f;
		float g_logicTimer = 0.0f;
		float g_lastPlayerAttack = -100.0f;
		float g_lastHeal = -100.0f;
		float g_lastPlayerCast = -100.0f;
		float g_sprintSince = -1.0f;

		RE::ActorHandle g_watchTarget;
		float           g_watchTime = 0.0f;
		RE::ActorHandle g_marked;
		float           g_markedUntil = 0.0f;

		bool                g_retreating = false;
		float               g_retreatStart = 0.0f;
		std::deque<Sample>  g_samples;

		struct PendingHit
		{
			RE::ActorHandle target;
			float           time;
		};
		std::vector<PendingHit> g_pendingPlayerHits;

		// ------------------------------------------------------------------ helpers
		void Debug(std::string_view a_text)
		{
			logger::info("{}", a_text);
			if (Settings::Get().debug) {
				RE::DebugNotification(std::string{ a_text }.c_str());
			}
		}

		const char* Name(RE::Actor* a_actor) { return a_actor ? a_actor->GetDisplayFullName() : "none"; }

		float Dist(RE::TESObjectREFR* a, RE::TESObjectREFR* b) { return a->GetPosition().GetDistance(b->GetPosition()); }

		bool Alive(RE::Actor* a) { return a && !a->IsDead() && !a->IsDisabled(); }

		RE::Actor* Get(const RE::ActorHandle& h)
		{
			auto p = h.get();
			return p ? p.get() : nullptr;
		}

		RE::ActorHandle HandleOf(RE::TESObjectREFR* a_ref)
		{
			auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			return actor ? actor->GetHandle() : RE::ActorHandle{};
		}

		bool IsFollower(RE::Actor* a, RE::PlayerCharacter* a_player)
		{
			return a && a != a_player && Alive(a) && a->IsPlayerTeammate() && !a->IsCommandedActor() && !a->IsAMount();
		}

		bool IsWaiting(RE::Actor* a)
		{
			return a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f;
		}

		bool InScene(RE::Actor* a) { return a->GetCurrentScene() != nullptr; }

		RE::Actor* CombatTarget(RE::Actor* a) { return Get(a->GetActorRuntimeData().currentCombatTarget); }

		// Turn to face a_target and swing (a strike the mod starts: punish, finish, opening).
		// A dodge animation or the AI may have left her facing elsewhere; swinging at the air with
		// her back to the enemy is the last thing a player would do.
		void StrikeAt(RE::Actor* a, RE::Actor* a_target)
		{
			if (a_target) {
				RE::NiPoint3 to = a_target->GetPosition() - a->GetPosition();
				if (to.x * to.x + to.y * to.y > 1.0f) a->SetRotationZ(std::atan2(to.x, to.y));
			}
			a->NotifyAnimationGraph("attackStart"sv);
		}

		float MeleeReach(RE::Actor* f)
		{
			auto w = f->GetEquippedObject(false) ? f->GetEquippedObject(false)->As<RE::TESObjectWEAP>() : nullptr;
			return w ? std::max(120.0f, w->GetReach() * 140.0f) : 150.0f;
		}

		bool Reachable(RE::Actor* f, RE::Actor* e)
		{
			return std::fabs(e->GetPosition().z - f->GetPosition().z) < 120.0f && Driver::ClearLine(f, f->GetPosition(), e->GetPosition());
		}

		bool IsEnemyOf(RE::Actor* a_enemy, RE::PlayerCharacter* a_player)
		{
			return Alive(a_enemy) && a_enemy != a_player && !a_enemy->IsPlayerTeammate() && a_enemy->IsHostileToActor(a_player);
		}

		bool IsHostileSpell(RE::TESForm* a_form)
		{
			auto item = a_form ? a_form->As<RE::MagicItem>() : nullptr;
			if (!item) return false;
			for (auto effect : item->effects) {
				if (effect && effect->baseEffect && effect->baseEffect->IsHostile()) return true;
			}
			return false;
		}

		bool IsHealingSpell(RE::TESForm* a_form)
		{
			auto item = a_form ? a_form->As<RE::MagicItem>() : nullptr;
			if (!item) return false;
			for (auto effect : item->effects) {
				auto base = effect ? effect->baseEffect : nullptr;
				if (base && !base->IsHostile() && !base->IsDetrimental() && base->data.primaryAV == RE::ActorValue::kHealth) {
					return true;
				}
			}
			return false;
		}

		bool IsHealingEffect(RE::FormID a_effect)
		{
			auto base = RE::TESForm::LookupByID<RE::EffectSetting>(a_effect);
			return base && !base->IsHostile() && !base->IsDetrimental() && base->data.primaryAV == RE::ActorValue::kHealth;
		}

		bool IsRanged(RE::Actor* a) { return Loadout::HoldsRanged(a); }

		bool PlayerIsHealing(RE::PlayerCharacter* a_player)
		{
			using Source = RE::MagicSystem::CastingSource;
			for (auto source : { Source::kLeftHand, Source::kRightHand }) {
				auto caster = a_player->GetMagicCaster(source);
				if (caster && caster->currentSpell && caster->state.underlying() != 0 && IsHealingSpell(caster->currentSpell)) {
					return true;
				}
			}
			return false;
		}

		RE::NiPoint3 Forward(RE::TESObjectREFR* a_ref)
		{
			float z = a_ref->GetAngleZ();
			return { std::sin(z), std::cos(z), 0.0f };
		}

		// ------------------------------------------------------------------ speed boost
		// Temporary actor value change, undone later (and from the co-save if the game was saved meanwhile).
		void SetMod(RE::Actor* a, FollowerState& s, RE::ActorValue a_av, float a_amount)
		{
			float current = s.avMods.contains(a_av) ? s.avMods[a_av] : 0.0f;
			if (current == a_amount) return;
			a->AsActorValueOwner()->ModActorValue(a_av, a_amount - current);
			if (a_amount == 0.0f) {
				s.avMods.erase(a_av);
			} else {
				s.avMods[a_av] = a_amount;
			}
		}

		void SetBoost(RE::Actor* a, FollowerState& s, float a_amount)
		{
			if (s.speedBoost == a_amount) return;
			SetMod(a, s, RE::ActorValue::kSpeedMult, a_amount);
			// Speed changes only take effect when carry weight is recalculated.
			auto avo = a->AsActorValueOwner();
			avo->ModActorValue(RE::ActorValue::kCarryWeight, 0.01f);
			avo->ModActorValue(RE::ActorValue::kCarryWeight, -0.01f);
			s.speedBoost = a_amount;
		}

		// Cowardly for a moment: the combat AI turns and runs instead of backpedalling.
		void SetFleeing(RE::Actor* a, FollowerState& s, bool a_flee)
		{
			if (a_flee == s.avMods.contains(RE::ActorValue::kConfidence)) return;
			(void)a_flee;  // cowardice made followers shout surrender lines: no longer used, only undone
			SetMod(a, s, RE::ActorValue::kConfidence, 0.0f);
		}

		const Loadout::Options& GetLoadout(RE::Actor* a, FollowerState& s)
		{
			if (g_time - s.loadoutTime > 5.0f) {
				auto next = Loadout::Read(a);
				if (s.loadoutTime < 0.0f || next.melee != s.loadout.melee || next.bow != s.loadout.bow ||
					next.spell != s.loadout.spell || next.ammo != s.loadout.ammo) {
					logger::info("{} loadout: melee {} (skill {:.0f}, score {:.0f}), ranged {} (skill {:.0f}, score {:.0f}), ammo {}{}", Name(a),
						next.melee ? next.melee->GetName() : "-", next.meleeSkill, next.meleeScore,
						next.bow ? next.bow->GetName() : (next.spell ? next.spell->GetName() : "-"), next.rangedSkill, next.rangedScore,
						next.ammo ? next.ammo->GetName() : "-",
						next.unusableBow ? std::format(", {} has no ammo", next.unusableBow->GetName()) : std::string{});
				}
				s.loadout = next;
				s.loadoutTime = g_time;
			}
			return s.loadout;
		}

		void Swap(RE::Actor* a, FollowerState& s, bool a_ranged, std::string_view a_why)
		{
			if (g_time - s.lastSwap < 2.5f || Loadout::IsDrawingBow(a)) return;
			auto& options = GetLoadout(a, s);
			if (g_time < s.respectAIUntil) return;
			// the combat AI swapped back soon after our last change: count it, and give up for a while
			if (g_time - s.lastSwap < 6.0f && a_ranged == s.swappedToRanged) {
				if (++s.overrides >= 2) {
					s.respectAIUntil = g_time + 20.0f;
					s.overrides = 0;
					logger::info("  combat AI hooks so far: {} rating changes, {} refused equips", g_scoreCalls.load(), g_equipVetoes.load());
					Debug(std::format("{} keeps their own weapon choice for now", Name(a)));
					return;
				}
			} else if (g_time - s.lastSwap >= 6.0f) {
				s.overrides = 0;
			}
			bool  done = a_ranged ? Loadout::EquipRanged(a, options) : Loadout::EquipMelee(a, options);
			if (done) {
				s.lastSwap = g_time;
				s.swappedToRanged = a_ranged;
				Debug(std::format("{} switches to {} ({})", Name(a), a_ranged ? "ranged" : "melee", a_why));
				logger::info("  dps melee {:.1f} ({} hits/{:.0f}s), ranged {:.1f} ({} hits/{:.0f}s)",
					s.heldTime[0] > 0 ? s.dealt[0] / s.heldTime[0] : 0.0f, s.hits[0], s.heldTime[0],
					s.heldTime[1] > 0 ? s.dealt[1] / s.heldTime[1] : 0.0f, s.hits[1], s.heldTime[1]);
			}
		}

		void ClearOffset(RE::Actor* a, FollowerState& s)
		{
			if (s.offsetMode >= 0) {
				Papyrus::ClearOffset(a);
				s.offsetMode = -1;
				s.offsetSlot = -1;
			}
		}

		// Leave combat and head back to the player.
		void Disengage(RE::Actor* a, FollowerState& s)
		{
			Papyrus::StopCombat(a);
			a->EvaluatePackage(true, false);
			s.lastStop = g_time;
			s.offsetMode = -1;  // combat may have dropped the offset: apply it again
			s.offsetSlot = -1;
		}

		// The spot (player-local x = right, y = forward) a follower keeps next to the player.
		RE::NiPoint2 OffsetSpot(int a_mode, int a_slot)
		{
			auto& t = kTuning[a_mode];
			float n = static_cast<float>((a_slot + 1) / 2);
			float sign = a_slot % 2 ? 1.0f : -1.0f;
			return { a_slot == 0 ? 0.0f : sign * t.side * n, -(t.back + 50.0f * n) };  // behind
		}

		// ------------------------------------------------------------------ the player's trail
		// The route the player walked (a point every ~60 units, in this area). A follower that can't
		// find its own way (an enemy up a tower whose stairs the pathfinder misses) runs it instead.
		std::deque<RE::NiPoint3> g_trail;
		RE::TESObjectCELL*       g_trailCell = nullptr;
		RE::TESWorldSpace*       g_trailWorld = nullptr;

		void RecordTrail(RE::PlayerCharacter* a_player)
		{
			auto cell = a_player->GetParentCell();
			auto world = a_player->GetWorldspace();
			bool sameArea = world ? world == g_trailWorld : cell == g_trailCell;
			if (!sameArea) {
				g_trail.clear();
				g_trailCell = cell;
				g_trailWorld = world;
			}
			auto pos = a_player->GetPosition();
			if (g_trail.empty() || pos.GetDistance(g_trail.back()) >= 60.0f) {
				if (a_player->IsOnMount() || a_player->AsActorState()->IsSwimming()) return;
				g_trail.push_back(pos);
				if (g_trail.size() > 400) g_trail.pop_front();
			}
		}

		// Trail points from near the follower to near a_goal (or to the trail's end). Empty if the
		// trail doesn't pass close to both.
		std::vector<RE::NiPoint3> TrailRoute(RE::Actor* f, const RE::NiPoint3* a_goal)
		{
			std::vector<RE::NiPoint3> out;
			if (g_trail.size() < 3) return out;
			auto        fp = f->GetPosition();
			std::size_t start = g_trail.size();
			// join the trail at its most recent point she can walk straight to (not the nearest one,
			// which may be an older loop of the route that leads away from the player)
			for (std::size_t i = g_trail.size(); i-- > 0;) {
				if (fp.GetDistance(g_trail[i]) < 350.0f && std::fabs(g_trail[i].z - fp.z) < 120.0f &&
					Driver::ClearLine(f, fp, g_trail[i]) && Driver::GroundAlong(f, fp, g_trail[i])) {
					start = i;
					break;
				}
			}
			if (start >= g_trail.size()) return out;
			std::size_t end = g_trail.size() - 1;
			if (a_goal) {
				float bestGoal = 450.0f;
				std::size_t found = g_trail.size();
				for (std::size_t i = start; i < g_trail.size(); ++i) {
					float d = a_goal->GetDistance(g_trail[i]);
					if (d < bestGoal) {
						bestGoal = d;
						found = i;
					}
				}
				if (found >= g_trail.size()) return out;
				end = found;
			}
			if (end <= start + 1) return out;
			for (std::size_t i = start; i <= end; ++i) out.push_back(g_trail[i]);
			return out;
		}

		bool RunTrail(RE::Actor* f, FollowerState& s, RE::PlayerCharacter* a_player, RE::Actor* a_goal, std::string_view a_why,
			float a_cooldown = 4.0f)
		{
			if (g_time < s.nextTrail || Driver::IsDriving(f)) return false;
			if (a_why == "footsteps") {
				// footsteps runs that hit something right away: give up on them for a while
				std::string_view last{ Driver::LastEnd(f) };
				if (g_time - s.footstepsAt < 1.5f && (last == "blocked" || last == "edge")) ++s.footstepFails;
				if (s.footstepFails >= 2 || Dist(f, a_player) < 200.0f) return false;
			}
			s.nextTrail = g_time + a_cooldown;
			RE::NiPoint3 goal;
			if (a_goal) goal = a_goal->GetPosition();
			auto route = TrailRoute(f, a_goal ? &goal : nullptr);
			if (!a_goal) {
				// to the player: stop a step short of them, not on top of them
				auto pp = a_player->GetPosition();
				while (!route.empty() && route.back().GetDistance(pp) < 140.0f) route.pop_back();
				if (route.size() < 2) return false;
				// a route much longer than the way there (a loop): not worth it
				float length = route.front().GetDistance(f->GetPosition());
				for (std::size_t i = 1; i < route.size(); ++i) length += route[i].GetDistance(route[i - 1]);
				if (length > 2.5f * f->GetPosition().GetDistance(pp) + 300.0f) return false;
			}
			if (route.empty()) return false;
			Driver::Order order;
			order.kind = Driver::Kind::kPath;
			order.target = a_player->GetHandle();
			order.path = std::move(route);
			// to the player: faster than they move while she is well behind, or she never catches up
			order.speed = a_goal ? Tuning::Get().chargeSpeed :
			              Dist(f, a_player) > 450.0f ? 890.0f :
			              a_player->AsActorState()->IsSprinting() ? 715.0f : Tuning::Get().chargeSpeed;
			order.duration = 20.0f;
			if (!Driver::Start(f, order)) return false;
			if (a_why == "footsteps") {
				s.footstepsAt = g_time;
				logger::info("{} takes your route (footsteps, {} points)", Name(f), order.path.size());
			} else {
				Debug(std::format("{} takes your route ({})", Name(f), a_why));
			}
			return true;
		}

		// A follower stuck below a rock the player is on, above a drop the player went down, or at a fence:
		// hop there. Returns true when a hop started.
		bool TryHop(RE::Actor* f, FollowerState& s, RE::PlayerCharacter* a_player)
		{
			if (g_time < s.nextHop || Driver::IsDriving(f) || s.hopFails >= 4) return false;
			RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
			auto         fp = f->GetPosition();
			auto         pp = a_player->GetPosition();
			RE::NiPoint3 flat = pp - fp;
			flat.z = 0.0f;
			float d = flat.Length();
			if (d < 1.0f) return false;
			RE::NiPoint3 dir = flat / d;
			float        dz = pp.z - fp.z;
			RE::NiPoint3 landing;
			const char*  what = nullptr;
			s.nextHop = g_time + 2.0f;

			if (dz > 60.0f && dz < 260.0f && d < 600.0f) {
				// up onto the player's level, a step short of the player
				landing = pp - dir * std::min(110.0f, d * 0.5f);
				if (!Driver::GroundAt(f, landing, 120.0f) || std::fabs(landing.z - pp.z) > 60.0f) {
					++s.hopFails;
					return false;
				}
				if (!Driver::ClearLine(f, fp + up * (dz + 30.0f), landing + up * 10.0f)) {
					++s.hopFails;
					return false;
				}
				what = "up";
			} else if (dz < -80.0f && dz > -700.0f && d < 800.0f) {
				// down toward the player
				landing = fp + dir * std::min(260.0f, d * 0.6f);
				// the first ground straight down from her own height (searching from the player's level skipped
				// the stairs she stood on and dropped her through them), clear air out to it, and it must
				// be the player's level, not another turn of the stairs
				landing.z = fp.z - 320.0f;
				auto above = fp + dir * std::min(260.0f, d * 0.6f);
				if (!Driver::ClearLine(f, fp, above) || !Driver::GroundAt(f, landing, 380.0f) || landing.z > fp.z - 40.0f ||
				    std::fabs(landing.z - pp.z) > 80.0f) {
					++s.hopFails;
					return false;
				}
				what = "down";
			} else if (std::fabs(dz) < 80.0f) {
				// over a fence or low wall: blocked at the knee, clear at head height, ground beyond
				bool knee = Driver::ClearLine(f, fp - up * 25.0f, fp - up * 25.0f + dir * 120.0f);
				bool head = Driver::ClearLine(f, fp + up * 80.0f, fp + up * 80.0f + dir * 180.0f);
				if (knee || !head) {
					++s.hopFails;
					return false;
				}
				landing = fp + dir * 180.0f;
				if (!Driver::GroundAt(f, landing, 150.0f) || std::fabs(landing.z - fp.z) > 120.0f) {
					++s.hopFails;
					return false;
				}
				what = "over";
			} else {
				return false;
			}
			Driver::Order order;
			order.kind = Driver::Kind::kHop;
			order.target = a_player->GetHandle();
			order.landing = landing;
			if (!Driver::Start(f, order)) return false;
			s.hopFails = 0;
			s.stallSince = -1.0f;
			Debug(std::format("{} hops {}", Name(f), what));
			return true;
		}

		// The area a follower keeps: anywhere around the player, close by (closer indoors, where rooms
		// are cramped and a few steps away is often behind a pillar).
		Driver::Order FormationOrder(bool a_indoors = false)
		{
			Driver::Order order;
			order.kind = Driver::Kind::kFormation;
			order.zoneAngle = 180.0f;
			order.zoneHalf = 180.0f;
			order.zoneNear = a_indoors ? 80.0f : 120.0f;
			order.zoneFar = a_indoors ? 220.0f : 330.0f;
			return order;
		}

		// Nothing in the way at chest height, at head height and (on level ground) at knee height:
		// pillars, beams, low walls and clutter that a chest-high check alone misses.
		bool ClearPath(RE::Actor* f, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
			if (!Driver::ClearLine(f, a_from, a_to)) return false;
			if (!Driver::ClearLine(f, a_from + up * 50.0f, a_to + up * 50.0f)) return false;
			if (std::fabs(a_to.z - a_from.z) < 30.0f && !Driver::ClearLine(f, a_from - up * 25.0f, a_to - up * 25.0f)) return false;
			return true;
		}

		RE::NiPoint3 SpotInWorld(RE::Actor* a_player, RE::NiPoint2 a_spot)
		{
			float z = a_player->GetAngleZ();
			RE::NiPoint3 fwd{ std::sin(z), std::cos(z), 0.0f };
			RE::NiPoint3 right{ std::cos(z), -std::sin(z), 0.0f };
			return a_player->GetPosition() + right * a_spot.x + fwd * a_spot.y;
		}

		void ApplyOffset(RE::Actor* a, FollowerState& s, RE::PlayerCharacter* a_player, int a_mode, int a_slot)
		{
			// re-applied every few seconds: teleports, doors and loads silently drop it
			if (s.offsetMode == a_mode && s.offsetSlot == a_slot && g_time - s.offsetApplied < 3.0f) return;
			auto& t = kTuning[a_mode];
			auto  spot = OffsetSpot(a_mode, a_slot);
			Papyrus::KeepOffset(a, a_player, spot.x, spot.y, t.catchUpRadius, t.followRadius);
			if (s.offsetMode != a_mode || s.offsetSlot != a_slot) {
				logger::info("{} keeps offset ({:.0f}, {:.0f}) mode {}", Name(a), spot.x, spot.y, a_mode);
			}
			s.offsetMode = a_mode;
			s.offsetSlot = a_slot;
			s.offsetApplied = g_time;
		}

		// A sneaking actor can only walk: stand up to fight (a player stops sneaking once a fight is on).
		void StandUp(RE::Actor* a)
		{
			auto state = a->AsActorState();
			if (!state->IsSneaking()) return;
			state->actorState1.sneaking = 0;
			a->NotifyAnimationGraph("SneakStop"sv);
		}

		void StartCombat(RE::Actor* a, FollowerState& s, RE::Actor* a_target, std::string_view a_why)
		{
			if (!Alive(a_target) || CombatTarget(a) == a_target) return;
			StandUp(a);
			ClearOffset(a, s);
			s.lastRetarget = g_time;
			s.engagingUntil = g_time + 2.0f;
			s.missedSwings = 0;
			s.stuckSince = -1.0f;
			s.sampleTime = g_time;
			s.samplePos = a->GetPosition();
			Papyrus::StartCombat(a, a_target);
			Debug(std::format("{} -> {} ({})", Name(a), Name(a_target), a_why));
		}

		// ------------------------------------------------------------------ world scan
		struct Scan
		{
			RE::PlayerCharacter*     player = nullptr;
			std::vector<RE::Actor*>  followers;
			std::vector<RE::Actor*>  enemies;  // hostile, alive, in combat, near the player
		};

		Scan DoScan(RE::PlayerCharacter* a_player)
		{
			Scan scan;
			scan.player = a_player;
			auto lists = RE::ProcessLists::GetSingleton();
			if (!lists) return scan;
			for (auto& handle : lists->highActorHandles) {
				auto actor = Get(handle);
				if (!actor) continue;
				if (IsFollower(actor, a_player)) {
					scan.followers.push_back(actor);
				} else if (IsEnemyOf(actor, a_player) && actor->IsInCombat() && Dist(actor, a_player) < kEnemyRange) {
					scan.enemies.push_back(actor);
				}
			}
			// A follower left far behind drops out of the nearby (high process) actors, and then nothing here
			// looked after her: stuck somewhere the game can't path from (no navmesh in a custom house), she
			// stayed there for good. Followers further out are looked after too (catch-up teleport).
			for (auto list : { &lists->middleHighActorHandles, &lists->middleLowActorHandles, &lists->lowActorHandles }) {
				for (auto& handle : *list) {
					auto actor = Get(handle);
					if (actor && IsFollower(actor, a_player) && std::ranges::find(scan.followers, actor) == scan.followers.end()) {
						scan.followers.push_back(actor);
					}
				}
			}
			return scan;
		}

		bool TargetsParty(RE::Actor* a_enemy, const Scan& a_scan)
		{
			auto target = CombatTarget(a_enemy);
			if (!target) return false;
			if (target == a_scan.player) return true;
			return std::ranges::find(a_scan.followers, target) != a_scan.followers.end();
		}

		// Best enemy for a follower: whoever is fighting the player first, then the closest to the player.
		// ---- enemies her weapon can't hurt (immune, invulnerable, a whirlwind the modpack made an actor):
		// a player hits one a few times, sees the health bar not move, and leaves it alone for a while.
		struct HitTally
		{
			float hpAtFirst = 0.0f;
			float firstTime = -100.0f;
			float lastTime = -100.0f;
			int   hits = 0;
		};
		std::map<std::pair<RE::FormID, RE::FormID>, HitTally> g_hitTally;      // (follower, enemy): her hits
		std::map<std::pair<RE::FormID, RE::FormID>, HitTally> g_knockTally;    // (follower, enemy): it knocking her down
		struct FutileMark
		{
			float           until = 0.0f;
			RE::ActorHandle who;      // the one she tried it on
			float           hp = 0.0f;  // its health then: if that drops, it can be hurt after all
		};
		// keyed by what the enemy is made from: every whirlwind of a boss, but not every mage when one
		// boss is invulnerable for a while
		std::map<std::pair<RE::FormID, RE::FormID>, FutileMark> g_futile;

		RE::FormID MadeFrom(RE::Actor* e)
		{
			if (auto lvl = e->extraList.GetByType<RE::ExtraLeveledCreature>(); lvl && lvl->originalBase) return lvl->originalBase->GetFormID();
			auto base = e->GetActorBase();
			return base ? base->GetFormID() : e->GetFormID();
		}

		bool Futile(RE::Actor* f, RE::Actor* e)
		{
			if (!f || !e) return false;
			auto it = g_futile.find({ f->GetFormID(), MadeFrom(e) });
			return it != g_futile.end() && g_time < it->second.until;
		}

		void MarkFutile(RE::Actor* f, RE::Actor* e, std::string_view a_why)
		{
			float hp = e->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			g_futile[{ f->GetFormID(), MadeFrom(e) }] = { g_time + 45.0f, e->GetHandle(), hp };
			Debug(std::format("{} can't {} {}: leaves it alone", Name(f), a_why, Name(e)));
		}

		// its health bar moved (her hits, the player's, a spell): vulnerable again (bosses with
		// invulnerable phases)
		void RecheckFutile(RE::Actor* f)
		{
			for (auto& [key, mark] : g_futile) {
				if (key.first != f->GetFormID() || g_time >= mark.until) continue;
				auto e = Get(mark.who);
				if (!Alive(e)) continue;
				auto  avo = e->AsActorValueOwner();
				float maxHp = std::max(1.0f, avo->GetPermanentActorValue(RE::ActorValue::kHealth));
				if (avo->GetActorValue(RE::ActorValue::kHealth) < mark.hp - 0.01f * maxHp) {
					mark.until = 0.0f;
					Debug(std::format("{}: {} can be hurt now", Name(f), Name(e)));
				}
			}
		}

		// knocked down by the same enemy twice while it took no damage: can't even get near it
		void TallyKnock(RE::Actor* f, RE::Actor* e)
		{
			float hp = e->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			auto& t = g_knockTally[{ f->GetFormID(), e->GetFormID() }];
			if (g_time - t.firstTime > 20.0f) {
				t = { hp, g_time, g_time, 1 };
				return;
			}
			float maxHp = std::max(1.0f, e->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth));
			if (++t.hits >= 2 && t.hpAtFirst - hp < 0.01f * maxHp) {
				MarkFutile(f, e, "get near");
				t = {};
			}
		}

		void TallyHit(RE::Actor* f, RE::Actor* e)
		{
			auto  avo = e->AsActorValueOwner();
			float hp = avo->GetActorValue(RE::ActorValue::kHealth);
			auto& t = g_hitTally[{ f->GetFormID(), e->GetFormID() }];
			if (g_time - t.lastTime < 0.3f) return;  // one swing: the weapon and its enchantments, perks
			t.lastTime = g_time;
			if (g_time - t.firstTime > 12.0f) {
				t = { hp, g_time, g_time, 1 };
				return;
			}
			if (++t.hits < 4) return;
			float maxHp = std::max(1.0f, avo->GetPermanentActorValue(RE::ActorValue::kHealth));
			if (t.hpAtFirst - hp < 0.01f * maxHp) MarkFutile(f, e, "hurt");
			t = { hp, g_time, g_time, 1 };
		}

		RE::Actor* PickEnemy(RE::Actor* a, const Scan& a_scan, RE::Actor* a_exclude = nullptr, float a_maxFromPlayer = kEnemyRange)
		{
			RE::Actor* best = nullptr;
			float      bestScore = std::numeric_limits<float>::max();
			for (auto enemy : a_scan.enemies) {
				if (enemy == a_exclude || !TargetsParty(enemy, a_scan) || Dist(enemy, a_scan.player) > a_maxFromPlayer) continue;
				float score = Dist(enemy, a_scan.player) * 0.6f + Dist(enemy, a) * 0.4f;
				if (CombatTarget(enemy) == a_scan.player) score -= 400.0f;
				if (Futile(a, enemy)) score += 100000.0f;  // only if there is nothing else
				if (score < bestScore) {
					bestScore = score;
					best = enemy;
				}
			}
			return best;
		}

		RE::Actor* NearestAttacker(RE::Actor* a, const Scan& a_scan, RE::Actor* a_exclude)
		{
			RE::Actor* best = nullptr;
			float      bestDist = std::numeric_limits<float>::max();
			for (auto enemy : a_scan.enemies) {
				if (enemy == a_exclude || CombatTarget(enemy) != a || Futile(a, enemy)) continue;
				float d = Dist(enemy, a);
				if (d < bestDist) {
					bestDist = d;
					best = enemy;
				}
			}
			return best;
		}

		// ------------------------------------------------------------------ awareness
		void EngageAll(const Scan& a_scan, RE::Actor* a_target, std::string_view a_why)
		{
			if (!Alive(a_target) || g_retreating) return;
			for (auto f : a_scan.followers) {
				if (IsWaiting(f)) continue;
				auto& s = g_state[f->GetFormID()];
				auto  current = CombatTarget(f);
				if (!f->IsInCombat() || !Alive(current)) {
					StartCombat(f, s, a_target, a_why);
				}
			}
		}

		void UpdateWatch(const Scan& a_scan)
		{
			auto player = a_scan.player;
			auto pick = RE::CrosshairPickData::GetSingleton();
			RE::Actor* aimed = nullptr;
			if (pick) {
				auto ref = pick->targetActor.get();
				aimed = ref ? ref->As<RE::Actor>() : nullptr;
			}
			auto state = player->AsActorState();
			bool ready = state->IsWeaponDrawn() || state->IsSneaking();
			if (!aimed || !ready || !IsEnemyOf(aimed, player)) {
				g_watchTarget = {};
				g_watchTime = 0.0f;
				return;
			}
			if (Get(g_watchTarget) != aimed) {
				g_watchTarget = aimed->GetHandle();
				g_watchTime = 0.0f;
			}
			g_watchTime += kLogicInterval;

			auto attack = state->GetAttackState();
			bool aiming = attack >= RE::ATTACK_STATE_ENUM::kBowDraw && attack <= RE::ATTACK_STATE_ENUM::kBowReleasing;
			if (aiming || g_watchTime >= kWatchSeconds) {
				if (Get(g_marked) != aimed) Debug(std::format("Marked {}", Name(aimed)));
				g_marked = aimed->GetHandle();
				g_markedUntil = g_time + kMarkSeconds;
				for (auto f : a_scan.followers) {
					auto& s = g_state[f->GetFormID()];
					if (!f->IsInCombat() && !IsWaiting(f) && !f->AsActorState()->IsWeaponDrawn()) {
						f->DrawWeaponMagicHands(true);
						s.drewForMark = true;
					}
				}
			}
		}

		RE::Actor* AttackTarget(RE::PlayerCharacter* a_player)
		{
			auto pick = RE::CrosshairPickData::GetSingleton();
			if (pick) {
				auto ref = pick->targetActor.get();
				auto aimed = ref ? ref->As<RE::Actor>() : nullptr;
				if (aimed && IsEnemyOf(aimed, a_player)) return aimed;
			}
			auto marked = Get(g_marked);
			if (marked && g_time < g_markedUntil && IsEnemyOf(marked, a_player) && Dist(marked, a_player) < kEnemyRange) {
				return marked;
			}
			return nullptr;
		}

		// ------------------------------------------------------------------ retreat
		void UpdateRetreat(const Scan& a_scan)
		{
			auto  player = a_scan.player;
			auto& cfg = Settings::Get();
			if (!cfg.retreat || !cfg.combatMovement || !player->IsInCombat() || a_scan.enemies.empty()) {
				if (g_retreating) Debug("Retreat over");
				g_retreating = false;
				g_samples.clear();
				return;
			}
			if (PlayerIsHealing(player)) g_lastHeal = g_time;

			float nearest = std::numeric_limits<float>::max();
			for (auto e : a_scan.enemies) nearest = std::min(nearest, Dist(e, player));
			g_samples.push_back({ g_time, nearest, player->GetPosition() });
			while (!g_samples.empty() && g_time - g_samples.front().time > 7.0f) g_samples.pop_front();
			// a retreat is a sustained sprint: track how long the player has sprinted without a break
			if (player->AsActorState()->IsSprinting()) {
				if (g_sprintSince < 0.0f) g_sprintSince = g_time;
			} else {
				g_sprintSince = -1.0f;
			}

			auto sampleAgo = [&](float a_seconds) -> const Sample* {
				for (auto& s : g_samples) {
					if (g_time - s.time <= a_seconds) return &s;
				}
				return nullptr;
			};

			bool healing = g_time - g_lastHeal < kHealGrace;
			auto now = player->GetPosition();

			if (!g_retreating) {
				bool  sprint = player->AsActorState()->IsSprinting();
				// A retreat needs all of: sprinting for 5 s without a break, the nearest enemy further away
				// than 5 s ago, and no attack or spell from the player in those 5 s. Running circles round
				// an enemy is not a retreat.
				(void)sprint;
				bool  sprintedLong = g_sprintSince >= 0.0f && g_time - g_sprintSince >= 5.0f;
				auto  old = sampleAgo(5.0f);
				if (!sprintedLong || !old || g_time - old->time < 4.8f) return;
				float gained = nearest - old->enemyDistance;
				float moved = now.GetDistance(old->playerPos);
				bool  acted = g_time - g_lastPlayerAttack < 5.0f || g_time - g_lastHeal < 5.0f || g_time - g_lastPlayerCast < 5.0f;
				if (gained > 0.0f && !acted) {
					g_retreating = true;
					g_retreatStart = g_time;
					Debug(std::format("Retreat! (gained {:.0f}, moved {:.0f})", gained, moved));
				}
			} else {
				auto  old = sampleAgo(1.5f);
				float moved = old ? now.GetDistance(old->playerPos) : 1000.0f;
				bool  stopped = old && g_time - old->time > 1.2f && moved < 60.0f;
				if (g_lastPlayerAttack > g_retreatStart || healing || stopped) {
					g_retreating = false;
					Debug("Retreat over");
				}
			}
		}

		// ------------------------------------------------------------------ events
		void HandleEvent(const QueuedEvent& a_event, const Scan& a_scan)
		{
			auto& cfg = Settings::Get();
			auto  player = a_scan.player;
			auto  a = Get(a_event.a);
			auto  b = Get(a_event.b);

			switch (a_event.type) {
			case EventType::kPlayerAttack:
				g_lastPlayerAttack = g_time;
				if (cfg.awareness) {
					if (auto target = AttackTarget(player)) EngageAll(a_scan, target, "you attacked");
				}
				break;

			case EventType::kPlayerCast:
				g_lastPlayerCast = g_time;
				break;

			case EventType::kPlayerHeal:
				g_lastHeal = g_time;
				if (b && IsFollower(b, player)) g_state[b->GetFormID()].healedUntil = g_time + 5.0f;
				break;

			case EventType::kFollowerSwing:
				if (a && IsFollower(a, player) && !IsRanged(a)) {
					auto& s = g_state[a->GetFormID()];
					if (s.missedSwings == 0) s.firstMissTime = g_time;
					++s.missedSwings;
				}
				break;

			case EventType::kHazard:
				if (b && IsFollower(b, player)) {
					auto& s = g_state[b->GetFormID()];
					if (g_time >= s.hazardUntil) Debug(std::format("{} hurt by a trap or hazard: the game moves her for a moment", Name(b)));
					s.hazardUntil = g_time + 3.0f;
					if (Driver::IsDriving(b)) Driver::Stop(b);
				}
				break;

			case EventType::kShotBlocked:
				if (a && IsFollower(a, player)) g_state[a->GetFormID()].shotBlockedAt = g_time;
				break;
			case EventType::kMeleeHit:  // a = attacker, b = victim
				if (a && b && IsFollower(a, player) && b != player && !b->IsPlayerTeammate() && Alive(b)) TallyHit(a, b);
				if (a && b && IsFollower(b, player)) {
					auto& s = g_state[b->GetFormID()];
					if (IsRanged(b) && g_time - s.lastMeleeHitTime > 0.4f) s.pressure.push_back(g_time);  // one swing, one count
					s.lastMeleeHitTime = g_time;
					s.lastMeleeAttacker = a->GetHandle();
				}
				break;
			case EventType::kHit:  // a = attacker, b = victim
				if (!a || !b) break;
				if (a == player && !b->IsPlayerTeammate() && Alive(b)) {
					g_lastPlayerAttack = g_time;
					g_pendingPlayerHits.push_back({ b->GetHandle(), g_time + 0.15f });
				} else if (IsFollower(a, player)) {
					g_state[a->GetFormID()].missedSwings = 0;
				}
				if (IsFollower(b, player) && a != player && !a->IsPlayerTeammate()) {
					auto& s = g_state[b->GetFormID()];
					s.lastAttacker = a->GetHandle();
					s.lastAttackedTime = g_time;
					if (Get(s.redirectedFrom) == a) {
						if (g_time - s.summonHitStart > 6.0f) {
							s.summonHitStart = g_time;
							s.summonHits = 0;
						}
						if (++s.summonHits >= 2) {
							s.ignoredSummon = a->GetHandle();
							s.ignoreSummonUntil = g_time + 15.0f;
							s.redirectedFrom = {};
							Debug(std::format("{} fights back against {}", Name(b), Name(a)));
						}
					}
				}
				if (b == player && a != player) {
					// someone hit the player: idle followers join in
					if (cfg.awareness && IsEnemyOf(a, player)) EngageAll(a_scan, a, "you were attacked");
				}
				break;

			case EventType::kDeath:  // a = dead actor
				// how she does against that kind of enemy now (followers who fought it)
				if (a && a != player && !a->IsPlayerTeammate()) {
					for (auto f : a_scan.followers) {
						if (CombatTarget(f) == a || Get(g_state[f->GetFormID()].lastAttacker) == a) {
							Debug(std::format("{} vs {}", Name(f), Experience::Summary(f, a)));
						}
					}
				}
				if (!cfg.retarget || !a) break;
				for (auto f : a_scan.followers) {
					auto& s = g_state[f->GetFormID()];
					if (CombatTarget(f) == a) {
						if (auto next = PickEnemy(f, a_scan, a)) StartCombat(f, s, next, "target died");
					}
				}
				break;
			}
		}

		// ------------------------------------------------------------------ per-follower logic
		// ------------------------------------------------------------------ reflexes (every frame)
		struct ReflexState
		{
			int   action = 0;  // 0 none, 1 step out, 2 block, 3 bash, 4 dodge animation
			float nextRoomCheck = 0.0f;  // keeping room to back off (wall behind her?)
			float nextOpening = 0.0f;    // looking for an opening to strike
			float nextGuardBreak = 0.0f; // a raised guard: a heavy attack into it
			float nextKite = 0.0f;       // bow or staff in hand: backing off from blades
			float blindSince = -1.0f;    // bow drawn, target out of sight
			float blockSince = -1.0f;
			float guardCheckAt = -1.0f;  // did that heavy attack stagger it?
			RE::ActorHandle guardTarget;
			std::uint32_t   guardKind = 0;
			float nextFlankCheck = 0.0f; // enemies getting behind her?
			float nextRegroup = 0.0f;    // outnumbered: back to the player's side
			float approachCheck = 0.0f;  // closing on a distant target? (dist then)
			float approachDist = -1.0f;
			float idleSince = -1.0f;     // her target standing idle since
			RE::ActorHandle seen;      // an attack she has seen coming, to react to at reactAt
			float reactAt = -1.0f;
			float until = 0.0f;
			float cooldownUntil = 0.0f;
			float punishAt = -1.0f;
			float dodgeReadyAt = 0.0f;
			bool  punishApproachStarted = false;
			RE::ActorHandle against;
			float bashTime = -100.0f;
			bool  bashPending = false;
			bool  offset = false;
		};

		std::unordered_map<RE::FormID, ReflexState>                  g_reflex;

		// what came of each way out (scored a moment later: was she hit?)
		struct Outcome
		{
			RE::FormID      follower;
			RE::ActorHandle enemy;
			std::uint32_t   kind;
			int             way;
			float           time;
		};
		std::vector<Outcome> g_outcomes;
		std::unordered_map<RE::FormID, float> g_drawStart;  // enemy: when its current wind-up began

		std::unordered_map<RE::FormID, RE::ATTACK_STATE_ENUM>         g_enemyAttack;  // last seen attack state
		std::unordered_map<RE::FormID, bool>                          g_enemyStagger;
		std::unordered_map<RE::FormID, int>                           g_bashFails;    // enemies that shrug off bashes
		// damage followers took, for what they learn about their enemies (filled on any thread)
		struct HurtEvent
		{
			RE::FormID      victim;
			RE::ActorHandle attacker;
			float           damage;
		};
		std::vector<HurtEvent> g_hurt;

		std::vector<RE::ActorHandle> g_lastFollowers;
		std::vector<RE::ActorHandle> g_lastEnemies;

		struct ReflexStats
		{
			int stepOuts = 0, blocks = 0, bashes = 0, taken = 0, punishes = 0, zigzags = 0;
			int enemyAttacks = 0;   // attack starts by enemies within 450 of a follower
			int avoided = 0;        // reactions with no damage taken in the next 1.2 s
			float damageTaken = 0.0f;
		} g_stats;
		float g_lastFollowerHurt = -100.0f;
		std::vector<float> g_pendingCheck;  // reaction times waiting for their 1.2 s verdict

		bool HasShield(RE::Actor* a)
		{
			auto left = a->GetEquippedObject(true);
			return left && left->IsArmor();
		}

		bool IsStaggered(RE::Actor* a)
		{
			bool value = false;
			return a->GetGraphVariableBool("IsStaggering", value) && value;
		}

		void EndReflex(RE::Actor* f, ReflexState& r)
		{
			if (r.action == 2 || r.action == 3) f->NotifyAnimationGraph("blockStop"sv);
			if (r.offset) {
				Papyrus::ClearOffset(f);
				r.offset = false;
				auto& s = g_state[f->GetFormID()];
				s.offsetMode = -1;
				s.offsetSlot = -1;
			}
			r.action = 0;
		}

		// Charging or casting a spell (or a shout) right now, in any hand.
		bool IsCasting(RE::Actor* a)
		{
			using S = RE::MagicCaster::State;
			for (auto src : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand,
					 RE::MagicSystem::CastingSource::kOther, RE::MagicSystem::CastingSource::kInstant }) {
				auto caster = a->GetMagicCaster(src);
				if (!caster) continue;
				auto st = caster->state.get();
				if (st != S::kNone && st != S::kReady) return true;
			}
			return false;
		}

		void ReactToAttack(RE::Actor* f, RE::Actor* e, ReflexState& r)
		{
			// down on her knees or knocked flat: nothing she can do about it
			if (f->AsActorState()->IsBleedingOut() || f->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal) return;
			// A spell or a shout being cast is no swing to get out of: its hands are busy, so hit it.
			if (IsCasting(e)) {
				auto  weap = f->GetEquippedObject(false);
				auto  w = weap ? weap->As<RE::TESObjectWEAP>() : nullptr;
				float reach = w ? std::max(120.0f, w->GetReach() * 140.0f) : 150.0f;
				if (!IsRanged(f) && Dist(f, e) <= reach + 40.0f && f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone) {
					StrikeAt(f, e);
					Debug(std::format("{}: hits {} while it casts", Name(f), Name(e)));
				}
				r.cooldownUntil = g_time + 0.6f;
				return;
			}
			auto process = e->GetActorRuntimeData().currentProcess;
			auto data = process && process->high ? process->high->attackData.get() : nullptr;
			bool  power = data && data->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack);
			float mult = data ? std::max(1.0f, data->data.damageMult) : 1.0f;
			float hit = Loadout::HitDamage(e, f, e->GetEquippedEntryData(false)) * mult;
			// creatures (claws, bites) and fists: no weapon to work it out from, so their unarmed damage
			if (hit <= 0.0f) hit = std::max(1.0f, e->AsActorValueOwner()->GetActorValue(RE::ActorValue::kUnarmedDamage)) * mult;
			// (minus health a test command added: hits are judged against her real health)
			float health = std::max(1.0f, f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) - Lab::ExtraHealth(f));
			float share = health > 0.0f ? hit / health : 1.0f;
			// Bashing needs a block: a shield, a two-handed weapon, or one weapon with the other hand free.
			// Dual wielders can't block, so they can't bash. Enemies that shrug bashes off are remembered.
			auto  right = f->GetEquippedObject(false);
			auto  left = f->GetEquippedObject(true);
			auto  rightWeap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
			bool  canBlock = (left && left->IsArmor()) ||
			                (rightWeap && (rightWeap->IsTwoHandedSword() || rightWeap->IsTwoHandedAxe())) ||
			                (rightWeap && rightWeap->IsMelee() && !left);
			// (not mid-swing of her own: a bash can't start then)
			// (and stamina for it: a bash costs some, and an exhausted one just doesn't happen)
			bool  bashable = canBlock && g_bashFails[e->GetFormID()] < 2 && Dist(f, e) < 200.0f &&
			                f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone &&
			                f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) >= 40.0f;

			// A player's read of the moment: if she can bring it down with the hits she has time for before
			// its swing lands, dodging only wastes the opening. Keep hitting.
			if (!IsRanged(f) && rightWeap && rightWeap->IsMelee()) {
				float reach = std::max(120.0f, rightWeap->GetReach() * 140.0f);
				float window = power ? 0.9f : 0.5f;                                   // windup left, roughly
				float swing = 0.7f / std::max(0.5f, rightWeap->GetSpeed());          // seconds per swing
				bool  free = f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone;
				int   hits = static_cast<int>(window / swing) + (free ? 1 : 0);
				float mine = hits * Loadout::HitDamage(f, e, f->GetEquippedEntryData(false));
				float left = e->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
				if (hits > 0 && Dist(f, e) <= reach + 20.0f && mine >= left * Tuning::Get().finishMargin) {
					if (free) StrikeAt(f, e);
					r.action = 0;
					r.cooldownUntil = g_time + 0.5f;
					Debug(std::format("{}: finishes {} before its {} lands ({} hits, {:.0f} dmg vs {:.0f} hp)", Name(f), Name(e),
						power ? "power attack" : "swing", hits, mine, left));
					return;
				}
			}

			const char* what = nullptr;
			auto        stance = Experience::StanceVs(f, e);
			bool        worth = (power && (!stance.relaxed || share > 0.1f)) || share > stance.threshold;
			float       enemyReach = 250.0f;
			if (auto weap = e->GetEquippedObject(false); weap && weap->As<RE::TESObjectWEAP>()) {
				enemyReach = std::max(150.0f, weap->As<RE::TESObjectWEAP>()->GetReach() * 140.0f);
			}
			if (power && bashable) {
				// the shield goes up first; the bash follows a moment later (the graph refuses it before)
				f->NotifyAnimationGraph("blockStart"sv);
				r.bashPending = true;
				r.action = 3;
				r.until = g_time + 0.5f;
				r.bashTime = g_time;
				++g_stats.bashes;
				what = "bash";
			} else if (!power && HasShield(f) && !stance.relaxed) {
				// a shield on her arm: any ordinary swing she sees coming, she takes on it
				f->NotifyAnimationGraph("blockStart"sv);
				r.action = 2;
				r.until = g_time + 0.7f;
				++g_stats.blocks;
				what = "block";
			} else if (worth && g_time >= r.dodgeReadyAt && static_cast<float>(std::rand() % 100) < Tuning::Get().dodgeChance) {
				// a real dodge animation, left, right or back, as she has learned works best against these;
				// the slide-back (way 0) only when the animation can't play here
				int         way = Experience::PickWay(f, e);
				const char* dir = Driver::Dodge(f, e, way == 1 ? "left" : way == 2 ? "right" : "back");
				r.dodgeReadyAt = g_time + Tuning::Get().dodgeCooldown / (1.0f + stance.danger);  // a breather; less against what hurts
				++g_stats.stepOuts;
				if (dir) {
					int taken = dir[0] == 'l' ? 1 : dir[0] == 'r' ? 2 : 3;
					g_outcomes.push_back({ f->GetFormID(), e->GetHandle(), Experience::KindOf(e), taken, g_time });
					what = Experience::kWays[taken];
					r.action = 4;  // strike back once it's done (or keep backing off while it still swings)
					r.until = g_time + 0.7f;
				} else {
					Driver::Order order;
					order.kind = Driver::Kind::kStepBack;
					order.target = e->GetHandle();
					order.speed = Tuning::Get().stepSpeed;  // a player's diagonal backpedal (measured)
					order.weave = Tuning::Get().stepDiagonal;
					order.stopDistance = Dist(f, e) + enemyReach * 0.35f + 40.0f;  // just out of reach, no further
					order.duration = 1.8f;  // the drive ends when the swing is over
					order.holdWhileAttacked = true;
					Driver::Start(f, order);
					r.action = 1;
					r.until = g_time + 2.0f;
					g_outcomes.push_back({ f->GetFormID(), e->GetHandle(), Experience::KindOf(e), 0, g_time });
					what = "step out";
				}
			} else {
				++g_stats.taken;
				logger::info("{}: takes {} from {} ({:.0f} dmg, {:.0f}% hp)", Name(f), power ? "power attack" : "attack", Name(e), hit, share * 100.0f);
				r.cooldownUntil = g_time + 0.3f;
				return;
			}
			r.against = e->GetHandle();
			r.cooldownUntil = g_time + 1.0f;
			g_pendingCheck.push_back(g_time);
			Debug(std::format("{}: {} vs {} ({}{:.0f} dmg, {:.0f}% hp)", Name(f), what, Name(e), power ? "power, " : "", hit, share * 100.0f));
		}

		// Whether a heavy attack into a raised guard staggers that kind of enemy (vanilla: nearly always;
		// combat overhauls change it). She keeps doing it only while it works.
		struct GuardBreaks
		{
			int tries = 0;
			int staggers = 0;
		};
		std::map<std::pair<RE::FormID, std::uint32_t>, GuardBreaks> g_guardBreaks;

		void UpdateReflexes()
		{
			if (!Settings::Get().reflexes) return;  // dodges work with or without the movement switch
			for (auto it = g_pendingCheck.begin(); it != g_pendingCheck.end();) {
				if (g_time - *it < 1.2f) {
					++it;
					continue;
				}
				if (g_lastFollowerHurt < *it) ++g_stats.avoided;
				it = g_pendingCheck.erase(it);
			}
			// what came of each way out: hit by that enemy within the next second or so?
			for (auto it = g_outcomes.begin(); it != g_outcomes.end();) {
				if (g_time - it->time < 1.0f) {
					++it;
					continue;
				}
				auto& st = g_state[it->follower];
				bool  hit = st.lastMeleeHitTime > it->time && st.lastMeleeHitTime <= it->time + 1.0f && st.lastMeleeAttacker == it->enemy;
				Experience::Outcome(it->follower, it->kind, it->way, !hit);
				auto e = Get(it->enemy);
				logger::info("{} vs {}: {}", Experience::kWays[it->way], e ? Name(e) : "enemy", hit ? "hit" : "avoided");
				it = g_outcomes.erase(it);
			}
			for (auto& fh : g_lastFollowers) {
				auto f = Get(fh);
				if (!f || !Alive(f)) continue;
				auto& r = g_reflex[f->GetFormID()];
				// knocked down or getting up: nothing to do until she's on her feet (stepping in only
				// started drives that ended at once, over and over)
				if (f->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || f->AsActorState()->IsBleedingOut()) continue;

				if (r.action == 3 && r.bashPending && g_time - r.bashTime >= 0.08f) {
					r.bashPending = false;
					if (!f->NotifyAnimationGraph("bashStart"sv)) f->NotifyAnimationGraph("bashPowerStart"sv);
				}
				if (r.action == 3 && !r.bashPending && g_time - r.bashTime > 0.3f && g_time - r.bashTime < 0.45f &&
					f->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kBash) {
					// the bash did not happen (logged with her stamina, to see why): a dodge instead (the animation), or a short step out of reach
					auto e = Get(r.against);
					EndReflex(f, r);
					const char* side = Alive(e) ? Driver::Dodge(f, e, Experience::PickWay(f, e) == 2 ? "right" : "left") : nullptr;
					if (side) {
						r.action = 4;
						r.until = g_time + 0.7f;
						r.against = e->GetHandle();
						logger::info("{}: bash did not start (stamina {:.0f}), dodges {} instead", Name(f),
							f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina), side);
					} else if (Alive(e)) {
						float eReach = 250.0f;
						if (auto ew = e->GetEquippedObject(false); ew && ew->As<RE::TESObjectWEAP>()) {
							eReach = std::max(150.0f, ew->As<RE::TESObjectWEAP>()->GetReach() * 140.0f);
						}
						Driver::Order order;
						order.kind = Driver::Kind::kStepBack;
						order.target = e->GetHandle();
						order.speed = Tuning::Get().stepSpeed;
						order.stopDistance = std::max(Dist(f, e), eReach + 40.0f);  // just out of its reach
						order.duration = 1.8f;
						order.holdWhileAttacked = true;
						Driver::Start(f, order);
						r.action = 1;
						r.until = g_time + 2.0f;
						r.against = e->GetHandle();
						++g_stats.stepOuts;
						logger::info("{}: bash did not start, stepping out instead", Name(f));
					}
				}
				// a step back is over once the driver lets go (the swing ended): go back in and punish
				if (r.action == 1 && !Driver::IsDriving(f)) {
					EndReflex(f, r);
					r.punishAt = g_time;
					r.punishApproachStarted = false;
				}
				if (r.action == 4 && g_time >= r.until) {
					// still winding up or swinging (a bear holding its swipe): wait it out before striking
					auto e = Get(r.against);
					auto st = e ? e->AsActorState()->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
					bool waiting = Alive(e) && (st == RE::ATTACK_STATE_ENUM::kDraw || st == RE::ATTACK_STATE_ENUM::kSwing) &&
					               g_time - r.until < 2.5f;
					if (!waiting) {
						EndReflex(f, r);
						r.punishAt = g_time;
						r.punishApproachStarted = false;
					} else if (!Driver::IsDriving(f) && Dist(f, e) < 400.0f) {
						// the dodge didn't take her far enough (an enemy that tracks her): keep backing off
						// until its swing is over, then strike
						Driver::Order order;
						order.kind = Driver::Kind::kStepBack;
						order.target = e->GetHandle();
						order.speed = Tuning::Get().stepSpeed;
						order.weave = Tuning::Get().stepDiagonal;
						float eReach = 250.0f;
						if (auto ew = e->GetEquippedObject(false); ew && ew->As<RE::TESObjectWEAP>()) {
							eReach = std::max(150.0f, ew->As<RE::TESObjectWEAP>()->GetReach() * 140.0f);
						}
						order.stopDistance = std::max(Dist(f, e), eReach + 40.0f);  // just out of its reach
						order.duration = 1.2f;
						order.holdWhileAttacked = true;
						Driver::Start(f, order);
						r.action = 1;  // a step out from here on: it ends with the swing, then she punishes
						r.until = g_time + 2.0f;
					}
				}
				if (r.action != 0 && r.action != 4 && g_time >= r.until) {
					// a bash that did not stagger counts against that enemy
					if (r.action == 3) {
						auto e = Get(r.against);
						if (e && !IsStaggered(e)) ++g_bashFails[e->GetFormID()];
					}
					bool counter = r.action == 2 || r.action == 3;  // the swing was blocked or bashed: hit back now
					EndReflex(f, r);
					if (counter) {
						r.punishAt = g_time;
						r.punishApproachStarted = false;
					}
				}
				if (r.punishAt >= 0.0f && g_time >= r.punishAt) {
					// close back in to our own reach, then strike
					auto  e = Get(r.against);
					float reach = 150.0f;
					if (auto weap = f->GetEquippedObject(false); weap && weap->As<RE::TESObjectWEAP>()) {
						reach = std::max(120.0f, weap->As<RE::TESObjectWEAP>()->GetReach() * 140.0f);
					}
					if (!Alive(e) || IsRanged(f) || g_time - r.punishAt > 1.5f) {
						r.punishAt = -1.0f;
					} else if (Dist(f, e) <= reach + 40.0f) {
						Driver::Stop(f);
						StrikeAt(f, e);
						++g_stats.punishes;
						r.punishAt = -1.0f;
						Debug(std::format("{}: punish {}", Name(f), Name(e)));
					} else if (!Driver::IsDriving(f) && r.punishApproachStarted && std::string_view{ Driver::LastEnd(f) } != "arrived") {
						r.punishAt = -1.0f;  // the way in is blocked: let the combat AI take it from here
					} else if (!Driver::IsDriving(f)) {
						r.punishApproachStarted = true;
						Driver::Order order;
						order.kind = Driver::Kind::kRunTo;
						order.target = e->GetHandle();
						order.speed = Tuning::Get().closeSpeed;
						order.stopDistance = reach * 0.85f;
						order.duration = 1.2f;
						Driver::Start(f, order);
					}
				}
				if (!f->IsInCombat()) continue;

				// a reaction on its way (she saw the attack a moment ago): act now if it's still coming
				if (r.reactAt >= 0.0f && g_time >= r.reactAt) {
					auto e = Get(r.seen);
					r.reactAt = -1.0f;
					auto st = e ? e->AsActorState()->GetAttackState() : RE::ATTACK_STATE_ENUM::kNone;
					if (Alive(e) && (r.action == 0 || r.action == 4) &&
						(st == RE::ATTACK_STATE_ENUM::kDraw || st == RE::ATTACK_STATE_ENUM::kSwing)) {
						ReactToAttack(f, e, r);
					} else if (Alive(e)) {
						logger::info("{} reacts too late to {}'s attack", Name(f), Name(e));
					}
				}

				// Keep room to back off, as a player does: with a wall or a drop at her back and the enemy
				// between attacks, she circles round it until there's open ground behind her.
				if (r.action == 0 && r.reactAt < 0.0f && r.punishAt < 0.0f && !Driver::IsDriving(f) &&
					g_time >= r.nextRoomCheck) {
					r.nextRoomCheck = g_time + 0.5f;
					auto e = CombatTarget(f);
					float d = Alive(e) ? Dist(f, e) : 9999.0f;
					if (d < 350.0f && !IsRanged(f) && e->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone) {
						auto         ep = e->GetPosition();
						RE::NiPoint3 away = f->GetPosition() - ep;
						away.z = 0.0f;
						float len = away.Length();
						if (len > 1.0f) {
							away = away / len;
							float back = Driver::Room(f, f->GetPosition(), away, 240.0f);
							if (back < 120.0f) {
								int   sign = (std::rand() % 2) ? 1 : -1;
								float angles[] = { 35.0f * sign, -35.0f * sign };  // a step round, not a lap
								for (float deg : angles) {
									float        rad = deg * 0.0174533f, c = std::cos(rad), sn = std::sin(rad);
									RE::NiPoint3 dir{ away.x * c - away.y * sn, away.x * sn + away.y * c, 0.0f };
									RE::NiPoint3 spot = ep + dir * std::max(len, 150.0f);
									spot.z = f->GetPosition().z;
									RE::NiPoint3 way = spot - f->GetPosition();
									way.z = 0.0f;
									float wayLen = way.Length();
									if (wayLen < 40.0f || Driver::Room(f, f->GetPosition(), way / wayLen, wayLen) + 80.0f < wayLen) continue;
									if (Driver::Room(f, spot, dir, 240.0f) < 240.0f) continue;
									// a real side step (the dodge animation carries her, feet and all) toward that side
									RE::NiPoint3 toE = ep - f->GetPosition();
									toE.z = 0.0f;
									bool        right = (way.x * toE.y - way.y * toE.x) > 0.0f;  // spot on her right of the enemy line
									const char* side = Driver::Dodge(f, e, right ? "right" : "left");
									if (side && side[0] != 'b') {
										r.nextRoomCheck = g_time + 4.0f;
										r.action = 4;
										r.until = g_time + 0.7f;
										Debug(std::format("{}: wall behind ({:.0f} room), side-steps {} round {}", Name(f), back, side, Name(e)));
									}
									break;
								}
							}
						}
					}
				}

				// Openings a player wouldn't waste: the target casting (hands busy) or just standing there for a
				// moment between attacks (draugr do). Strike while it lasts.
				if (r.action == 0 && r.reactAt < 0.0f && r.punishAt < 0.0f && !Driver::IsDriving(f) && !IsRanged(f) &&
					g_time >= r.nextOpening) {
					r.nextOpening = g_time + 0.2f;
					auto e = CombatTarget(f);
					auto w = f->GetEquippedObject(false) ? f->GetEquippedObject(false)->As<RE::TESObjectWEAP>() : nullptr;
					float reach = w ? std::max(120.0f, w->GetReach() * 140.0f) : 150.0f;
					// (a step or two away counts too: she steps in and strikes, rather than hovering out of reach)
					if (Alive(e) && Dist(f, e) <= reach + 180.0f && f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone &&
						Reachable(f, e)) {
						bool casting = IsCasting(e);
						bool idle = e->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone && !e->IsBlocking() && !IsStaggered(e);
						if (idle) {
							if (r.idleSince < 0.0f) r.idleSince = g_time;
						} else {
							r.idleSince = -1.0f;
						}
						if (casting || (idle && g_time - r.idleSince > 0.4f)) {
							if (f->IsBlocking()) f->NotifyAnimationGraph("blockStop"sv);
							if (Dist(f, e) <= reach + 30.0f) {
								StrikeAt(f, e);
							} else {
								r.punishAt = g_time;  // close in, then strike (the punish approach)
								r.against = e->GetHandle();
								r.punishApproachStarted = false;
							}
							r.nextOpening = g_time + 0.8f;
							r.idleSince = -1.0f;
							logger::info("{}: opening, {} {} ({})", Name(f), Dist(f, e) <= reach + 30.0f ? "strikes" : "steps in on", Name(e),
								casting ? "it casts" : "it stands there");
						}
					}
				}

				// A bow or staff in hand: a player doesn't trade blows. Someone with a blade coming for her (or
				// right on her) gets left behind: a backpedal to shooting distance between shots.
				if (r.action == 0 && r.reactAt < 0.0f && !Driver::IsDriving(f) && IsRanged(f) && g_time >= r.nextKite &&
					f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone) {
					r.nextKite = g_time + 0.25f;
					RE::Actor* close = nullptr;
					float      closest = 260.0f;
					for (auto& h : g_lastEnemies) {
						auto e = Get(h);
						if (!Alive(e) || IsRanged(e) || IsStaggered(e)) continue;
						float d = Dist(f, e);
						if ((CombatTarget(e) == f || d < 150.0f) && d < closest) {
							closest = d;
							close = e;
						}
					}
					if (close) {
						Driver::Order order;
						order.kind = Driver::Kind::kStepBack;
						order.target = close->GetHandle();
						order.speed = Tuning::Get().stepSpeed;
						order.weave = Tuning::Get().stepDiagonal;
						order.stopDistance = 450.0f;
						order.duration = 1.5f;
						if (Driver::Start(f, order)) {
							r.nextKite = g_time + 1.5f;
							g_state[f->GetFormID()].pressure.push_back(g_time);
							logger::info("{}: backs off from {} ({:.0f} away) to shoot", Name(f), Name(close), closest);
						}
					}
				}

				// Her arrow hit scenery (a rock or rail between her and a kneeling enemy): a player moves for a
				// clear shot instead of loosing into the cover again.
				if (IsRanged(f) && !Driver::IsDriving(f) && g_time - g_state[f->GetFormID()].shotBlockedAt < 0.5f && g_time >= r.nextKite) {
					auto e = CombatTarget(f);
					bool hunted = false;  // (never walk into someone coming for her: kiting comes first)
					for (auto& h : g_lastEnemies) {
						auto o = Get(h);
						if (Alive(o) && o != e && CombatTarget(o) == f && Dist(f, o) < 500.0f) hunted = true;
					}
					if (Alive(e) && !hunted) {
						Driver::Order order;
						order.kind = Driver::Kind::kRunTo;
						order.target = e->GetHandle();
						order.speed = Tuning::Get().closeSpeed;
						order.stopDistance = std::max(250.0f, Dist(f, e) * 0.5f);
						order.duration = 2.0f;
						if (Driver::Start(f, order)) {
							g_state[f->GetFormID()].shotBlockedAt = -100.0f;
							r.nextKite = g_time + 2.0f;
							logger::info("{}: arrow hit cover, moves in for a clear shot at {}", Name(f), Name(e));
						}
					}
				}

				// Bow drawn but the target behind a pillar: a player lowers the bow and walks round at full
				// speed instead of creeping along drawn.
				{
					int  atk = static_cast<int>(f->AsActorState()->GetAttackState());
					auto e = CombatTarget(f);
					bool unused = false;
					if (IsRanged(f) && atk >= 8 && atk <= 10 && Alive(e) && !f->HasLineOfSight(e, unused)) {
						if (r.blindSince < 0.0f) r.blindSince = g_time;
						if (g_time - r.blindSince > 0.6f) {
							bool took = f->NotifyAnimationGraph("attackStop"sv);
							logger::info("{}: {} out of sight, lowers the bow ({})", Name(f), Name(e), took ? "taken" : "refused");
							r.blindSince = -1.0f;
						}
					} else {
						r.blindSince = -1.0f;
					}
				}

				// A raised guard: a player puts a heavy attack into it for a free stagger, as long as that works
				// in this game (a few tries, then only if it staggers at least a third of the time).
				if (r.action == 0 && r.reactAt < 0.0f && r.punishAt < 0.0f && r.guardCheckAt < 0.0f && !Driver::IsDriving(f) &&
					!IsRanged(f) && g_time >= r.nextGuardBreak) {
					r.nextGuardBreak = g_time + 0.2f;
					auto e = CombatTarget(f);
					if (Alive(e) && e->IsBlocking() && Dist(f, e) <= MeleeReach(f) + 30.0f &&
						f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone && Reachable(f, e)) {
						if (r.blockSince < 0.0f) r.blockSince = g_time;
						auto  kind = Experience::KindOf(e);
						auto& gb = g_guardBreaks[{ f->GetFormID(), kind }];
						bool  worth = gb.tries < 3 || gb.staggers * 3 >= gb.tries;
						float stamina = f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
						// (bosses raise the shield only for a moment, often to bash: a player answers at once)
						if (worth && g_time - r.blockSince > 0.15f && stamina >= 30.0f) {
							if (f->IsBlocking()) f->NotifyAnimationGraph("blockStop"sv);
							RE::NiPoint3 to = e->GetPosition() - f->GetPosition();
							if (to.x * to.x + to.y * to.y > 1.0f) f->SetRotationZ(std::atan2(to.x, to.y));
							bool took = f->NotifyAnimationGraph("attackPowerStartInPlace"sv) || f->NotifyAnimationGraph("attackPowerStartForward"sv);
							if (took) {
								r.guardCheckAt = g_time + 1.8f;
								r.guardTarget = e->GetHandle();
								r.guardKind = kind;
								r.nextGuardBreak = g_time + 3.0f;
								r.blockSince = -1.0f;
								logger::info("{}: heavy attack into {}'s guard ({} of {} staggered so far)", Name(f), Name(e), gb.staggers, gb.tries);
							}
						}
					} else {
						r.blockSince = -1.0f;
					}
				}
				if (r.guardCheckAt >= 0.0f) {
					auto e = Get(r.guardTarget);
					bool staggered = Alive(e) && IsStaggered(e);
					if (staggered || g_time > r.guardCheckAt || !Alive(e)) {
						auto& gb = g_guardBreaks[{ f->GetFormID(), r.guardKind }];
						++gb.tries;
						if (staggered) ++gb.staggers;
						logger::info("{}: guard break vs {}: {}", Name(f), e ? Name(e) : "enemy", staggered ? "staggered" : "no stagger");
						r.guardCheckAt = -1.0f;
					}
				}

				// A target some way off and she isn't getting closer (the game's circling spirals in slowly):
				// run at it, as a player would.
				if (r.action == 0 && r.punishAt < 0.0f && !Driver::IsDriving(f) && !IsRanged(f) && g_time >= r.approachCheck &&
				    !f->AsActorState()->IsBleedingOut()) {
					r.approachCheck = g_time + 1.0f;
					auto  target = CombatTarget(f);
					float d = Alive(target) ? Dist(f, target) : -1.0f;
					float reach = MeleeReach(f);
					if (d > reach + 180.0f && d < 2000.0f && r.approachDist > 0.0f && r.approachDist - d < 80.0f && Reachable(f, target)) {
						Driver::Order order;
						order.kind = Driver::Kind::kRunTo;
						order.target = target->GetHandle();
						order.speed = d > 600.0f ? 600.0f : Tuning::Get().closeSpeed;
						order.sprint = d > 600.0f;
						order.stopDistance = reach * 0.9f;
						order.duration = 3.0f;
						if (Driver::Start(f, order)) logger::info("{} runs at {} ({:.0f} away)", Name(f), Name(target), d);
						r.approachDist = -1.0f;
					} else {
						r.approachDist = d;
					}
				}

				// Outnumbered, as a player plays it: don't let them get behind you (step so they're all in
				// front), and don't fight a crowd alone far from your partner (fall back to the player's side).
				if (r.action == 0 && r.reactAt < 0.0f && !Driver::IsDriving(f) && !IsRanged(f) && g_time >= r.nextFlankCheck) {
					r.nextFlankCheck = g_time + 0.5f;
					auto target = CombatTarget(f);
					if (Alive(target)) {
						auto         fp = f->GetPosition();
						RE::NiPoint3 toT = target->GetPosition() - fp;
						toT.z = 0.0f;
						float tl = toT.Length();
						RE::Actor* flanker = nullptr;
						int        close = 0;
						for (auto& h : g_lastEnemies) {
							auto o = Get(h);
							if (!Alive(o) || Dist(f, o) > 350.0f) continue;
							++close;
							if (o == target || tl < 1.0f) continue;
							RE::NiPoint3 toO = o->GetPosition() - fp;
							toO.z = 0.0f;
							float ol = toO.Length();
							if (ol > 1.0f && (toO.x * toT.x + toO.y * toT.y) / (ol * tl) < -0.35f) flanker = o;  // >110 degrees off
						}
						if (flanker && f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone) {
							// step to the side away from the one behind, so both end up in front of her
							RE::NiPoint3 toO = flanker->GetPosition() - fp;
							bool         flankerLeft = (toT.x * toO.y - toT.y * toO.x) > 0.0f;
							const char*  side = Driver::Dodge(f, target, flankerLeft ? "right" : "left");
							if (side && side[0] != 'b') {
								r.action = 4;
								r.until = g_time + 0.7f;
								r.against = target->GetHandle();
								r.nextFlankCheck = g_time + 3.0f;
								logger::info("{}: {} getting behind her, side-steps {}", Name(f), Name(flanker), side);
							}
						} else if (close >= 3 && g_time >= r.nextRegroup) {
							auto player = RE::PlayerCharacter::GetSingleton();
							float pd = Dist(f, player);
							if (pd > 450.0f && pd < 1500.0f && Reachable(f, player)) {
								Driver::Order order;
								order.kind = Driver::Kind::kRunTo;
								order.target = player->GetHandle();
								order.speed = Tuning::Get().closeSpeed;
								order.stopDistance = 250.0f;
								order.duration = 2.0f;
								if (Driver::Start(f, order)) {
									r.nextRegroup = g_time + 8.0f;
									logger::info("{}: {} enemies on her, falls back to your side", Name(f), close);
								}
							}
						}
					}
				}

				for (auto& eh : g_lastEnemies) {
					auto e = Get(eh);
					if (!Alive(e)) continue;
					auto id = e->GetFormID();
					auto state = e->AsActorState()->GetAttackState();
					auto prev = g_enemyAttack.contains(id) ? g_enemyAttack[id] : RE::ATTACK_STATE_ENUM::kNone;
					g_enemyAttack[id] = state;
					bool staggered = IsStaggered(e);
					bool wasStaggered = g_enemyStagger[id];
					g_enemyStagger[id] = staggered;
					float d = Dist(f, e);
					if (d > 450.0f) continue;

					// opening: the enemy was just staggered next to us -> hit it now
					if (staggered && !wasStaggered && d < 300.0f && r.action == 0 && !IsRanged(f) &&
						f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone) {
						StrikeAt(f, e);
						++g_stats.punishes;
						Debug(std::format("{}: punish staggered {}", Name(f), Name(e)));
						continue;
					}

					// a new attack: winding up or swinging now, and not a moment ago (chained attacks skip kNone)
					auto windup = [](RE::ATTACK_STATE_ENUM a) {
						return a == RE::ATTACK_STATE_ENUM::kDraw || a == RE::ATTACK_STATE_ENUM::kSwing;
					};
					if (state == RE::ATTACK_STATE_ENUM::kDraw && prev != RE::ATTACK_STATE_ENUM::kDraw) g_drawStart[id] = g_time;
					// a new attack: and, after a long wind-up (bears, trolls hold it for seconds), the swing
					// itself, which lands a split second later
					bool longDraw = prev == RE::ATTACK_STATE_ENUM::kDraw && state == RE::ATTACK_STATE_ENUM::kSwing &&
					                g_time - g_drawStart[id] > 0.5f;
					bool starting = (windup(state) && !windup(prev)) || longDraw;
					if (starting) ++g_stats.enemyAttacks;
					// the attack is meant for us if we are its target, or we are in front of it and within reach
					bool aimedAtUs = CombatTarget(e) == f;
					if (!aimedAtUs && d < 350.0f) {
						RE::NiPoint3 toF = f->GetPosition() - e->GetPosition();
						float len = std::sqrt(toF.x * toF.x + toF.y * toF.y);
						auto  fwd = Forward(e);
						aimedAtUs = len > 1.0f && (toF.x * fwd.x + toF.y * fwd.y) / len > 0.5f;
					}
					if (starting && aimedAtUs && ((r.action == 0 && g_time >= r.cooldownUntil) || (longDraw && r.action == 4))) {
						// Does she see it coming, and how quickly? That depends on what she has learned about
						// this kind of enemy (a new one fools her often; one she knows well, rarely).
						auto read = Experience::Read(f, e);
						Experience::SawAttack(f, e);
						if (static_cast<float>(std::rand() % 1000) >= read.chance * 1000.0f) {
							logger::info("{} doesn't see {}'s attack coming ({:.0f}% read)", Name(f), Name(e), read.chance * 100.0f);
							r.cooldownUntil = g_time + 0.3f;
						} else if (read.delay < 0.02f) {
							ReactToAttack(f, e, r);
						} else {
							r.seen = e->GetHandle();
							r.reactAt = g_time + read.delay;
						}
					}
				}
			}
		}

		// ------------------------------------------------------------------ charging shooters
		constexpr float kChargeStop = 350.0f;    // hand back to the combat AI this close
		constexpr float kChargeStart = 700.0f;   // only worth it from this far
		constexpr float kSprintReach = 1100.0f;  // about two seconds of sprinting
		std::unordered_map<RE::FormID, float> g_nextCharge;

		// A melee fighter charges a shooter: the driver runs it at sprint speed on a weaving path,
		// facing the shooter, then the combat AI takes over at sword range.
		bool MaybeCharge(RE::Actor* f, FollowerState& s, RE::Actor* a_target)
		{
			auto& cfg = Settings::Get();
			if (!cfg.zigzag || !cfg.combatMovement || !Alive(a_target) || !IsRanged(a_target) || g_retreating) return false;
			if (g_reflex[f->GetFormID()].action != 0) return false;
			if (std::string_view{ Driver::LastEnd(f) } == "blocked" && s.chargeTarget && !s.chargeBlockNoted) {
				g_nextCharge[f->GetFormID()] = g_time + 6.0f;  // the straight line didn't work: give pathfinding time
				s.chargeBlockNoted = true;
			}
			if (g_time < g_nextCharge[f->GetFormID()]) return false;
			s.chargeBlockNoted = false;
			float bias = cfg.combatDistance == 0 ? 1.25f : (cfg.combatDistance == 2 ? 0.8f : 1.0f);
			if (GetLoadout(f, s).PrefersRanged(bias)) return false;  // archers and mages don't charge
			float d = Dist(f, a_target);
			// Far: charge right away. Close/Medium: trade shots first, charge from about two seconds away.
			float startAt = cfg.combatDistance == 2 ? 4000.0f : kSprintReach;
			if (d < kChargeStart || d > startAt) return false;
			// only a straight run on level ground: a shooter on a wall or up a slope is the pathfinder's job
			if (std::fabs(a_target->GetPosition().z - f->GetPosition().z) > 120.0f) return false;
			if (!Driver::ClearLine(f, f->GetPosition(), a_target->GetPosition())) return false;
			if (g_time < s.unreachableUntil) return false;
			SetWeaponPref(f, 1);
			Swap(f, s, false, "charging a shooter");
			Driver::Order order;
			order.kind = Driver::Kind::kCharge;
			order.target = a_target->GetHandle();
			order.speed = Tuning::Get().chargeSpeed;  // a player's run (measured); sprint waits for a working sprint animation
			order.weave = 0.0f;  // zig-zag shelved until the run looks right
			order.flipMin = Tuning::Get().flipMin;
			order.flipMax = Tuning::Get().flipMax;
			order.stopDistance = kChargeStop;
			order.duration = 10.0f;
			order.sprint = true;
			if (!Driver::Start(f, order)) return false;
			g_nextCharge[f->GetFormID()] = g_time + 1.5f;
			s.chargeTarget = a_target->GetHandle();
			++g_stats.zigzags;
			Debug(std::format("{} charges {}", Name(f), Name(a_target)));
			return true;
		}

		// Damage per second: measured in this session once there is enough data, otherwise estimated.
		float Dps(const FollowerState& s, int a_kind, float a_estimate)
		{
			if (s.hits[a_kind] >= 5 && s.heldTime[a_kind] >= 10.0f) {
				float measured = s.dealt[a_kind] / s.heldTime[a_kind];
				return a_estimate > 0.0f ? measured * 0.75f + a_estimate * 0.25f : measured;
			}
			return a_estimate;
		}

		float NearestEnemyDist(RE::Actor* f)
		{
			float best = std::numeric_limits<float>::max();
			for (auto& h : g_lastEnemies) {
				auto e = Get(h);
				// an enemy up on a wall or down a drop can't hit her in melee: it doesn't count as close
				if (Alive(e) && std::fabs(e->GetPosition().z - f->GetPosition().z) < 150.0f) best = std::min(best, Dist(f, e));
			}
			return best;
		}

		void UpdateWeapons(RE::Actor* f, FollowerState& s, RE::Actor* a_target, bool a_retreating)
		{
			auto  options = GetLoadout(f, s);
			// a different weapon of a kind: what she did with the old one says nothing about it
			RE::FormID now[2] = { options.melee ? options.melee->GetFormID() : 0, options.bow ? options.bow->GetFormID() : 0 };
			for (int k = 0; k < 2; ++k) {
				if (now[k] != s.measuredWith[k]) {
					s.measuredWith[k] = now[k];
					s.dealt[k] = 0.0f;
					s.heldTime[k] = 0.0f;
					s.hits[k] = 0;
				}
			}
			options.meleeScore = Dps(s, 0, options.meleeScore);
			options.rangedScore = Dps(s, 1, options.rangedScore);
			if (!options.HasRanged() || !options.HasMelee() || !a_target) return;

			float d = Dist(f, a_target);
			if (g_time - s.targetDistTime >= 1.0f) {
				s.closing = s.prevTargetDist > 0.0f && s.prevTargetDist - d > 100.0f;
				s.prevTargetDist = d;
				s.targetDistTime = g_time;
			}
			bool ranged = IsRanged(f);

			if (a_retreating) {
				SetWeaponPref(f, 2);
				if (!ranged) Swap(f, s, true, "covering the retreat");
				return;
			}
			auto& cfg = Settings::Get();
			float bias = cfg.combatDistance == 0 ? 1.25f : (cfg.combatDistance == 2 ? 0.8f : 1.0f);
			if (options.PrefersRanged(bias)) {
				// archer/mage: shoot from afar while the enemies go for the player. When they go for her and
				// backing off doesn't shake them (hits keep landing, backpedal after backpedal), a player
				// pulls the melee weapon, if it isn't much worse; back to the bow once they leave her be.
				while (!s.pressure.empty() && g_time - s.pressure.front() > 10.0f) s.pressure.pop_front();
				bool onHer = false;
				for (auto& h : g_lastEnemies) {
					auto e = Get(h);
					if (Alive(e) && CombatTarget(e) == f && Dist(f, e) < 600.0f) onHer = true;
				}
				// (kept up for a while, not just the opening seconds of a fight when everyone runs at her)
				bool pressured = s.pressure.size() >= 4 && s.pressure.back() - s.pressure.front() >= 5.0f && NearestEnemyDist(f) < 400.0f;
				bool meleeWorth = options.HasMelee() && options.meleeScore >= options.rangedScore * 0.6f;
				if (pressured && meleeWorth) {
					s.meleeUntil = g_time + 6.0f;
					SetWeaponPref(f, 1);
					if (ranged) Swap(f, s, false, std::format("they keep catching her ({} in 10 s)", s.pressure.size()));
				} else if (!ranged && !onHer && NearestEnemyDist(f) > 300.0f) {
					// nobody after her any more (the one on her is dead): back to the bow at once
					s.meleeUntil = 0.0f;
					SetWeaponPref(f, 2);
					Swap(f, s, true, "nobody on her, back to the bow");
				} else if (!ranged && onHer && g_time < s.meleeUntil + 6.0f) {
					s.meleeUntil = std::max(s.meleeUntil, g_time + 1.0f);  // still on her: keep fighting
				} else if (g_time >= s.meleeUntil) {
					SetWeaponPref(f, 2);
					if (!ranged) Swap(f, s, true, "back to range");
				}
			} else {
				// fighter: charge. Shoot only at what they can't reach, or what is too far from the player to chase.
				auto  player = RE::PlayerCharacter::GetSingleton();
				float leash = kTuning[std::clamp(cfg.combatDistance, 0, 2)].combatLeash;
				bool  cantChase = ((cfg.combatLeash && cfg.combatMovement) && Dist(a_target, player) > leash) || g_time < s.unreachableUntil;
				// Close/Medium: trade a few shots with a far shooter before charging
				if (cfg.combatDistance != 2 && IsRanged(a_target) && d > kSprintReach) cantChase = true;
				// never pull a bow with an enemy this close, whatever else is going on
				if (!cantChase || d < 600.0f || NearestEnemyDist(f) < 600.0f) {
					SetWeaponPref(f, 1);
					if (ranged) Swap(f, s, false, "charging");
				} else {
					SetWeaponPref(f, 2);
					if (!ranged) Swap(f, s, true, "can't reach, shooting");
				}
			}
		}

		// ------------------------------------------------------------------ cutting enemies down
		// Roughly how long she needs to kill e from where she stands: walking up plus her swings.
		float KillTime(RE::Actor* f, RE::Actor* e)
		{
			auto  w = f->GetEquippedObject(false) ? f->GetEquippedObject(false)->As<RE::TESObjectWEAP>() : nullptr;
			float hit = Loadout::HitDamage(f, e, f->GetEquippedEntryData(false));
			if (hit <= 1.0f) return 99.0f;
			float hp = e->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			float hits = std::ceil(std::max(1.0f, hp) / hit);
			float swing = 0.7f / std::max(0.5f, w ? w->GetSpeed() : 1.0f);
			float walk = std::max(0.0f, Dist(f, e) - MeleeReach(f)) / 400.0f;
			return hits * swing + walk;
		}

		// Against enemies she knows to be harmless, a player stops fencing and mows them down: finish
		// whoever is on their knees first, then always the quickest kill in reach, no pause between them,
		// and walk right up to a lone one so every swing lands. Returns true when it moved her this tick.
		bool Meatgrind(RE::Actor* f, FollowerState& s, const Scan& a_scan, RE::Actor* a_target)
		{
			if (g_time < s.nextGrind || IsRanged(f) || Driver::IsDriving(f)) return false;
			s.nextGrind = g_time + 0.25f;
			float reach = MeleeReach(f);
			bool  free = f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone && !f->IsBlocking();

			// someone on their knees nearby: finish them before they get up
			RE::Actor* downed = nullptr;
			for (auto e : a_scan.enemies) {
				if (Alive(e) && e->AsActorState()->IsBleedingOut() && Dist(f, e) < 500.0f && Reachable(f, e) &&
					(!downed || Dist(f, e) < Dist(f, downed))) {
					downed = e;
				}
			}
			if (downed) {
				if (CombatTarget(f) != downed && g_time - s.lastRetarget > 0.5f) StartCombat(f, s, downed, "finishing a downed enemy");
				if (Dist(f, downed) <= reach + 30.0f) {
					if (free) StrikeAt(f, downed);
				} else {
					Driver::Order order;
					order.kind = Driver::Kind::kRunTo;
					order.target = downed->GetHandle();
					order.speed = Tuning::Get().closeSpeed;
					order.stopDistance = reach * 0.8f;
					order.duration = 1.5f;
					if (Driver::Start(f, order)) logger::info("{} goes to finish {}", Name(f), Name(downed));
				}
				return true;
			}

			if (!Alive(a_target) || !Experience::StanceVs(f, a_target).relaxed) return false;

			// the quickest kill among the harmless ones around her
			RE::Actor* best = a_target;
			float      bestTime = KillTime(f, a_target);
			int        others = 0;
			for (auto e : a_scan.enemies) {
				if (!Alive(e) || e == a_target) continue;
				float d = Dist(f, e);
				if (d < 1000.0f) ++others;
				if (d > 700.0f || Futile(f, e) || !Experience::StanceVs(f, e).relaxed || !Reachable(f, e)) continue;
				float t = KillTime(f, e);
				if (t + 0.5f < bestTime) {
					best = e;
					bestTime = t;
				}
			}
			if (best != a_target && g_time - s.lastRetarget > 0.8f) {
				StartCombat(f, s, best, std::format("quickest kill, {:.1f}s", bestTime));
				return false;
			}

			// a lone harmless enemy: right up close, so nothing whiffs
			if (others == 0 && free && Dist(f, a_target) > reach + 20.0f && Dist(f, a_target) < 600.0f &&
				a_target->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone && Reachable(f, a_target)) {
				Driver::Order order;
				order.kind = Driver::Kind::kRunTo;
				order.target = a_target->GetHandle();
				order.speed = Tuning::Get().closeSpeed;
				order.stopDistance = reach * 0.75f;
				order.duration = 1.2f;
				if (Driver::Start(f, order)) {
					logger::info("{} closes in on {}", Name(f), Name(a_target));
					return true;
				}
			}
			return false;
		}

		void UpdateFollower(RE::Actor* f, int a_slot, const Scan& a_scan)
		{
			auto& cfg = Settings::Get();
			auto  player = a_scan.player;
			auto& s = g_state[f->GetFormID()];
			Tactics::Apply(f, IsRanged(f));
			constexpr int mode = 0;  // close following
			int   combatMode = std::clamp(cfg.combatDistance + (g_time < s.healedUntil ? 1 : 0), 0, 2);
			auto& tune = kTuning[mode];
			float leash = kTuning[combatMode].combatLeash;
			{
				// healthy (over half health) or being healed: she keeps to her fight, wherever it goes
				auto  avo = f->AsActorValueOwner();
				float maxHealth = avo->GetPermanentActorValue(RE::ActorValue::kHealth);
				bool  healthy = maxHealth > 0.0f && avo->GetActorValue(RE::ActorValue::kHealth) > maxHealth * 0.5f;
				if (healthy || g_time < s.healedUntil) leash = std::max(leash, 6000.0f);
			}
			float dist = Dist(f, player);
			bool  engaging = g_time < s.engagingUntil;
			bool  inCombat = f->IsInCombat() || engaging;
			bool  waiting = IsWaiting(f);
			auto  state = f->AsActorState();
			bool  busy = waiting || InScene(f) || f->IsOnMount() || player->IsOnMount() ||
			            state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal || state->IsBleedingOut() || g_time < s.hazardUntil;
			if ((state->IsBleedingOut() || g_time < s.hazardUntil) && Driver::IsDriving(f)) Driver::Stop(f);

			// Legs running, body going nowhere, under the game's own AI: a body in the way, most likely.
			// Shove it aside, as a player's walk does.
			{
				auto  pos = f->GetPosition();
				float moved = pos.GetDistance(s.bodyCheckPos);
				s.bodyCheckPos = pos;
				float animSpeed = 0.0f;
				f->GetGraphVariableFloat("Speed", animSpeed);
				if (!Driver::IsDriving(f) && !busy && animSpeed > 80.0f && moved < 5.0f) {
					if (s.slowSince < 0.0f) s.slowSince = g_time;
					if (g_time - s.slowSince > 0.4f) {
						if (auto body = Bodies::InTheWay(f, Forward(f), 150.0f)) Bodies::Kick(f, body);
					}
				} else {
					s.slowSince = -1.0f;
				}
			}
			bool  sameSpace = f->GetWorldspace() == player->GetWorldspace() &&
			                 (player->GetWorldspace() || f->GetParentCell() == player->GetParentCell());
			// tagging along after an unreachable enemy ends with the fight, a retreat or a new target
			if (s.tagAlong && (!inCombat || busy || g_retreating || g_time < s.recallUntil || !Alive(CombatTarget(f)) ||
								  Get(s.tagTarget) != CombatTarget(f))) {
				s.tagAlong = false;
				if (Driver::IsDriving(f)) Driver::Stop(f);
				Debug(std::format("{} stops tagging along", Name(f)));
			}

			{
				// undo the old "relax" wait from earlier versions
				if (s.avMods.contains(RE::ActorValue::kWaitingForPlayer)) {
					SetMod(f, s, RE::ActorValue::kWaitingForPlayer, 0.0f);
					f->EvaluatePackage(true, false);
				}
				// formation looseness: weapon drawn = tight; sheathed = more relaxed
				// (indoors the rooms are too cramped for it: a few steps further is round a corner, out of sight)
				auto here = player->GetParentCell();
				bool inside = here && here->IsInteriorCell();
				s.looseness = player->AsActorState()->IsWeaponDrawn() || inside ? 1.0f : 1.4f;
				if (s.formationDrive) Driver::SetLooseness(f, s.looseness);

				// a follower left in another area (a door the player went through) is brought over
				if (!sameSpace && !waiting && !InScene(f) && !player->IsInCombat()) {
					if (s.otherSpaceSince < 0.0f) s.otherSpaceSince = g_time;
					if (g_time - s.otherSpaceSince > 4.0f) {
						auto fwd = Forward(player);
						Papyrus::MoveTo(f, player, -fwd.x * 150.0f, -fwd.y * 150.0f);
						s.otherSpaceSince = -1.0f;
						Debug(std::format("{} caught up (other area)", Name(f)));
					}
				} else {
					s.otherSpaceSince = -1.0f;
				}
			}

			// ---- retreat / recall: head back to the player
			bool recalling = !waiting && !busy && (g_retreating || g_time < s.recallUntil);
			bool coverFire = false;
			if (recalling) {
				auto& options = GetLoadout(f, s);
				coverFire = g_retreating && options.HasRanged() && dist < cfg.rangedRetreatDistance;
				if (coverFire) {
					// keep shooting while backing off with the player
					if (cfg.smartWeapons) UpdateWeapons(f, s, CombatTarget(f), true);
					SetFleeing(f, s, false);
				} else {
					if (inCombat && g_time - s.lastStop > 0.3f) Disengage(f, s);
					SetFleeing(f, s, false);
					// turn and run: the driver runs the body to the player, facing the way it goes
					if (!Driver::IsDriving(f) && dist > 300.0f && sameSpace) {
						Driver::Order order;
						order.kind = Driver::Kind::kRunTo;
						order.target = player->GetHandle();
						order.speed = 500.0f;
						order.stopDistance = 250.0f;
						order.duration = 4.0f;
						order.sprint = true;
						Driver::Start(f, order);
					}
				}
			} else {
				SetFleeing(f, s, false);
			}

			// the driver moves this follower right now (a charge, a dodge): leave it alone
			// in the player's footsteps and already next to them: the formation takes over
			if (std::string_view{ Driver::Current(f) } == "path" && g_time - s.footstepsAt < 30.0f && dist < 200.0f && !inCombat) {
				Driver::Stop(f);
			}
			if (Driver::IsDriving(f) && !s.formationDrive && !s.tagAlong) return;

			// ---- keep-offset follow
			// Close/Medium: the movement driver holds the follower's spot next to the player (one smooth run
			// at the player's pace, sprinting when the player sprints, facing the way the group moves).
			// With a wall, fence or rock between her and the spot, the game's own follow (pathfinding,
			// traversal jumps) takes over for a moment. Retreats still use the keep-offset.
			bool wantOffset = !busy && sameSpace && recalling;
			if (wantOffset) {
				ApplyOffset(f, s, player, 0, a_slot);
			} else {
				ClearOffset(f, s);
			}
			bool formation = cfg.outsideMovement && !busy && sameSpace && !inCombat && !recalling && mode != 2 && !waiting && dist < 1500.0f;
			// left to the game's pathfinding after a block, but falling behind: her footsteps instead
			if (formation && !Driver::IsDriving(f) && g_time < s.pathUntil && dist > 350.0f && s.footstepFails < 2 &&
				RunTrail(f, s, player, nullptr, "footsteps", 0.5f)) {
				s.pathUntil = 0.0f;
			}
			if (formation && !Driver::IsDriving(f) && g_time >= s.pathUntil) {
				std::string_view lastEnd = Driver::LastEnd(f);
				if (lastEnd == "blocked" || lastEnd == "edge" || lastEnd == "step" || lastEnd == "too far") {
					if (!s.pathNoted && lastEnd == "blocked" && TryHop(f, s, player)) {
						s.pathNoted = true;
					}
					// walk in the player's footsteps: their route is known to be walkable
					if (!s.pathNoted && s.footstepFails < 2 && RunTrail(f, s, player, nullptr, "footsteps", 0.5f)) {
						s.pathNoted = true;
					}
					if (!s.pathNoted) {
						// let the game's pathfinding (and traversal mods) get round it, longer each time
						s.blockedCount = std::min(s.blockedCount + 1, 4);
						s.pathUntil = g_time + std::min(10.0f, 2.0f * static_cast<float>(1 << (s.blockedCount - 1)));
						s.pathNoted = true;
					}
				} else if (!s.pathNoted) {
					s.blockedCount = 0;
				}
				auto          cell = player->GetParentCell();
				Driver::Order order = FormationOrder(cell && cell->IsInteriorCell());
				order.target = player->GetHandle();
				RE::NiPoint3 nearest;
				float        gap = Driver::ZoneGap(order, f, player, &nearest);
				// a climb or a drop (the player up a rock, down a ledge) is the game's and traversal mods' job
				float height = std::fabs(player->GetPosition().z - f->GetPosition().z);
				bool  clear = g_time >= s.pathUntil && height < 80.0f && (gap > 40.0f || player->IsMoving()) &&
				             ClearPath(f, f->GetPosition(), nearest);
				// something in the way of the straight line: take the player's footsteps instead
				if (!clear && g_time >= s.pathUntil && gap > 40.0f && !Driver::IsDriving(f) && s.footstepFails < 2) {
					RunTrail(f, s, player, nullptr, "footsteps", 0.5f);
				}
				if (clear && !Driver::IsDriving(f)) {
					order.speed = Tuning::Get().chargeSpeed;  // a run
					order.sprintSpeed = 715.0f;               // a player's sprint (measured)
					order.looseness = s.looseness;
					order.duration = 3600.0f;
					if (Driver::Start(f, order)) s.pathNoted = false;
					s.driveStarted = g_time;
				}
			} else if (!formation && Driver::IsDriving(f) && s.formationDrive) {
				Driver::Stop(f);
			}
			// Stuck while following (not driven, the player away, barely moving): try a hop.
			if (formation && !Driver::IsDriving(f) && dist > 250.0f) {
				if (g_time - s.followSampleTime >= (s.looseness <= 1.01f ? 0.25f : 0.5f)) {
					float moved = f->GetPosition().GetDistance(s.followSample);
					s.followSample = f->GetPosition();
					s.followSampleTime = g_time;
					if (moved < (s.looseness <= 1.01f ? 15.0f : 30.0f)) {
						if (s.stallSince < 0.0f) s.stallSince = g_time;
					} else {
						s.stallSince = -1.0f;
						s.hopFails = 0;
					}
				}
				float stallLimit = s.looseness <= 1.01f ? 0.5f : 1.0f;  // weapons drawn: decide quicker
				if (s.stallSince >= 0.0f && g_time - s.stallSince >= stallLimit && !TryHop(f, s, player) &&
					g_time - s.stallSince >= stallLimit + 1.5f) {
					RunTrail(f, s, player, nullptr, "to you");
				}
			} else if (!Driver::IsDriving(f)) {
				s.stallSince = -1.0f;
			}
			if (s.formationDrive && Driver::IsDriving(f) && g_time - s.driveStarted > 3.0f) {
				s.blockedCount = 0;
				s.footstepFails = 0;
			}
			if (!Driver::IsDriving(f) && g_time >= s.pathUntil && s.footstepFails >= 2 && g_time - s.footstepsAt > 6.0f) s.footstepFails = 0;
			s.formationDrive = formation && Driver::IsDriving(f);

			// ---- catch-up speed and teleport
			bool travelling = (!inCombat || recalling) && !busy && sameSpace;
			float boostAt = recalling ? 300.0f : tune.boostDistance;
			// Close/Medium: a follower lagging behind her spot while the player moves gets faster in steps,
			// so she keeps up with a player who runs faster than she does.
			float lagBoost = 0.0f;
			if (travelling && !inCombat && mode != 2 && s.offsetMode >= 0 && player->IsMoving()) {
				float lag = f->GetPosition().GetDistance(SpotInWorld(player, OffsetSpot(mode, a_slot)));
				if (lag > 90.0f) lagBoost = std::min(60.0f, std::floor((lag - 90.0f) / 40.0f + 1.0f) * 15.0f);
			}
			if (cfg.catchUp && cfg.outsideMovement && travelling && dist > boostAt) {
				SetBoost(f, s, kSpeedBoost);
			} else if (cfg.catchUp && cfg.outsideMovement && lagBoost > 0.0f) {
				SetBoost(f, s, lagBoost);
			} else if (s.speedBoost != 0.0f && (!travelling || (dist < boostAt * 0.6f && lagBoost == 0.0f))) {
				SetBoost(f, s, 0.0f);
			}

			// ---- sprint when the player sprints (out of combat, while moving with the player)
			bool wantSprint = travelling && !inCombat && player->AsActorState()->IsSprinting() && f->IsMoving() && !Driver::IsDriving(f);
			auto fstate = f->AsActorState();
			if (wantSprint) {
				fstate->actorState1.sprinting = 1;
				bool still = false;
				f->GetGraphVariableBool("IsSprinting", still);
				if (!still && g_time - s.lastSprintKick > 0.4f) {
					f->SetGraphVariableBool("IsSprinting", true);
					f->NotifyAnimationGraph("SprintStart"sv);
					s.lastSprintKick = g_time;
				}
				s.mirroringSprint = true;
			} else if (s.mirroringSprint) {
				fstate->actorState1.sprinting = 0;
				f->SetGraphVariableBool("IsSprinting", false);
				f->NotifyAnimationGraph("SprintStop"sv);
				s.mirroringSprint = false;
			}

			// weapon stuck half drawn (seen in a city after a fight: combat pose, sheathed, deaf to the player
			// drawing): back to sheathed and the default pose, and the game draws again when it wants to
			{
				using W = RE::WEAPON_STATE;
				auto ws = fstate->GetWeaponState();
				bool between = ws == W::kWantToDraw || ws == W::kDrawing || ws == W::kWantToSheathe || ws == W::kSheathing;
				if (!between || Driver::IsDriving(f)) {
					s.weaponStuckSince = -1.0f;
				} else if (s.weaponStuckSince < 0.0f) {
					s.weaponStuckSince = g_time;
				} else if (g_time - s.weaponStuckSince > 3.0f) {
					fstate->actorState2.weaponState = W::kSheathed;
					f->NotifyAnimationGraph("IdleForceDefaultState"sv);
					logger::info("{}: weapon stuck {} for 3 s, reset to sheathed", Name(f), static_cast<int>(ws));
					s.weaponStuckSince = -1.0f;
				}
			}

			// potions: a heal when hurt, buffs at the start of a fight worth it
			{
				// fodder only if every enemy in the fight is (a boss's summons are fodder, the boss isn't)
				bool fodder = !a_scan.enemies.empty();
				for (auto e : a_scan.enemies) {
					if (Alive(e) && !Experience::StanceVs(f, e).relaxed) {
						fodder = false;
						break;
					}
				}
				Potions::Update(f, inCombat, fodder, g_time);
			}

			// knocked down: by whom (what hit her just now, or the enemy nearest her)
			{
				bool knocked = fstate->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal;
				if (knocked && !s.wasKnocked) {
					auto by = g_time - s.lastAttackedTime < 1.0f ? Get(s.lastAttacker) : nullptr;
					if (Alive(by) && by != player && !by->IsPlayerTeammate()) TallyKnock(f, by);
				}
				s.wasKnocked = knocked;
				RecheckFutile(f);
			}

			// sprinting on the spot (the combat AI sprints her into a coffin, a wall, a crowd): a player lets
			// go of sprint when they aren't getting anywhere
			if (fstate->IsSprinting() && !Driver::IsDriving(f)) {
				if (s.sprintCheckTime < 0.0f) {
					s.sprintCheckTime = g_time;
					s.sprintCheckPos = f->GetPosition();
				} else if (g_time - s.sprintCheckTime >= 0.6f) {
					if (f->GetPosition().GetDistance(s.sprintCheckPos) < 0.6f * 120.0f) {
						fstate->actorState1.sprinting = 0;
						f->SetGraphVariableBool("IsSprinting", false);
						f->NotifyAnimationGraph("SprintStop"sv);
						s.mirroringSprint = false;
						logger::info("{} stops sprinting on the spot", Name(f));
					}
					s.sprintCheckTime = -1.0f;
				}
			} else {
				s.sprintCheckTime = -1.0f;
			}

			if (cfg.teleport && cfg.outsideMovement && travelling && !inCombat && dist > tune.teleportDistance) {
				if (s.farSince < 0.0f) s.farSince = g_time;
				bool seen = false;
				bool unused = false;
				RE::NiPoint3 toF = f->GetPosition() - player->GetPosition();
				toF.z = 0.0f;
				float len = toF.Length();
				if (len > 1.0f) {
					auto fwd = Forward(player);
					float cosAngle = (toF.x * fwd.x + toF.y * fwd.y) / len;
					seen = cosAngle > 0.2f && player->HasLineOfSight(f, unused);
				}
				if (!seen && g_time - s.farSince > kTeleportDelay) {
					auto fwd = Forward(player);
					Papyrus::MoveTo(f, player, -fwd.x * 200.0f, -fwd.y * 200.0f);
					s.farSince = -1.0f;
					s.offsetApplied = -100.0f;
					Debug(std::format("{} caught up (teleport)", Name(f)));
				}
			} else {
				s.farSince = -1.0f;
			}

			// ---- out of combat: match the player's weapon choice
			if (!inCombat && !recalling && cfg.matchWeapons && !busy && !player->IsInCombat()) {
				int playerRanged = player->AsActorState()->IsWeaponDrawn() ? (IsRanged(player) ? 1 : 0) : -1;
				if (playerRanged >= 0 && playerRanged != (IsRanged(f) ? 1 : 0)) {
					auto& options = GetLoadout(f, s);
					if (playerRanged == 1 ? options.HasRanged() : options.HasMelee()) {
						Swap(f, s, playerRanged == 1, "matching you");
					}
				}
			}

			if (!inCombat) SetWeaponPref(f, 0);
			if (!inCombat || recalling) {
				s.missedSwings = 0;
				s.stuckSince = -1.0f;
				// join the fight when the player is fighting and nobody told us
				if (!recalling && !waiting && !g_retreating && cfg.awareness && player->IsInCombat()) {
					float reach = (cfg.combatLeash && cfg.combatMovement) ? leash : 2500.0f;
					if (auto enemy = PickEnemy(f, a_scan, nullptr, reach); enemy && g_time - s.lastRetarget > 1.0f && g_time - s.lastStop > 3.0f) {
						StartCombat(f, s, enemy, "joining your fight");
					}
				}
				return;
			}

			auto target = CombatTarget(f);
			if (engaging && !f->IsInCombat()) return;

			// ---- retarget when the target is gone
			if (cfg.retarget && !Alive(target) && g_time - s.lastRetarget > 0.5f) {
				if (auto next = PickEnemy(f, a_scan, nullptr, (cfg.combatLeash && cfg.combatMovement) ? leash : kEnemyRange)) StartCombat(f, s, next, "next enemy");
				return;
			}
			if (!target) return;
			// the weapon first: the rules below may end the tick (mowing down a weak enemy did, every tick,
			// and kept her in melee long after the enemies had turned to the player)
			if (cfg.smartWeapons) UpdateWeapons(f, s, target, false);
			if (cfg.reflexes && Meatgrind(f, s, a_scan, target)) return;

			// ---- a downed (bleeding out, kneeling) enemy: close in and finish it, don't stand back
			if (cfg.combatMovement && target->AsActorState()->IsBleedingOut() && !IsRanged(f) && Dist(f, target) < 900.0f) {
				float reach = 150.0f;
				if (auto weap = f->GetEquippedObject(false); weap && weap->As<RE::TESObjectWEAP>()) {
					reach = std::max(120.0f, weap->As<RE::TESObjectWEAP>()->GetReach() * 140.0f);
				}
				if (Dist(f, target) > reach + 40.0f) {
					Driver::Order order;
					order.kind = Driver::Kind::kRunTo;
					order.target = target->GetHandle();
					order.speed = Tuning::Get().closeSpeed;
					order.stopDistance = reach * 0.85f;
					order.duration = 2.0f;
					if (Driver::Start(f, order)) Debug(std::format("{} goes to finish {}", Name(f), Name(target)));
				} else if (f->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone && g_time - s.lastFinish > 1.0f) {
					StrikeAt(f, target);
					s.lastFinish = g_time;
				}
				return;
			}

			// ---- leash: don't chase enemies far away from the player
			if ((cfg.combatLeash && cfg.combatMovement) && dist > leash && Dist(target, player) > leash * 0.8f && g_time - s.lastLeash > 2.0f) {
				s.lastLeash = g_time;
				if (auto closer = PickEnemy(f, a_scan, nullptr, leash)) {
					StartCombat(f, s, closer, "too far from you");
				} else {
					s.recallUntil = g_time + 3.0f;
					Disengage(f, s);
					Debug(std::format("{} falls back to you", Name(f)));
				}
				return;
			}

			// ---- ranged or melee
			{
				float td = Dist(f, target);
				if (Get(s.progressTarget) != target) {
					s.progressTarget = target->GetHandle();
					s.bestTargetDist = td;
					s.bestTargetTime = g_time;
				} else if (td < s.bestTargetDist - 100.0f) {
					s.bestTargetDist = td;
					s.bestTargetTime = g_time;
				} else if (f->AsActorState()->IsBleedingOut() || g_time < s.hazardUntil || IsRanged(f)) {
					// down, in a trap, or shooting: not getting closer says nothing about the way there
					s.bestTargetDist = td;
					s.bestTargetTime = g_time;
				} else if (td > 300.0f && g_time - s.bestTargetTime > 5.0f && g_time >= s.unreachableUntil) {
					s.bestTargetTime = g_time;
					if (Reachable(f, target)) {
						// level ground and a clear line: it is backing off (a caster keeping its distance), not out
						// of reach. A player runs it down rather than pulling a bow.
						s.bestTargetDist = td;
						Debug(std::format("{} keeps after {} (it backs off)", Name(f), Name(target)));
					} else {
						s.unreachableUntil = g_time + 8.0f;
						Debug(std::format("{} can't reach {}", Name(f), Name(target)));
					}
				}
				// the pathfinder can't get there: take the player's route, if it passes near the target
				if (cfg.combatMovement && g_time < s.unreachableUntil && !Driver::IsDriving(f) && RunTrail(f, s, player, target, "to the enemy")) {
					s.unreachableUntil = 0.0f;
					s.bestTargetTime = g_time;
					return;
				}
			}
			// ---- can't reach the target (even by the player's route or a hop): stick close to the player,
			// who is likely on the way there, until the target is near and on her level, then let the
			// game's pathfinding take over.
			if (s.tagAlong) {
				const char* why = nullptr;
				auto        fp = f->GetPosition();
				if (!cfg.combatMovement || !sameSpace || Get(s.tagTarget) != target || IsRanged(f)) {
					why = "stopped";
				} else if (Dist(f, target) < 800.0f && std::fabs(target->GetPosition().z - fp.z) < 150.0f) {
					why = "in reach";
				}
				RE::Actor* other = nullptr;
				if (!why) {
					for (auto e : a_scan.enemies) {
						if (e == target || !Alive(e)) continue;
						auto ep = e->GetPosition();
						if (fp.GetDistance(ep) < 700.0f && std::fabs(ep.z - fp.z) < 120.0f && Driver::ClearLine(f, fp, ep)) {
							other = e;
							why = "another enemy in reach";
							break;
						}
					}
				}
				if (why) {
					if (Driver::IsDriving(f)) Driver::Stop(f);
					s.tagAlong = false;
					s.unreachableUntil = 0.0f;
					s.bestTargetDist = Dist(f, target);
					s.bestTargetTime = g_time;
					Debug(std::format("{} stops tagging along ({})", Name(f), why));
					if (other) {
						StartCombat(f, s, other, "in reach");
						return;
					}
				} else {
					if (!Driver::IsDriving(f) && g_time >= s.tagRetryAt) {
						std::string_view lastEnd = Driver::LastEnd(f);
						bool             moved = false;
						if (lastEnd == "blocked" || lastEnd == "edge" || lastEnd == "step" || lastEnd == "too far") {
							moved = TryHop(f, s, player) || RunTrail(f, s, player, nullptr, "to you");
						}
						if (!moved) {
							Driver::Order order = FormationOrder();
							order.target = player->GetHandle();
							order.speed = Tuning::Get().chargeSpeed;
							order.sprintSpeed = 715.0f;
							order.looseness = 1.0f;
							order.duration = 3600.0f;
							if (!Driver::Start(f, order)) s.tagRetryAt = g_time + 1.5f;  // the game's pathing for a moment
						}
					}
					return;
				}
			}
			if (MaybeCharge(f, s, target)) return;
			if (cfg.combatMovement && !s.tagAlong && g_time < s.unreachableUntil && !IsRanged(f) && sameSpace &&
				Dist(f, target) > 300.0f && !Driver::IsDriving(f)) {
				s.tagAlong = true;
				s.tagTarget = target->GetHandle();
				s.tagRetryAt = 0.0f;
				Debug(std::format("{} can't reach {}, sticks with you", Name(f), Name(target)));
				return;
			}
			if (Dist(f, target) > 300.0f) StandUp(f);

			// ---- a target her hits don't hurt: the summoner that made it, or anyone else
			if (Futile(f, target) && g_time - s.lastRetarget > 1.0f) {
				auto summoner = target->IsCommandedActor() ? target->GetCommandingActor() : nullptr;
				RE::Actor* other = summoner && Alive(summoner.get()) && IsEnemyOf(summoner.get(), player) ? summoner.get() : PickEnemy(f, a_scan, target);
				if (other && other != target && !Futile(f, other)) {
					StartCombat(f, s, other, "can't hurt its target");
					return;
				}
			}

			// ---- summons: go for the summoner when it can be reached
			if (cfg.focusSummoners && target->IsCommandedActor() && g_time - s.lastRetarget > 1.0f) {
				bool ignored = Get(s.ignoredSummon) == target && g_time < s.ignoreSummonUntil;
				auto summoner = target->GetCommandingActor();
				// a summon already on her while its master is well away: cut it down first, as a player does
				// (otherwise she and the game's AI tug her target back and forth every second)
				bool summonOnHer = summoner && Dist(f, target) < 300.0f && Dist(f, summoner.get()) > Dist(f, target) + 300.0f;
				// worth going for only if it drops quickly (a necromancer in robes). A tough boss: clear its
				// adds first, the small victories that stop the pressure, as a player would.
				bool quickKill = false;
				if (summoner && Alive(summoner.get())) {
					float boss = KillTime(f, summoner.get());
					float add = KillTime(f, target);
					quickKill = boss <= std::max(4.0f, 2.0f * add);
					if (!quickKill && Get(s.addsFirstFor) != summoner.get()) {
						s.addsFirstFor = summoner->GetHandle();
						Debug(std::format("{}: {} is tough (~{:.0f}s to kill), clears its adds first", Name(f), Name(summoner.get()), boss));
					}
				}
				if (!ignored && !summonOnHer && quickKill && summoner && Alive(summoner.get()) && IsEnemyOf(summoner.get(), player) &&
					Dist(f, summoner.get()) < kSummonerReach) {
					s.redirectedFrom = target->GetHandle();
					StartCombat(f, s, summoner.get(), "summoner first");
					return;
				}
			}

			if (!cfg.meleeSwitch || IsRanged(f)) return;

			// ---- melee: two missed swings while someone else hits us -> switch
			auto attacker = Get(s.lastAttacker);
			if (s.missedSwings >= 2 && s.lastAttackedTime >= s.firstMissTime && Alive(attacker) && attacker != target) {
				auto other = NearestAttacker(f, a_scan, target);
				if (!other) other = attacker;
				f->NotifyAnimationGraph("attackStop"sv);
				StartCombat(f, s, other, "missed twice, switching");
				return;
			}

			// ---- melee: stuck short of the target
			if (g_time - s.sampleTime >= 1.0f) {
				float moved = f->GetPosition().GetDistance(s.samplePos);
				s.samplePos = f->GetPosition();
				s.sampleTime = g_time;
				bool farFromTarget = Dist(f, target) > 280.0f;
				if (farFromTarget && moved < 50.0f && !f->AsActorState()->IsBleedingOut()) {
					if (s.stuckSince < 0.0f) s.stuckSince = g_time;
				} else {
					s.stuckSince = -1.0f;
				}
			}
			if (s.stuckSince >= 0.0f && g_time - s.stuckSince >= 3.0f && g_time - s.lastRepath > 6.0f) {
				s.lastRepath = g_time;
				s.unreachableUntil = g_time + 8.0f;
				s.stuckSince = -1.0f;
				if (attacker && Alive(attacker) && attacker != target && g_time - s.lastAttackedTime < 4.0f) {
					StartCombat(f, s, attacker, "can't reach target, fighting attacker");
				} else {
					// Leave the game's pathfinding alone (resetting the AI threw its route away, e.g. up a
					// tower's stairs): only note the target as out of reach, so a bow comes out if she has one.
					s.unreachableUntil = g_time + 8.0f;
				}
			}
		}

		void RunLogic(RE::PlayerCharacter* a_player)
		{
			auto scan = DoScan(a_player);
			g_lastFollowers.clear();
			g_lastEnemies.clear();
			for (auto f : scan.followers) g_lastFollowers.push_back(f->GetHandle());
			for (auto e : scan.enemies) g_lastEnemies.push_back(e->GetHandle());

			std::vector<QueuedEvent> events;
			std::vector<HurtEvent>   hurt;
			{
				std::scoped_lock lock{ g_lock };
				events.swap(g_queue);
				hurt.swap(g_hurt);
			}
			for (auto& h : hurt) {
				auto victim = RE::TESForm::LookupByID<RE::Actor>(h.victim);
				auto attacker = h.attacker.get().get();
				if (!victim || !attacker || attacker->IsPlayerRef()) continue;
				float health = std::max(1.0f, victim->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) - Lab::ExtraHealth(victim));
				Experience::Hurt(victim, attacker, h.damage / health);
			}
			for (auto& e : events) HandleEvent(e, scan);

			std::vector<DamageRecord> damage;
			{
				std::scoped_lock lock{ g_lock };
				damage.swap(g_damage);
			}
			for (auto& d : damage) {
				auto it = g_state.find(d.attacker);
				auto actor = RE::TESForm::LookupByID<RE::Actor>(d.attacker);
				if (it == g_state.end() || !actor) continue;
				int kind = IsRanged(actor) ? 1 : 0;
				it->second.dealt[kind] += d.amount;
				it->second.hits[kind] += 1;
			}
			for (auto f : scan.followers) {
				if (f->IsInCombat()) g_state[f->GetFormID()].heldTime[IsRanged(f) ? 1 : 0] += kLogicInterval;
			}

			for (auto it = g_pendingPlayerHits.begin(); it != g_pendingPlayerHits.end();) {
				if (g_time >= it->time) {
					auto target = Get(it->target);
					if (Settings::Get().awareness && target && IsEnemyOf(target, a_player)) {
						EngageAll(scan, target, "you hit it");
					}
					it = g_pendingPlayerHits.erase(it);
				} else {
					++it;
				}
			}

			if (Settings::Get().awareness) UpdateWatch(scan);
			UpdateRetreat(scan);

			RecordTrail(a_player);
			int slot = 0;
			for (auto f : scan.followers) {
				UpdateFollower(f, slot++, scan);
			}

			// forget followers that left
			std::erase_if(g_state, [&](auto& kv) {
				auto form = RE::TESForm::LookupByID<RE::Actor>(kv.first);
				if (form && IsFollower(form, a_player)) return false;
				if (form) {
					ClearOffset(form, kv.second);
					SetBoost(form, kv.second, 0.0f);
					SetFleeing(form, kv.second, false);
					Tactics::Restore(form);
					for (auto [av, amount] : kv.second.avMods) form->AsActorValueOwner()->ModActorValue(av, -amount);
				}
				return true;
			});
		}

		// ------------------------------------------------------------------ hooks & sinks
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_player, float a_delta)
			{
				func(a_player, a_delta);
				Tick(a_delta);
				Test::Tick(a_delta);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The combat AI rates every weapon a fighter carries and draws the best. Tilt that rating toward
		// the kind this mod chose, so the AI draws it itself instead of undoing our choice.
		template <int Kind>  // 1 melee, 2 ranged
		struct ItemScore
		{
			static float thunk(RE::CombatInventoryItem* a_item, RE::CombatController* a_controller)
			{
				float score = func(a_item, a_controller);
				int   pref = GetWeaponPref(a_controller);
				if (pref == 0) return score;
				++g_scoreCalls;
				return pref == Kind ? score * 10.0f + 10.0f : score * 0.05f;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Refuse to draw the other kind of weapon at all while the mod has made a choice.
		template <int Kind>
		struct ItemShouldEquip
		{
			static bool thunk(RE::CombatInventoryItem* a_item, RE::CombatController* a_controller)
			{
				int pref = GetWeaponPref(a_controller);
				if (pref != 0 && pref != Kind) {
					++g_equipVetoes;
					return false;
				}
				return func(a_item, a_controller);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct HealthDamage
		{
			static void thunk(RE::Actor* a_victim, RE::Actor* a_attacker, float a_damage)
			{
				func(a_victim, a_attacker, a_damage);
				if (a_attacker && a_victim && !a_victim->IsPlayerRef() && a_victim->IsPlayerTeammate() && !a_attacker->IsPlayerTeammate()) {
					std::scoped_lock lock{ g_lock };
					g_stats.damageTaken += std::abs(a_damage);
					g_lastFollowerHurt = g_time;
					g_hurt.push_back({ a_victim->GetFormID(), a_attacker->GetHandle(), std::abs(a_damage) });
				}
				if (a_attacker && a_attacker != a_victim && !a_attacker->IsPlayerRef() && a_attacker->IsPlayerTeammate()) {
					std::scoped_lock lock{ g_lock };
					g_damage.push_back({ a_attacker->GetFormID(), std::abs(a_damage) });
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Queue(EventType a_type, RE::ActorHandle a_a, RE::ActorHandle a_b = {})
		{
			std::scoped_lock lock{ g_lock };
			g_queue.push_back({ a_type, a_a, a_b });
		}

		class Sink :
			public RE::BSTEventSink<RE::TESHitEvent>,
			public RE::BSTEventSink<RE::TESDeathEvent>,
			public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>,
			public RE::BSTEventSink<SKSE::ActionEvent>
		{
		public:
			static Sink* Get()
			{
				static Sink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (a_event && a_event->cause && a_event->target) {
					Queue(EventType::kHit, HandleOf(a_event->cause.get()), HandleOf(a_event->target.get()));
					auto source = a_event->source ? RE::TESForm::LookupByID(a_event->source) : nullptr;
					auto weap = source ? source->As<RE::TESObjectWEAP>() : nullptr;
					// a blocked hit does next to nothing: it says nothing about whether the target can be hurt
					// (an enemy behind a shield isn't immune), nor is it a dodge that failed
					bool blocked = a_event->flags.any(RE::TESHitEvent::Flag::kHitBlocked);
					if (!a_event->projectile && !blocked && (!source || (weap && weap->IsMelee()))) {
						Queue(EventType::kMeleeHit, HandleOf(a_event->cause.get()), HandleOf(a_event->target.get()));
					}
				}
				if (a_event && a_event->cause && a_event->target && a_event->projectile && !a_event->target->As<RE::Actor>()) {
					Queue(EventType::kShotBlocked, HandleOf(a_event->cause.get()));
				}
				if (a_event && a_event->target && (!a_event->cause || !a_event->cause->As<RE::Actor>())) {
					Queue(EventType::kHazard, {}, HandleOf(a_event->target.get()));
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (a_event && a_event->actorDying) Queue(EventType::kDeath, HandleOf(a_event->actorDying.get()));
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
			{
				auto casterActor = a_event && a_event->caster ? a_event->caster->As<RE::Actor>() : nullptr;
				if (casterActor && (casterActor->IsPlayerRef() || casterActor->IsPlayerTeammate()) && IsHealingEffect(a_event->magicEffect)) {
					Queue(EventType::kPlayerHeal, {}, HandleOf(a_event->target.get()));
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override
			{
				if (!a_event || !a_event->actor) return RE::BSEventNotifyControl::kContinue;
				using T = SKSE::ActionEvent::Type;
				auto type = a_event->type.get();
				if (a_event->actor->IsPlayerRef()) {
					if (type == T::kSpellCast || type == T::kSpellFire || type == T::kVoiceCast) {
						Queue(EventType::kPlayerCast, {});
					}
					if (type == T::kSpellCast || type == T::kSpellFire) {
						if (IsHealingSpell(a_event->sourceForm)) {
							Queue(EventType::kPlayerHeal, {});
							return RE::BSEventNotifyControl::kContinue;
						}
					}
					if (type == T::kWeaponSwing || type == T::kBowRelease || (type == T::kSpellFire && IsHostileSpell(a_event->sourceForm))) {
						Queue(EventType::kPlayerAttack, a_event->actor->GetHandle());
					}
				} else if (type == T::kWeaponSwing) {
					Queue(EventType::kFollowerSwing, a_event->actor->GetHandle());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		constexpr std::uint32_t kRecordBoosts = 'BOST';
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdate::func = vtbl.write_vfunc(0xAD, PlayerUpdate::thunk);
		REL::Relocation<std::uintptr_t> characterVtbl{ RE::VTABLE_Character[0] };
		HealthDamage::func = characterVtbl.write_vfunc(0x104, HealthDamage::thunk);
		Driver::Install();
		Recorder::Install();
		Lab::Install();
		REL::Relocation<std::uintptr_t> meleeVtbl{ RE::VTABLE_CombatInventoryItemMelee[0] };
		ItemScore<1>::func = meleeVtbl.write_vfunc(0x0C, ItemScore<1>::thunk);
		ItemShouldEquip<1>::func = meleeVtbl.write_vfunc(0x0F, ItemShouldEquip<1>::thunk);
		REL::Relocation<std::uintptr_t> rangedVtbl{ RE::VTABLE_CombatInventoryItemRanged[0] };
		ItemScore<2>::func = rangedVtbl.write_vfunc(0x0C, ItemScore<2>::thunk);
		ItemShouldEquip<2>::func = rangedVtbl.write_vfunc(0x0F, ItemShouldEquip<2>::thunk);

		auto holder = RE::ScriptEventSourceHolder::GetSingleton();
		holder->AddEventSink<RE::TESHitEvent>(Sink::Get());
		holder->AddEventSink<RE::TESDeathEvent>(Sink::Get());
		holder->AddEventSink<RE::TESMagicEffectApplyEvent>(Sink::Get());
		Host::ActionEvents()->AddEventSink(Sink::Get());
		logger::info("Hooks and event sinks installed");
	}

	void Shutdown()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		vtbl.write_vfunc(0xAD, PlayerUpdate::func.address());
		REL::Relocation<std::uintptr_t> characterVtbl{ RE::VTABLE_Character[0] };
		characterVtbl.write_vfunc(0x104, HealthDamage::func.address());
		REL::Relocation<std::uintptr_t> meleeVtbl{ RE::VTABLE_CombatInventoryItemMelee[0] };
		meleeVtbl.write_vfunc(0x0C, ItemScore<1>::func.address());
		meleeVtbl.write_vfunc(0x0F, ItemShouldEquip<1>::func.address());
		REL::Relocation<std::uintptr_t> rangedVtbl{ RE::VTABLE_CombatInventoryItemRanged[0] };
		rangedVtbl.write_vfunc(0x0C, ItemScore<2>::func.address());
		rangedVtbl.write_vfunc(0x0F, ItemShouldEquip<2>::func.address());

		auto holder = RE::ScriptEventSourceHolder::GetSingleton();
		holder->RemoveEventSink<RE::TESHitEvent>(Sink::Get());
		holder->RemoveEventSink<RE::TESDeathEvent>(Sink::Get());
		holder->RemoveEventSink<RE::TESMagicEffectApplyEvent>(Sink::Get());
		Host::ActionEvents()->RemoveEventSink(Sink::Get());

		Driver::Uninstall();
		Experience::Stash();
		Lab::Shutdown();
		Test::Shutdown();
		// put the followers back the way the game had them
		for (auto& [id, st] : g_state) {
			auto actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!actor) continue;
			for (auto [av, amount] : st.avMods) actor->AsActorValueOwner()->ModActorValue(av, -amount);
			st.avMods.clear();
			if (st.offsetMode >= 0) Papyrus::ClearOffset(actor);
			Tactics::Restore(actor);
		}
		g_state.clear();
		logger::info("Hooks and event sinks removed");
	}

	void Reset()
	{
		Tuning::Load();
		Lab::ForgetHealth();
		Experience::Apply();
		g_hitTally.clear();
		g_knockTally.clear();
		g_futile.clear();
		g_guardBreaks.clear();
		Potions::Forget();
		{
			std::scoped_lock lock{ g_lock };
			g_queue.clear();
		}
		g_state.clear();
		g_pendingPlayerHits.clear();
		g_samples.clear();
		g_retreating = false;
		g_marked = {};
		g_watchTarget = {};
		g_reflex.clear();
		g_enemyAttack.clear();
		g_enemyStagger.clear();
		g_nextCharge.clear();
		Driver::StopAll();
		g_stats = {};
		g_pendingCheck.clear();
		{
			std::scoped_lock lock{ g_prefLock };
			g_weaponPref.clear();
		}

		// undo temporary changes that were saved into this game
		for (auto& [id, av, amount] : g_pendingUndo) {
			if (auto actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
				actor->AsActorValueOwner()->ModActorValue(av, -amount);
				logger::info("Removed leftover change ({}) from {}", static_cast<int>(av), Name(actor));
			}
		}
		g_pendingUndo.clear();
	}

	void Tick(float a_delta)
	{
		g_time += a_delta;
		g_logicTimer += a_delta;
		Driver::Frame(a_delta);
		Recorder::Frame(a_delta);
		Lab::Frame(a_delta);
		UpdateReflexes();
		if (g_logicTimer < kLogicInterval) return;
		g_logicTimer = 0.0f;

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) return;
		RunLogic(player);
	}

	void Tracked(std::vector<RE::Actor*>& a_followers, std::vector<RE::Actor*>& a_enemies)
	{
		for (auto& h : g_lastFollowers) {
			if (auto a = h.get().get()) a_followers.push_back(a);
		}
		for (auto& h : g_lastEnemies) {
			if (auto a = h.get().get()) a_enemies.push_back(a);
		}
	}

	DebugInfo GetDebugInfo(RE::Actor* a_actor)
	{
		DebugInfo info;
		if (!a_actor) return info;
		{
			std::scoped_lock lock{ g_prefLock };
			auto it = g_weaponPref.find(a_actor->GetFormID());
			if (it != g_weaponPref.end()) info.pref = it->second;
		}
		info.stepOuts = g_stats.stepOuts;
		info.blocks = g_stats.blocks;
		info.bashes = g_stats.bashes;
		info.taken = g_stats.taken;
		info.punishes = g_stats.punishes;
		info.zigzags = g_stats.zigzags;
		info.damageTaken = g_stats.damageTaken;
		info.enemyAttacks = g_stats.enemyAttacks;
		info.avoided = g_stats.avoided;
		auto it = g_state.find(a_actor->GetFormID());
		if (it != g_state.end()) {
			for (int i = 0; i < 2; ++i) {
				info.dealt[i] = it->second.dealt[i];
				info.hits[i] = it->second.hits[i];
			}
		}
		return info;
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		std::vector<std::tuple<RE::FormID, RE::ActorValue, float>> mods;
		for (auto& [id, st] : g_state) {
			for (auto [av, amount] : st.avMods) mods.emplace_back(id, av, amount);
		}
		Experience::Save(a_intfc);
		if (!a_intfc->OpenRecord(kRecordBoosts, 2)) return;
		auto count = static_cast<std::uint32_t>(mods.size());
		a_intfc->WriteRecordData(count);
		for (auto& [id, av, amount] : mods) {
			a_intfc->WriteRecordData(id);
			a_intfc->WriteRecordData(static_cast<std::uint32_t>(av));
			a_intfc->WriteRecordData(amount);
		}
	}

	void OnLoad(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t type, version, length;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == Experience::kRecord) {
				Experience::Load(a_intfc, version, length);
				continue;
			}
			if (type != kRecordBoosts) continue;
			std::uint32_t count = 0;
			a_intfc->ReadRecordData(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				RE::FormID    id = 0, resolved = 0;
				std::uint32_t av = static_cast<std::uint32_t>(RE::ActorValue::kSpeedMult);
				float         amount = 0.0f;
				a_intfc->ReadRecordData(id);
				if (version >= 2) a_intfc->ReadRecordData(av);
				a_intfc->ReadRecordData(amount);
				if (a_intfc->ResolveFormID(id, resolved)) g_pendingUndo.emplace_back(resolved, static_cast<RE::ActorValue>(av), amount);
			}
		}
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		Experience::Revert();
		g_pendingUndo.clear();
	}
}
