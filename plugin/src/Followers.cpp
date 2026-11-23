#include "Followers.h"
#include "Driver.h"
#include "Tuning.h"
#include "Recorder.h"
#include "Loadout.h"
#include "Papyrus.h"
#include "Settings.h"
#include "Tactics.h"
#include "Test.h"

namespace Followers
{
	namespace
	{
		using Mode = Settings::FollowDistance;

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
			float engagingUntil = 0.0f;       // just told to attack: the game may not show combat yet
			float lastFinish = -100.0f;
			// measured damage while holding each kind of weapon (0 melee, 1 ranged)
			float dealt[2]{};
			float heldTime[2]{};
			int   hits[2]{};
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
			kFollowerSwing,
			kHit,
			kDeath
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

		void ApplyOffset(RE::Actor* a, FollowerState& s, RE::PlayerCharacter* a_player, int a_mode, int a_slot)
		{
			if (s.offsetMode == a_mode && s.offsetSlot == a_slot) return;
			auto& t = kTuning[a_mode];
			float side = a_slot == 0 ? 0.0f : (a_slot % 2 ? 1.0f : -1.0f) * t.side * static_cast<float>((a_slot + 1) / 2);
			float back = -(t.back + 50.0f * static_cast<float>((a_slot + 1) / 2));
			Papyrus::KeepOffset(a, a_player, side, back, t.catchUpRadius, t.followRadius);
			logger::info("{} keeps offset ({:.0f}, {:.0f}) mode {}", Name(a), side, back, a_mode);
			s.offsetMode = a_mode;
			s.offsetSlot = a_slot;
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
		RE::Actor* PickEnemy(RE::Actor* a, const Scan& a_scan, RE::Actor* a_exclude = nullptr, float a_maxFromPlayer = kEnemyRange)
		{
			RE::Actor* best = nullptr;
			float      bestScore = std::numeric_limits<float>::max();
			for (auto enemy : a_scan.enemies) {
				if (enemy == a_exclude || !TargetsParty(enemy, a_scan) || Dist(enemy, a_scan.player) > a_maxFromPlayer) continue;
				float score = Dist(enemy, a_scan.player) * 0.6f + Dist(enemy, a) * 0.4f;
				if (CombatTarget(enemy) == a_scan.player) score -= 400.0f;
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
				if (enemy == a_exclude || CombatTarget(enemy) != a) continue;
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
			if (!cfg.retreat || !player->IsInCombat() || a_scan.enemies.empty()) {
				if (g_retreating) Debug("Retreat over");
				g_retreating = false;
				g_samples.clear();
				return;
			}
			if (PlayerIsHealing(player)) g_lastHeal = g_time;

			float nearest = std::numeric_limits<float>::max();
			for (auto e : a_scan.enemies) nearest = std::min(nearest, Dist(e, player));
			g_samples.push_back({ g_time, nearest, player->GetPosition() });
			while (!g_samples.empty() && g_time - g_samples.front().time > 3.0f) g_samples.pop_front();

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
				auto  old = sampleAgo(sprint ? 0.8f : 1.5f);
				if (!old || g_time - old->time < (sprint ? 0.6f : 1.2f)) return;
				float gained = nearest - old->enemyDistance;
				float moved = now.GetDistance(old->playerPos);
				bool  attacking = g_time - g_lastPlayerAttack < 1.5f;
				if (gained > 150.0f && moved > 200.0f && !attacking && !healing) {
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
			int   action = 0;  // 0 none, 1 step out, 2 block, 3 bash
			float until = 0.0f;
			float cooldownUntil = 0.0f;
			float punishAt = -1.0f;
			float dodgeReadyAt = 0.0f;
			RE::ActorHandle against;
			float bashTime = -100.0f;
			bool  offset = false;
		};

		std::unordered_map<RE::FormID, ReflexState>                  g_reflex;
		std::unordered_map<RE::FormID, RE::ATTACK_STATE_ENUM>         g_enemyAttack;  // last seen attack state
		std::unordered_map<RE::FormID, bool>                          g_enemyStagger;
		std::unordered_map<RE::FormID, int>                           g_bashFails;    // enemies that shrug off bashes
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

		void ReactToAttack(RE::Actor* f, RE::Actor* e, ReflexState& r)
		{
			auto process = e->GetActorRuntimeData().currentProcess;
			auto data = process && process->high ? process->high->attackData.get() : nullptr;
			bool  power = data && data->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack);
			float mult = data ? std::max(1.0f, data->data.damageMult) : 1.0f;
			float hit = Loadout::HitDamage(e, f, e->GetEquippedEntryData(false)) * mult;
			float health = f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
			float share = health > 0.0f ? hit / health : 1.0f;
			// Bashing needs a block: a shield, a two-handed weapon, or one weapon with the other hand free.
			// Dual wielders can't block, so they can't bash. Enemies that shrug bashes off are remembered.
			auto  right = f->GetEquippedObject(false);
			auto  left = f->GetEquippedObject(true);
			auto  rightWeap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
			bool  canBlock = (left && left->IsArmor()) ||
			                (rightWeap && (rightWeap->IsTwoHandedSword() || rightWeap->IsTwoHandedAxe())) ||
			                (rightWeap && rightWeap->IsMelee() && !left);
			bool  bashable = canBlock && g_bashFails[e->GetFormID()] < 2 && Dist(f, e) < 200.0f;

			const char* what = nullptr;
			if (power && bashable) {
				f->NotifyAnimationGraph("blockStart"sv);
				f->NotifyAnimationGraph("bashStart"sv);
				r.action = 3;
				r.until = g_time + 0.5f;
				r.bashTime = g_time;
				++g_stats.bashes;
				what = "bash";
			} else if ((power || share > Tuning::Get().dodgeShare) && g_time >= r.dodgeReadyAt &&
					   static_cast<float>(std::rand() % 100) < Tuning::Get().dodgeChance) {
				if (!power && HasShield(f)) {
					f->NotifyAnimationGraph("blockStart"sv);
					r.action = 2;
					r.until = g_time + 0.7f;
					++g_stats.blocks;
					what = "block";
				} else {
					// step back out of reach (offset is in front of the enemy, away from it)
					float reach = 250.0f;
					if (auto weap = e->GetEquippedObject(false); weap && weap->As<RE::TESObjectWEAP>()) {
						reach = std::max(150.0f, weap->As<RE::TESObjectWEAP>()->GetReach() * 140.0f);
					}
					Driver::Order order;
					order.kind = Driver::Kind::kStepBack;
					order.target = e->GetHandle();
					order.speed = Tuning::Get().stepSpeed;  // a player's diagonal backpedal (measured)
					order.weave = Tuning::Get().stepDiagonal;
					order.stopDistance = Dist(f, e) + reach * 0.5f + 80.0f;
					order.duration = 1.8f;  // the drive ends when the swing is over
					order.holdWhileAttacked = true;
					Driver::Start(f, order);
					r.action = 1;
					r.until = g_time + 2.0f;
					r.dodgeReadyAt = g_time + Tuning::Get().dodgeCooldown;  // a breather before the next dodge
					++g_stats.stepOuts;
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

		void UpdateReflexes()
		{
			if (!Settings::Get().reflexes) return;
			for (auto it = g_pendingCheck.begin(); it != g_pendingCheck.end();) {
				if (g_time - *it < 1.2f) {
					++it;
					continue;
				}
				if (g_lastFollowerHurt < *it) ++g_stats.avoided;
				it = g_pendingCheck.erase(it);
			}
			for (auto& fh : g_lastFollowers) {
				auto f = Get(fh);
				if (!f || !Alive(f)) continue;
				auto& r = g_reflex[f->GetFormID()];

				if (r.action == 3 && g_time - r.bashTime > 0.2f && g_time - r.bashTime < 0.35f &&
					f->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kBash) {
					// the bash did not happen: fall back to stepping out of reach
					auto e = Get(r.against);
					EndReflex(f, r);
					if (Alive(e)) {
						Driver::Order order;
						order.kind = Driver::Kind::kStepBack;
						order.target = e->GetHandle();
						order.speed = 480.0f;
						order.stopDistance = Dist(f, e) + 250.0f;
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
				}
				if (r.action != 0 && g_time >= r.until) {
					// a bash that did not stagger counts against that enemy
					if (r.action == 3) {
						auto e = Get(r.against);
						if (e && !IsStaggered(e)) ++g_bashFails[e->GetFormID()];
					}
					EndReflex(f, r);
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
						f->NotifyAnimationGraph("attackStart"sv);
						++g_stats.punishes;
						r.punishAt = -1.0f;
						Debug(std::format("{}: punish {}", Name(f), Name(e)));
					} else if (!Driver::IsDriving(f)) {
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
						f->NotifyAnimationGraph("attackStart"sv);
						++g_stats.punishes;
						Debug(std::format("{}: punish staggered {}", Name(f), Name(e)));
						continue;
					}

					// a new attack: winding up or swinging now, and not a moment ago (chained attacks skip kNone)
					auto windup = [](RE::ATTACK_STATE_ENUM a) {
						return a == RE::ATTACK_STATE_ENUM::kDraw || a == RE::ATTACK_STATE_ENUM::kSwing;
					};
					bool starting = windup(state) && !windup(prev);
					if (starting) ++g_stats.enemyAttacks;
					// the attack is meant for us if we are its target, or we are in front of it and within reach
					bool aimedAtUs = CombatTarget(e) == f;
					if (!aimedAtUs && d < 350.0f) {
						RE::NiPoint3 toF = f->GetPosition() - e->GetPosition();
						float len = std::sqrt(toF.x * toF.x + toF.y * toF.y);
						auto  fwd = Forward(e);
						aimedAtUs = len > 1.0f && (toF.x * fwd.x + toF.y * fwd.y) / len > 0.5f;
					}
					if (starting && aimedAtUs && r.action == 0 && g_time >= r.cooldownUntil) {
						ReactToAttack(f, e, r);
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
			if (!cfg.zigzag || !Alive(a_target) || !IsRanged(a_target) || g_retreating) return false;
			if (g_reflex[f->GetFormID()].action != 0) return false;
			if (g_time < g_nextCharge[f->GetFormID()]) return false;
			float bias = cfg.combatDistance == 0 ? 1.25f : (cfg.combatDistance == 2 ? 0.8f : 1.0f);
			if (GetLoadout(f, s).PrefersRanged(bias)) return false;  // archers and mages don't charge
			float d = Dist(f, a_target);
			// Far: charge right away. Close/Medium: trade shots first, charge from about two seconds away.
			float startAt = cfg.combatDistance == 2 ? 4000.0f : kSprintReach;
			if (d < kChargeStart || d > startAt) return false;
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
			g_nextCharge[f->GetFormID()] = g_time + 4.0f;
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
				if (Alive(e)) best = std::min(best, Dist(f, e));
			}
			return best;
		}

		void UpdateWeapons(RE::Actor* f, FollowerState& s, RE::Actor* a_target, bool a_retreating)
		{
			auto  options = GetLoadout(f, s);
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
				// archer/mage: keep shooting, melee only when an enemy is in their face
				if (d < 300.0f) {
					SetWeaponPref(f, 1);
					if (ranged) Swap(f, s, false, "enemy in their face");
				} else if (d > 600.0f) {
					SetWeaponPref(f, 2);
					if (!ranged) Swap(f, s, true, "back to range");
				}
			} else {
				// fighter: charge. Shoot only at what they can't reach, or what is too far from the player to chase.
				auto  player = RE::PlayerCharacter::GetSingleton();
				float leash = kTuning[std::clamp(cfg.combatDistance, 0, 2)].combatLeash;
				bool  cantChase = (cfg.combatLeash && Dist(a_target, player) > leash) || g_time < s.unreachableUntil;
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

		void UpdateFollower(RE::Actor* f, int a_slot, const Scan& a_scan)
		{
			auto& cfg = Settings::Get();
			auto  player = a_scan.player;
			auto& s = g_state[f->GetFormID()];
			Tactics::Apply(f);
			int   mode = std::clamp(cfg.followDistance, 0, 2);
			int   combatMode = std::clamp(cfg.combatDistance + (g_time < s.healedUntil ? 1 : 0), 0, 2);
			auto& tune = kTuning[mode];
			float leash = kTuning[combatMode].combatLeash;
			float dist = Dist(f, player);
			bool  engaging = g_time < s.engagingUntil;
			bool  inCombat = f->IsInCombat() || engaging;
			bool  waiting = IsWaiting(f);
			auto  state = f->AsActorState();
			bool  busy = waiting || InScene(f) || f->IsOnMount() || player->IsOnMount() ||
			            state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal;
			bool  sameSpace = f->GetWorldspace() == player->GetWorldspace() &&
			                 (player->GetWorldspace() || f->GetParentCell() == player->GetParentCell());

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

			// the driver moves this follower right now: leave it alone
			if (Driver::IsDriving(f)) return;

			// ---- keep-offset follow
			bool wantOffset = !busy && sameSpace && dist < 4000.0f &&
			                  (recalling || (!inCombat && mode != 2));
			if (wantOffset) {
				ApplyOffset(f, s, player, recalling ? 0 : mode, a_slot);
			} else {
				ClearOffset(f, s);
			}

			// ---- catch-up speed and teleport
			bool travelling = (!inCombat || recalling) && !busy && sameSpace;
			float boostAt = recalling ? 300.0f : tune.boostDistance;
			if (cfg.catchUp && travelling && dist > boostAt) {
				SetBoost(f, s, kSpeedBoost);
			} else if (s.speedBoost != 0.0f && (!travelling || dist < boostAt * 0.6f)) {
				SetBoost(f, s, 0.0f);
			}

			if (cfg.teleport && travelling && !inCombat && dist > tune.teleportDistance) {
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
					float reach = cfg.combatLeash ? leash : 2500.0f;
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
				if (auto next = PickEnemy(f, a_scan, nullptr, cfg.combatLeash ? leash : kEnemyRange)) StartCombat(f, s, next, "next enemy");
				return;
			}
			if (!target) return;

			// ---- a downed (bleeding out, kneeling) enemy: close in and finish it, don't stand back
			if (target->AsActorState()->IsBleedingOut() && !IsRanged(f) && Dist(f, target) < 900.0f) {
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
					f->NotifyAnimationGraph("attackStart"sv);
					s.lastFinish = g_time;
				}
				return;
			}

			// ---- leash: don't chase enemies far away from the player
			if (cfg.combatLeash && dist > leash && Dist(target, player) > leash * 0.8f && g_time - s.lastLeash > 2.0f) {
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
			if (MaybeCharge(f, s, target)) return;
			if (cfg.smartWeapons) UpdateWeapons(f, s, target, false);
			if (Dist(f, target) > 300.0f) StandUp(f);

			// ---- summons: go for the summoner when it can be reached
			if (cfg.focusSummoners && target->IsCommandedActor() && g_time - s.lastRetarget > 1.0f) {
				bool ignored = Get(s.ignoredSummon) == target && g_time < s.ignoreSummonUntil;
				auto summoner = target->GetCommandingActor();
				if (!ignored && summoner && Alive(summoner.get()) && IsEnemyOf(summoner.get(), player) &&
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
				if (farFromTarget && moved < 50.0f) {
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
					f->EvaluatePackage(true, false);
					Papyrus::StartCombat(f, target);
					s.engagingUntil = g_time + 2.0f;
					Debug(std::format("{} can't reach {}, re-pathing", Name(f), Name(target)));
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
			{
				std::scoped_lock lock{ g_lock };
				events.swap(g_queue);
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
				if (a_event && a_event->caster && a_event->caster->IsPlayerRef() && IsHealingEffect(a_event->magicEffect)) {
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
		SKSE::GetActionEventSource()->AddEventSink(Sink::Get());
		logger::info("Hooks and event sinks installed");
	}

	void Reset()
	{
		Tuning::Load();
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
		UpdateReflexes();
		if (g_logicTimer < kLogicInterval) return;
		g_logicTimer = 0.0f;

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) return;
		RunLogic(player);
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
		g_pendingUndo.clear();
	}
}
