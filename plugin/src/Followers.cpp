#include "Followers.h"
#include "Papyrus.h"
#include "Settings.h"

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
			{ 0.0f, 0.0f, 0.0f, 0.0f, 1100.0f, 3200.0f, 2500.0f },         // far
		};

		constexpr float kSpeedBoost = 50.0f;
		constexpr float kLogicInterval = 0.1f;
		constexpr float kTeleportDelay = 3.0f;
		constexpr float kEnemyRange = 5000.0f;
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
			kPlayerHeal,
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
		std::unordered_map<RE::FormID, float>      g_pendingBoostUndo;

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

		bool IsFollowing(RE::Actor* a)
		{
			auto package = a->GetCurrentPackage();
			return package && package->packData.packType == RE::PACKAGE_PROCEDURE_TYPE::kFollow;
		}

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

		bool IsRanged(RE::Actor* a)
		{
			for (bool left : { false, true }) {
				auto obj = a->GetEquippedObject(left);
				if (!obj) continue;
				if (auto weap = obj->As<RE::TESObjectWEAP>()) {
					if (weap->IsBow() || weap->IsCrossbow() || weap->IsStaff()) return true;
				} else if (IsHostileSpell(obj)) {
					return true;
				}
			}
			return false;
		}

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
		void SetBoost(RE::Actor* a, FollowerState& s, float a_amount)
		{
			if (s.speedBoost == a_amount) return;
			auto avo = a->AsActorValueOwner();
			avo->ModActorValue(RE::ActorValue::kSpeedMult, a_amount - s.speedBoost);
			// Speed changes only take effect when carry weight is recalculated.
			avo->ModActorValue(RE::ActorValue::kCarryWeight, 0.01f);
			avo->ModActorValue(RE::ActorValue::kCarryWeight, -0.01f);
			s.speedBoost = a_amount;
		}

		void ClearOffset(RE::Actor* a, FollowerState& s)
		{
			if (s.offsetMode >= 0) {
				Papyrus::ClearOffset(a);
				s.offsetMode = -1;
				s.offsetSlot = -1;
			}
		}

		void ApplyOffset(RE::Actor* a, FollowerState& s, RE::PlayerCharacter* a_player, int a_mode, int a_slot)
		{
			if (s.offsetMode == a_mode && s.offsetSlot == a_slot) return;
			auto& t = kTuning[a_mode];
			float side = a_slot == 0 ? 0.0f : (a_slot % 2 ? 1.0f : -1.0f) * t.side * static_cast<float>((a_slot + 1) / 2);
			float back = -(t.back + 50.0f * static_cast<float>((a_slot + 1) / 2));
			Papyrus::KeepOffset(a, a_player, side, back, t.catchUpRadius, t.followRadius);
			s.offsetMode = a_mode;
			s.offsetSlot = a_slot;
		}

		void StartCombat(RE::Actor* a, FollowerState& s, RE::Actor* a_target, std::string_view a_why)
		{
			if (!Alive(a_target) || CombatTarget(a) == a_target) return;
			ClearOffset(a, s);
			s.lastRetarget = g_time;
			s.missedSwings = 0;
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
		RE::Actor* PickEnemy(RE::Actor* a, const Scan& a_scan, RE::Actor* a_exclude = nullptr)
		{
			RE::Actor* best = nullptr;
			float      bestScore = std::numeric_limits<float>::max();
			for (auto enemy : a_scan.enemies) {
				if (enemy == a_exclude || !TargetsParty(enemy, a_scan)) continue;
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
					Debug("Retreat!");
				}
			} else {
				auto  old = sampleAgo(1.5f);
				float moved = old ? now.GetDistance(old->playerPos) : 1000.0f;
				bool  stopped = old && g_time - old->time > 1.2f && moved < 60.0f;
				if (g_lastPlayerAttack > g_retreatStart || healing || stopped || nearest > 4000.0f) {
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
		void UpdateFollower(RE::Actor* f, int a_slot, const Scan& a_scan)
		{
			auto& cfg = Settings::Get();
			auto  player = a_scan.player;
			auto& s = g_state[f->GetFormID()];
			int   mode = std::clamp(cfg.followDistance, 0, 2);
			auto& tune = kTuning[mode];
			float dist = Dist(f, player);
			bool  inCombat = f->IsInCombat();
			bool  waiting = IsWaiting(f);
			auto  state = f->AsActorState();
			bool  busy = waiting || f->IsOnMount() || player->IsOnMount() ||
			            state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal;
			bool  sameSpace = f->GetWorldspace() == player->GetWorldspace() &&
			                 (player->GetWorldspace() || f->GetParentCell() == player->GetParentCell());

			// ---- retreat: disengage
			bool disengage = false;
			if (g_retreating && !waiting) {
				bool rangedClose = IsRanged(f) && dist < cfg.rangedRetreatDistance;
				disengage = !rangedClose;
				if (disengage && inCombat && g_time - s.lastStop > 0.5f) {
					Papyrus::StopCombat(f);
					s.lastStop = g_time;
					inCombat = false;
				}
			}

			// ---- keep-offset follow (out of combat)
			bool wantOffset = !inCombat && !busy && sameSpace && dist < 4000.0f &&
			                  (IsFollowing(f) || s.offsetMode >= 0) && (mode != 2 || disengage);
			if (wantOffset) {
				ApplyOffset(f, s, player, disengage ? 0 : mode, a_slot);
			} else {
				ClearOffset(f, s);
			}

			// ---- catch-up speed and teleport
			bool travelling = (!inCombat || disengage) && !busy && sameSpace;
			if (cfg.catchUp && travelling && dist > tune.boostDistance) {
				SetBoost(f, s, kSpeedBoost);
			} else if (s.speedBoost != 0.0f && (!travelling || dist < tune.boostDistance * 0.6f)) {
				SetBoost(f, s, 0.0f);
			}

			if (cfg.teleport && travelling && !waiting && dist > tune.teleportDistance) {
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

			if (!inCombat || disengage) {
				s.missedSwings = 0;
				s.stuckSince = -1.0f;
				// join the fight when the player is fighting and nobody told us
				if (!disengage && !waiting && !g_retreating && cfg.awareness && player->IsInCombat()) {
					if (auto enemy = PickEnemy(f, a_scan); enemy && Dist(enemy, player) < 2500.0f && g_time - s.lastRetarget > 1.0f) {
						StartCombat(f, s, enemy, "joining your fight");
					}
				}
				return;
			}

			auto target = CombatTarget(f);

			// ---- retarget when the target is gone
			if (cfg.retarget && !Alive(target) && g_time - s.lastRetarget > 0.5f) {
				if (auto next = PickEnemy(f, a_scan)) StartCombat(f, s, next, "next enemy");
				return;
			}
			if (!target) return;

			// ---- leash: don't chase enemies far away from the player
			if (cfg.combatLeash && dist > tune.combatLeash && Dist(target, player) > tune.combatLeash * 0.8f &&
				g_time - s.lastLeash > 6.0f) {
				s.lastLeash = g_time;
				if (auto closer = PickEnemy(f, a_scan); closer && Dist(closer, player) < tune.combatLeash) {
					StartCombat(f, s, closer, "too far from you");
				} else {
					Papyrus::StopCombat(f);
					Debug(std::format("{} falls back to you", Name(f)));
				}
				return;
			}

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
				s.stuckSince = -1.0f;
				if (attacker && Alive(attacker) && attacker != target && g_time - s.lastAttackedTime < 4.0f) {
					StartCombat(f, s, attacker, "can't reach target, fighting attacker");
				} else {
					Papyrus::StopCombat(f);
					Papyrus::StartCombat(f, target);
					Debug(std::format("{} can't reach {}, re-pathing", Name(f), Name(target)));
				}
			}
		}

		void RunLogic(RE::PlayerCharacter* a_player)
		{
			auto scan = DoScan(a_player);

			std::vector<QueuedEvent> events;
			{
				std::scoped_lock lock{ g_lock };
				events.swap(g_queue);
			}
			for (auto& e : events) HandleEvent(e, scan);

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
					Queue(EventType::kPlayerHeal, {});
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

		auto holder = RE::ScriptEventSourceHolder::GetSingleton();
		holder->AddEventSink<RE::TESHitEvent>(Sink::Get());
		holder->AddEventSink<RE::TESDeathEvent>(Sink::Get());
		holder->AddEventSink<RE::TESMagicEffectApplyEvent>(Sink::Get());
		SKSE::GetActionEventSource()->AddEventSink(Sink::Get());
		logger::info("Hooks and event sinks installed");
	}

	void Reset()
	{
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

		// undo speed boosts that were saved into this game
		for (auto& [id, amount] : g_pendingBoostUndo) {
			if (auto actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
				actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kSpeedMult, -amount);
				logger::info("Removed leftover speed boost from {}", Name(actor));
			}
		}
		g_pendingBoostUndo.clear();
	}

	void Tick(float a_delta)
	{
		g_time += a_delta;
		g_logicTimer += a_delta;
		if (g_logicTimer < kLogicInterval) return;
		g_logicTimer = 0.0f;

		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) return;
		RunLogic(player);
	}

	void OnSave(SKSE::SerializationInterface* a_intfc)
	{
		std::vector<std::pair<RE::FormID, float>> boosts;
		for (auto& [id, s] : g_state) {
			if (s.speedBoost != 0.0f) boosts.emplace_back(id, s.speedBoost);
		}
		if (!a_intfc->OpenRecord(kRecordBoosts, 1)) return;
		auto count = static_cast<std::uint32_t>(boosts.size());
		a_intfc->WriteRecordData(count);
		for (auto& [id, amount] : boosts) {
			a_intfc->WriteRecordData(id);
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
				RE::FormID id = 0, resolved = 0;
				float      amount = 0.0f;
				a_intfc->ReadRecordData(id);
				a_intfc->ReadRecordData(amount);
				if (a_intfc->ResolveFormID(id, resolved)) g_pendingBoostUndo[resolved] = amount;
			}
		}
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		g_pendingBoostUndo.clear();
	}
}
