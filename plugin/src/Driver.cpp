#include "Driver.h"
#include "Bodies.h"

namespace Driver
{
	namespace
	{
		// ------------------------------------------------------------------ animation channels
		// The locomotion graph reads an actor's speed, move direction and turn through "channels"
		// (BSTAnimationGraphDataChannel<Actor, float, ...>). Their update function (vfunc 1) writes the
		// value the graph will read; for a driven body we overwrite it afterwards.
		constexpr std::uintptr_t kChannelValue = 0x18;
		constexpr std::uintptr_t kChannelActor = 0x28;
		constexpr int            kSlots = 16;
		constexpr int            kFreshFrames = 2;

		struct Slot
		{
			std::atomic<std::uintptr_t> actor{ 0 };
			std::atomic<float>          speed{ 0.0f };
			std::atomic<float>          direction{ 0.0f };
			std::atomic<int>            frame{ -100 };
		};
		Slot             g_slots[kSlots];
		std::atomic<int> g_frame{ 0 };

		// direction values seen on actors we don't drive, to learn the channel's unit
		std::atomic<float> g_dirSeenMin{ 0.0f };
		std::atomic<float> g_dirSeenMax{ 0.0f };

		Slot* FindSlot(std::uintptr_t a_actor)
		{
			for (auto& slot : g_slots) {
				if (slot.actor.load(std::memory_order_acquire) == a_actor) return &slot;
			}
			return nullptr;
		}

		Slot* ClaimSlot(std::uintptr_t a_actor)
		{
			if (auto slot = FindSlot(a_actor)) return slot;
			for (auto& slot : g_slots) {
				std::uintptr_t empty = 0;
				if (slot.actor.compare_exchange_strong(empty, a_actor)) return &slot;
			}
			return nullptr;
		}

		void ReleaseSlot(std::uintptr_t a_actor)
		{
			if (auto slot = FindSlot(a_actor)) {
				slot->frame.store(-100);
				slot->actor.store(0, std::memory_order_release);
			}
		}

		enum class Channel
		{
			kSpeed,
			kDirection,
			kTurn
		};

		template <Channel C>
		struct ChannelHook
		{
			using Fn = void (*)(void*, bool);
			static inline Fn original{ nullptr };

			// animation threads
			static void Thunk(void* a_this, bool a_arg)
			{
				original(a_this, a_arg);
				if (!a_this) return;
				auto  self = static_cast<std::uint8_t*>(a_this);
				auto  actor = *reinterpret_cast<std::uintptr_t*>(self + kChannelActor);
				auto& value = *reinterpret_cast<float*>(self + kChannelValue);
				auto  slot = actor ? FindSlot(actor) : nullptr;
				if (!slot || g_frame.load(std::memory_order_relaxed) - slot->frame.load(std::memory_order_relaxed) > kFreshFrames) {
					if constexpr (C == Channel::kDirection) {
						if (std::isfinite(value)) {
							if (value < g_dirSeenMin.load(std::memory_order_relaxed)) g_dirSeenMin.store(value, std::memory_order_relaxed);
							if (value > g_dirSeenMax.load(std::memory_order_relaxed)) g_dirSeenMax.store(value, std::memory_order_relaxed);
						}
					}
					return;
				}
				if constexpr (C == Channel::kSpeed) value = slot->speed.load(std::memory_order_relaxed);
				if constexpr (C == Channel::kDirection) value = slot->direction.load(std::memory_order_relaxed);
				if constexpr (C == Channel::kTurn) value = 0.0f;
			}
		};

		// ------------------------------------------------------------------ hold off the AI
		std::mutex                    g_drivenLock;
		std::unordered_set<RE::FormID> g_drivenIds;  // read from AI threads

		bool IsDrivenId(RE::FormID a_id)
		{
			std::scoped_lock lock{ g_drivenLock };
			return g_drivenIds.contains(a_id);
		}

		// Only attacks and blocks are held off; drawing, sheathing and swapping weapons still go through.
		bool IsAttackAction(RE::FormID a_action)
		{
			switch (a_action) {
			case 0x13005:  // ActionRightAttack
			case 0x13004:  // ActionLeftAttack
			case 0x50C96:  // ActionDualAttack
			case 0x13383:  // ActionRightPowerAttack
			case 0x2E2F6:  // ActionLeftPowerAttack
			case 0x2E2F7:  // ActionDualPowerAttack
			case 0x193CE:  // ActionBlockAnticipate
			case 0x13AF4:  // ActionBlockHit
				return true;
			default:
				return false;
			}
		}

		struct ActionHook
		{
			using Fn = bool (*)(RE::TESActionData*);
			static inline Fn original{ nullptr };

			// whichever thread runs the actor's AI: no swings or blocks of its own while we drive it
			static bool Thunk(RE::TESActionData* a_this)
			{
				if (a_this && a_this->source && a_this->action && IsAttackAction(a_this->action->GetFormID()) &&
					IsDrivenId(a_this->source->GetFormID())) {
					return false;
				}
				return original(a_this);
			}
		};

		// While a body is driven, the AI's own graph events that would undo the drive are refused:
		// "stop moving" (legs back to idle under a moving body) and attack/block starts.
		thread_local bool g_ownEvent = false;

		bool RefusedWhileDriven(const char* a_event)
		{
			if (!a_event) return false;
			std::string_view e{ a_event };
			auto starts = [&](std::string_view p) {
				return e.size() >= p.size() && std::equal(p.begin(), p.end(), e.begin(), [](char a, char b) {
					return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
				});
			};
			// idles (looking around, stretching...) put the legs in a standing pose under a moving body
			bool idle = starts("Idle") && !starts("IdleStop") && !starts("IdleForceDefaultState");
			return idle || starts("moveStop") || starts("attackStart") || starts("attackPowerStart") || starts("bowAttackStart") ||
			       starts("blockStart") || starts("bashStart") || starts("turnStop") || starts("SprintStop");
		}

		struct GraphHook
		{
			using Fn = bool (*)(RE::IAnimationGraphManagerHolder*, const RE::BSFixedString&);
			static inline Fn original{ nullptr };

			static bool Thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
			{
				if (a_this && !g_ownEvent) {
					auto actor = static_cast<RE::Character*>(a_this);
					if (IsDrivenId(actor->GetFormID()) && RefusedWhileDriven(a_event.c_str())) return false;
				}
				return original(a_this, a_event);
			}
		};

		bool Send(RE::Actor* a_actor, std::string_view a_event)
		{
			g_ownEvent = true;
			bool took = a_actor->NotifyAnimationGraph(a_event);
			g_ownEvent = false;
			return took;
		}

		// ------------------------------------------------------------------ drives
		struct Drive
		{
			Order       order;
			float       elapsed = 0.0f;
			float       nextFlip = 0.0f;
			int         side = 1;
			bool        moveSent = false;
			bool        sprinting = false;
			float       lastSprintKick = 0.0f;
			int         sprintKicks = 0;
			bool        hasLastTarget = false;
			RE::NiPoint3 lastTargetPos;
			float       speedNow = -1.0f;  // step back: this frame's speed (gap keeping)
			bool        wantSprint = false;  // formation: sprint this frame
			bool        sprintLogged = false;
			float       nextPick = 0.0f;
			float       pickAngle = 0.0f;
			float       pickRange = 200.0f;
			float       lastSpeed = 0.0f;
			bool        hopStarted = false;
			bool        idleCleared = false;
			bool        stopSent = false;      // legs sent to the standing pose while standing in place
			float       standingSince = 0.0f;
			int         stepMode = 0;          // step back: 0 diagonal back, 1 sideways
			int         stepTries = 0;         // step back: other ways out tried after a wall
			// the facing this drive gives the body: kept here, not read back from the actor, so the AI
			// turning her each frame (toward its own goal) can't drag her off the way she runs
			float       facing = 0.0f;
			bool        hasFacing = false;
			std::size_t pathIndex = 0;
			float       zoneHeading = 0.0f;
			bool        wasInZone = false;
			float       stillFor = 0.0f;
			bool        zoneHeadingSet = false;
			bool        hopFalling = false;
			RE::NiPoint3 hopFrom;
			int         lastWeaponState = -1;
			float       lastMoveKick = 0.0f;
			float       nextSightCheck = 0.0f;  // formation: can she see the player from where she is?
			bool        seesTarget = true;
			float       moveRetryAt = 0.0f;     // the graph refused moveStart (mid-attack recovery): try again soon
			int         moveRefusals = 0;
			bool        standing = false;    // formation: legs were stopped, restart them when moving again
		};
		std::unordered_map<RE::FormID, Drive>        g_drives;
		std::unordered_map<RE::FormID, std::string>  g_lastEnd;

		float Heading(const RE::NiPoint3& a_dir) { return std::atan2(a_dir.x, a_dir.y); }

		// Someone who steps aside (or gets pushed): a living actor. A corpse is just in the way.
		bool Living(RE::TESObjectREFR* a_ref)
		{
			auto actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			return actor && !actor->IsDead();
		}

		// A body in the way of a drive: shove it aside (as a player's walk does) and keep going.
		bool ShoveBody(RE::Actor* a, RE::TESObjectREFR* a_ref)
		{
			auto body = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			if (!body || !body->IsDead()) return false;
			Bodies::Kick(a, body);
			return true;
		}

		constexpr float kDeg = 0.01745329f;

		// a point at angle (degrees, 0 = target's forward, + = right) and range from the target
		RE::NiPoint3 Polar(RE::Actor* a_target, float a_angle, float a_range, float a_heading = 999.0f)
		{
			float z = (a_heading == 999.0f ? a_target->GetAngleZ() : a_heading) + a_angle * kDeg;
			return a_target->GetPosition() + RE::NiPoint3{ std::sin(z), std::cos(z), 0.0f } * a_range;
		}

		// the area's centre angle for this follower (side placement mirrors to the follower's side)
		float ZoneCentre(const Order& a_order, float a_currentAngle)
		{
			if (!a_order.zoneMirror) return a_order.zoneAngle;
			return a_currentAngle < 0.0f ? -a_order.zoneAngle : a_order.zoneAngle;
		}

		void ToPolar(RE::Actor* a_actor, RE::Actor* a_target, float& a_angle, float& a_range, float a_heading = 999.0f)
		{
			RE::NiPoint3 rel = a_actor->GetPosition() - a_target->GetPosition();
			rel.z = 0.0f;
			a_range = rel.Length();
			float tz = a_heading == 999.0f ? a_target->GetAngleZ() : a_heading;
			float fwd = rel.x * std::sin(tz) + rel.y * std::cos(tz);
			float right = rel.x * std::cos(tz) - rel.y * std::sin(tz);
			a_angle = std::atan2(right, fwd) / kDeg;
		}

		float AngleDiff(float a_a, float a_b)
		{
			float d = std::fmod(a_a - a_b + 540.0f, 360.0f) - 180.0f;
			return d;
		}

		float Wrap(float a_angle)
		{
			constexpr float kTwoPi = 6.2831853f;
			a_angle = std::fmod(a_angle + 3.1415927f, kTwoPi);
			if (a_angle < 0.0f) a_angle += kTwoPi;
			return a_angle - 3.1415927f;
		}

		float TurnToward(float a_from, float a_to, float a_max)
		{
			float diff = Wrap(a_to - a_from);
			return a_from + std::clamp(diff, -a_max, a_max);
		}

		struct Hit
		{
			bool         hit = false;
			RE::NiPoint3 point;
			RE::TESObjectREFR* ref = nullptr;
		};

		Hit Cast(RE::Actor* a_actor, RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
		{
			Hit out;
			std::uint32_t filter = 0;
			a_actor->GetCollisionFilterInfo(filter);
			filter = (filter & 0xFFFF0000u) | static_cast<std::uint32_t>(RE::COL_LAYER::kCharController);
			float           scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick{};
			pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
			pick.rayInput.filterInfo = filter;
			a_world->PickObject(pick);
			if (!pick.rayOutput.HasHit()) return out;
			out.hit = true;
			out.point = a_from + (a_to - a_from) * pick.rayOutput.hitFraction;
			out.ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
			return out;
		}

		void SetDriven(RE::FormID a_id, bool a_on)
		{
			std::scoped_lock lock{ g_drivenLock };
			if (a_on) {
				g_drivenIds.insert(a_id);
			} else {
				g_drivenIds.erase(a_id);
			}
		}

		void End(RE::Actor* a_actor, Drive& a_drive, std::string_view a_why)
		{
			auto id = a_actor->GetFormID();
			bool graphSprint = false;
			a_actor->GetGraphVariableBool("IsSprinting", graphSprint);
			// also a sprint the game started before the drive: its SprintStop was refused while driven
			if (a_drive.sprinting || graphSprint || a_actor->AsActorState()->IsSprinting()) {
				a_actor->AsActorState()->actorState1.sprinting = 0;
				a_actor->SetGraphVariableBool("IsSprinting", false);
				Send(a_actor, "SprintStop"sv);
			}
			ReleaseSlot(reinterpret_cast<std::uintptr_t>(a_actor));
			SetDriven(id, false);
			// While driven, the AI's own "stop moving" was refused: stop the legs now, or the graph stays
			// in its moving state at no speed (drifting in place, no idles, no furniture).
			if (a_drive.order.kind != Kind::kHop) Send(a_actor, "moveStop"sv);
			g_lastEnd[id] = std::string{ a_why };
			logger::info("{}: drive ended ({}, {:.1f}s{})", a_actor->GetDisplayFullName(), a_why, a_drive.elapsed,
				a_drive.sprintKicks ? std::format(", sprint restarted {}x", a_drive.sprintKicks) : std::string{});
		}

		// A jump along an arc from where the hop started to the landing point, with the graph's own
		// jump, fall and landing animations. The body is placed every frame; no fall damage.
		const char* Hop(RE::Actor* a, Drive& d, RE::bhkCharacterController* a_controller, float a_delta)
		{
			if (!d.hopStarted) {
				d.hopStarted = true;
				d.hopFrom = a->GetPosition();
				RE::NiPoint3 flat = d.order.landing - d.hopFrom;
				flat.z = 0.0f;
				float run = flat.Length();
				d.order.duration = std::clamp(run / 420.0f, 0.55f, 1.2f);
				bool moving = a->IsMoving();
				Send(a, moving ? "JumpDirectionalStart"sv : "JumpStandingStart"sv);
				d.elapsed = 0.0f;
				logger::info("{}: hop {:.0f} across, {:+.0f} up", a->GetDisplayFullName(), run, d.order.landing.z - d.hopFrom.z);
			}
			float dt = std::min(a_delta, 0.05f);
			d.elapsed += dt;
			float t = std::min(1.0f, d.elapsed / d.order.duration);
			float rise = d.order.landing.z - d.hopFrom.z;
			float apex = 45.0f + std::max(0.0f, rise) * 0.35f;
			RE::NiPoint3 p = d.hopFrom + (d.order.landing - d.hopFrom) * t;
			p.z = d.hopFrom.z + rise * t + apex * 4.0f * t * (1.0f - t);
			RE::NiPoint3 dir = d.order.landing - d.hopFrom;
			dir.z = 0.0f;
			float len = dir.Length();
			if (len > 1.0f) a->SetRotationZ(TurnToward(a->GetAngleZ(), Heading(dir / len), 10.0f * dt));
			a->SetPosition(p, true);
			float scale = RE::bhkWorld::GetWorldScale();
			float horizontal = len / d.order.duration;
			RE::NiPoint3 v = len > 1.0f ? dir / len * horizontal : RE::NiPoint3{};
			float vz = (rise + apex * 4.0f * (1.0f - 2.0f * t)) / d.order.duration;
			a_controller->context.currentState = t < 1.0f ? RE::hkpCharacterStateType::kInAir : RE::hkpCharacterStateType::kOnGround;
			a_controller->SetLinearVelocityImpl(RE::hkVector4(v.x * scale, v.y * scale, vz * scale, 0.0f));
			a_controller->fallStartHeight = p.z;
			a_controller->fallTime = 0.0f;
			if (t > 0.5f && !d.hopFalling) {
				d.hopFalling = true;
				Send(a, "JumpFall"sv);
			}
			if (t >= 1.0f) {
				Send(a, horizontal > 150.0f ? "JumpLandDirectional"sv : "JumpLand"sv);
				return "landed";
			}
			return nullptr;
		}

		// Run through the order's points in turn (they lie on walkable ground: a route someone walked).
		// The sprint animation while a drive wants it (and ending it when not): the graph's sprint state
		// has to be kept alive by hand, since the game clears it for an actor it doesn't move itself.
		void Sprint(RE::Actor* a, Drive& d, bool a_want)
		{
			auto state = a->AsActorState();
			if (d.sprinting && !a_want) {
				state->actorState1.sprinting = 0;
				a->SetGraphVariableBool("IsSprinting", false);
				Send(a, "SprintStop"sv);
				d.sprinting = false;
				return;
			}
			if (!a_want) {
				// a sprint the game started before this drive: the game's SprintStop is refused while
				// driven, so it would never end (sprinting on the spot)
				bool graphSprint = false;
				a->GetGraphVariableBool("IsSprinting", graphSprint);
				if (graphSprint || state->IsSprinting()) {
					state->actorState1.sprinting = 0;
					a->SetGraphVariableBool("IsSprinting", false);
					Send(a, "SprintStop"sv);
				}
				return;
			}
			if (!d.sprinting) {
				state->actorState1.sprinting = 1;
				bool flag = a->SetGraphVariableBool("IsSprinting", true);
				bool took = Send(a, "SprintStart"sv);
				bool readBack = false;
				a->GetGraphVariableBool("IsSprinting", readBack);
				d.sprinting = true;
				if (!d.sprintLogged) {
					logger::info("{}: sprint: IsSprinting variable {}, read back {}, SprintStart event {}", a->GetDisplayFullName(),
						flag ? "set" : "not in this graph", readBack, took ? "taken" : "refused");
					d.sprintLogged = true;
				}
				return;
			}
			// the game clears the sprint state on its own: keep it, and restart the sprint if the graph left it
			state->actorState1.sprinting = 1;
			bool still = false;
			a->GetGraphVariableBool("IsSprinting", still);
			if (!still && d.elapsed - d.lastSprintKick > 0.3f) {
				a->SetGraphVariableBool("IsSprinting", true);
				Send(a, "SprintStart"sv);
				d.lastSprintKick = d.elapsed;
				++d.sprintKicks;
			}
		}

		const char* PathStep(RE::Actor* a, Drive& d, RE::bhkCharacterController* a_controller, RE::bhkWorld* a_world, float a_delta)
		{
			float dt = std::min(a_delta, 0.05f);
			d.elapsed += dt;
			if (d.elapsed > d.order.duration) return "time";
			auto pos = a->GetPosition();
			// skip points already reached (or passed)
			while (d.pathIndex < d.order.path.size()) {
				RE::NiPoint3 to = d.order.path[d.pathIndex] - pos;
				to.z = 0.0f;
				if (to.Length() > 45.0f) break;
				++d.pathIndex;
			}
			if (d.pathIndex >= d.order.path.size()) return "arrived";
			auto&        goal = d.order.path[d.pathIndex];
			RE::NiPoint3 to = goal - pos;
			to.z = 0.0f;
			float        len = to.Length();
			RE::NiPoint3 move = to / len;
			float        step = d.order.speed * dt;
			RE::NiPoint3 next = pos + move * std::min(step, len);
			// walls in the way: give up (the route came from someone walking it, so this is rare)
			RE::NiPoint3 chest{ 0.0f, 0.0f, 60.0f };
			auto         ahead = Cast(a, a_world, pos + chest, pos + chest + move * (step + 30.0f));
			if (ahead.hit && ahead.ref != a && !Living(ahead.ref) && !ShoveBody(a, ahead.ref)) return "blocked";
			// stay on the ground, following the route's height (stairs, slopes)
			float top = std::max(pos.z, goal.z) + 70.0f;
			auto  ground = Cast(a, a_world, RE::NiPoint3{ next.x, next.y, top }, RE::NiPoint3{ next.x, next.y, std::min(pos.z, goal.z) - 120.0f });
			if (!ground.hit) return "edge";
			next.z = ground.point.z;
			if (!d.hasFacing) {
				d.facing = a->GetAngleZ();
				d.hasFacing = true;
			}
			float heading = d.facing = TurnToward(d.facing, Heading(move), 8.0f * dt);
			a->SetPosition(next, true);
			a->SetRotationZ(heading);
			float scale = RE::bhkWorld::GetWorldScale();
			RE::NiPoint3 velocity = move * d.order.speed;
			a_controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
			a_controller->SetLinearVelocityImpl(RE::hkVector4(velocity.x * scale, velocity.y * scale, 0.0f, 0.0f));
			a_controller->fallStartHeight = next.z;
			a_controller->fallTime = 0.0f;
			if (auto slot = ClaimSlot(reinterpret_cast<std::uintptr_t>(a))) {
				slot->speed.store(d.order.speed, std::memory_order_relaxed);
				slot->direction.store(0.0f, std::memory_order_relaxed);
				slot->frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
			if (!d.moveSent || d.elapsed - d.lastMoveKick > 1.0f) {
				if (!d.moveSent) Send(a, "IdleStop"sv);  // end an idle she was standing in
				Send(a, "moveStart"sv);
				d.moveSent = true;
				d.lastMoveKick = d.elapsed;
			}
			Sprint(a, d, d.order.speed > 600.0f);  // faster than a jog: look like it
			return nullptr;
		}

		// Step back direction: 0 = diagonal back (a player's backpedal), 1 = sideways with a little back.
		RE::NiPoint3 StepDir(const RE::NiPoint3& a_toTarget, float a_weave, int a_side, int a_mode)
		{
			RE::NiPoint3 across{ -a_toTarget.y, a_toTarget.x, 0.0f };
			RE::NiPoint3 dir = a_mode == 1 ? across * static_cast<float>(a_side) - a_toTarget * 0.25f :
			                                 a_toTarget * -1.0f + across * (a_weave * static_cast<float>(a_side));
			return dir / dir.Length();
		}

		// Room to step that way: nothing at chest or knee height, and ground under the end.
		bool ClearStep(RE::Actor* a, RE::bhkWorld* a_world, const RE::NiPoint3& a_pos, const RE::NiPoint3& a_dir, float a_len)
		{
			auto solid = [&](const Hit& h) { return h.hit && h.ref != a && !(h.ref && h.ref->As<RE::Actor>()); };
			RE::NiPoint3 chest{ 0.0f, 0.0f, 60.0f }, knee{ 0.0f, 0.0f, 30.0f };
			if (solid(Cast(a, a_world, a_pos + chest, a_pos + chest + a_dir * a_len))) return false;
			if (solid(Cast(a, a_world, a_pos + knee, a_pos + knee + a_dir * a_len))) return false;
			auto end = a_pos + a_dir * a_len;
			auto ground = Cast(a, a_world, end + RE::NiPoint3{ 0.0f, 0.0f, 80.0f }, end - RE::NiPoint3{ 0.0f, 0.0f, 160.0f });
			return ground.hit && std::fabs(ground.point.z - a_pos.z) < 60.0f;
		}

		// One frame of one drive. Returns why it ends, or nullptr to go on.
		const char* Step(RE::Actor* a, Drive& d, float a_delta)
		{
			if (!a->Is3DLoaded() || a->IsDead() || a->IsInRagdollState() || a->IsInKillMove() || a->IsOnMount()) return "unavailable";
			auto state = a->AsActorState();
			if (state->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal || state->IsBleedingOut() || state->IsSwimming()) return "knocked";
			bool staggered = false;
			if (a->GetGraphVariableBool("IsStaggering", staggered) && staggered) return "staggered";
			auto controller = a->GetCharController();
			auto cell = a->GetParentCell();
			auto world = cell ? cell->GetbhkWorld() : nullptr;
			if (!controller || !world) return "no physics";

			if (d.order.kind == Kind::kHop) return Hop(a, d, controller, a_delta);
			if (d.order.kind == Kind::kPath) return PathStep(a, d, controller, world, a_delta);

			auto targetPtr = d.order.target.get();
			auto target = targetPtr.get();
			if (!target || target->IsDead()) return "target gone";

			d.elapsed += a_delta;
			if (d.elapsed > d.order.duration) return "time";

			RE::NiPoint3 pos = a->GetPosition();
			RE::NiPoint3 to = target->GetPosition() - pos;
			to.z = 0.0f;
			float dist = to.Length();
			if (dist < 1.0f && d.order.kind != Kind::kFormation) return "arrived";
			RE::NiPoint3 toDir = dist > 0.001f ? to / dist : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };

			RE::NiPoint3 move;
			float        face;
			switch (d.order.kind) {
			case Kind::kCharge:
				{
					if (dist <= d.order.stopDistance) return "arrived";
					// A player's running weave (measured): facing the target, running diagonally at 45
					// degrees, swapping sides every 0.25-0.6 s. The legs play the diagonal run that goes
					// with it. Straight in for the last stretch.
					if (d.elapsed >= d.nextFlip) {
						d.side = -d.side;
						float span = std::max(0.0f, d.order.flipMax - d.order.flipMin);
						d.nextFlip = d.elapsed + d.order.flipMin + span * static_cast<float>(std::rand() % 100) / 100.0f;
					}
					RE::NiPoint3 across{ -toDir.y, toDir.x, 0.0f };
					float        weave = dist > d.order.stopDistance + 250.0f ? d.order.weave : 0.0f;
					RE::NiPoint3 aim = toDir + across * (weave * static_cast<float>(d.side));
					move = aim / aim.Length();
					face = Heading(toDir);
					break;
				}
			case Kind::kStepBack:
				{
					auto swing = target->AsActorState()->GetAttackState();
					bool attacking = swing == RE::ATTACK_STATE_ENUM::kDraw || swing == RE::ATTACK_STATE_ENUM::kSwing ||
					                 swing == RE::ATTACK_STATE_ENUM::kHit;
					if (d.order.holdWhileAttacked && !attacking && d.elapsed > 0.2f) return "swing over";
					if (dist >= d.order.stopDistance && !d.order.holdWhileAttacked) return "clear";
					// Keep the gap: back off at the speed the attacker closes in (a little faster when inside
					// the gap), so waiting out a swing is one continuous backpedal instead of a tug of war.
					RE::NiPoint3 targetPos = target->GetPosition();
					float closing = 0.0f;
					if (d.hasLastTarget && a_delta > 0.0f) {
						RE::NiPoint3 moved = targetPos - d.lastTargetPos;
						moved.z = 0.0f;
						closing = std::max(0.0f, (moved.x * -toDir.x + moved.y * -toDir.y) / a_delta);
					}
					d.lastTargetPos = targetPos;
					d.hasLastTarget = true;
					float inside = std::max(0.0f, d.order.stopDistance - dist);
					d.speedNow = std::min(d.order.speed, closing + inside * 4.0f);
					// diagonal backpedal, as a player does it (measured ~450 units/s); sideways when a wall
					// is behind (see the wall check below)
					move = StepDir(toDir, d.order.weave, d.side, d.stepMode);
					face = Heading(toDir);
					break;
				}
			case Kind::kFormation:
				{
					// how the target (player) moves
					RE::NiPoint3 targetPos = target->GetPosition();
					RE::NiPoint3 tv{};
					if (d.hasLastTarget && a_delta > 0.0f) tv = (targetPos - d.lastTargetPos) / a_delta;
					tv.z = 0.0f;
					d.lastTargetPos = targetPos;
					d.hasLastTarget = true;
					float tSpeed = tv.Length();

					// The area follows the way the target travels. Running forward it turns with the target
					// quickly; turning on the spot, strafing or backpedalling it turns slowly, so a player
					// looking around doesn't send the follower running round them.
					float tHeading = target->GetAngleZ();
					RE::NiPoint3 tFwd{ std::sin(tHeading), std::cos(tHeading), 0.0f };
					bool forward = tSpeed > 120.0f && (tv.x * tFwd.x + tv.y * tFwd.y) / tSpeed > 0.5f;
					if (!d.zoneHeadingSet) {
						d.zoneHeading = tHeading;
						d.zoneHeadingSet = true;
					}
					d.zoneHeading = TurnToward(d.zoneHeading, tHeading, (forward ? 3.0f : 0.6f) * std::min(a_delta, 0.05f));
					float zh = d.zoneHeading;

					// looser in calm situations: a bigger area, a softer pull
					float loose = std::max(1.0f, d.order.looseness);
					float zNear = d.order.zoneNear * loose;
					float zFar = d.order.zoneFar * loose;
					float zHalf = std::min(180.0f, d.order.zoneHalf + (loose - 1.0f) * 25.0f);

					// where the follower is in the area's frame
					float angle, range;
					ToPolar(a, target, angle, range, zh);
					float centre = ZoneCentre(d.order, angle);
					// with a margin, so the edge of the area isn't a flicker between relaxing and catching up
					float margin = d.wasInZone ? 45.0f : 0.0f;
					float angleMargin = range > 1.0f ? margin / range / kDeg : 0.0f;
					bool  inZone = std::fabs(AngleDiff(angle, centre)) <= zHalf + angleMargin && range >= zNear - margin &&
					              range <= zFar + margin;
					// keeping the player in view: round a corner or behind a pillar doesn't count as in place,
					// however close (checked a few times a second)
					if (d.elapsed >= d.nextSightCheck) {
						d.nextSightCheck = d.elapsed + 0.25f;
						d.seesTarget = ClearLine(a, pos, targetPos);
					}
					if (!d.seesTarget && range > d.order.zoneNear) inZone = false;
					d.wasInZone = inZone;
					if (range > 1500.0f) return "too far";

					// a personal spot inside the area, picked again every few seconds: a little wandering
					if (d.elapsed >= d.nextPick || std::fabs(AngleDiff(d.pickAngle, centre)) > zHalf + 1.0f) {
						float r = static_cast<float>(std::rand() % 1000) / 1000.0f;
						float q = static_cast<float>(std::rand() % 1000) / 1000.0f;
						d.pickAngle = centre + (r * 2.0f - 1.0f) * zHalf * 0.8f;
						d.pickRange = zNear + (zFar - zNear) * (0.25f + q * 0.5f);
						d.nextPick = d.elapsed + 5.0f + static_cast<float>(std::rand() % 400) / 100.0f;
					}
					RE::NiPoint3 aim;
					float        gain;
					bool targetStill = tSpeed < 40.0f;
					if (inZone && targetStill) {
						// the player stands (or only turns): stay where she is
						aim = pos;
						gain = 0.0f;
						d.stillFor += a_delta;
						if (d.stillFor > 1.0f && d.lastSpeed < 40.0f) return "in place";
					} else if (inZone) {
						d.stillFor = 0.0f;
						aim = Polar(target, d.pickAngle, d.pickRange, zh);  // drift toward the personal spot
						gain = 0.5f / loose;
					} else {
						d.stillFor = 0.0f;
						// nearest point of the area
						float ca = centre + std::clamp(AngleDiff(angle, centre), -zHalf, zHalf);
						// can't see the player: closer in, to the near edge (the nearest point of the area may be
						// right where she stands, behind the pillar)
						float cr = d.seesTarget ? std::clamp(range, zNear, zFar) : zNear;
						aim = Polar(target, ca, cr, zh);
						gain = 1.4f / loose;
					}
					RE::NiPoint3 toAim = aim - pos;
					toAim.z = 0.0f;
					float gap = toAim.Length();


					// velocity: go along with the target (fully when it runs forward, partly when it strafes or
					// backpedals, less in calm situations), plus a pull toward the aim point
					float mirror = (forward ? 1.0f : 0.4f) / std::sqrt(loose);
					if (inZone && !forward) mirror *= 0.5f;
					RE::NiPoint3 want = tv * mirror + (gap > 1.0f ? toAim / gap : RE::NiPoint3{}) * std::min(gap * gain, 400.0f);
					want.z = 0.0f;
					bool  sprinting = target->AsActorState()->IsSprinting();
					// weapons drawn (looseness 1): eager, a higher top speed when behind and a quick speed-up
					bool  eager = loose <= 1.01f;
					float top = (sprinting ? d.order.sprintSpeed : d.order.speed) * (eager && !inZone ? 1.25f : 1.05f);
					float wantSpeed = std::min(want.Length(), top);
					// smooth: speed up and slow down at a person's rate, no instant matching
					float accel = (eager ? 1400.0f : 700.0f) * std::min(a_delta, 0.05f);
					float current = d.lastSpeed;
					float speedNow = std::clamp(wantSpeed, current - accel * 1.5f, current + accel);
					if (wantSpeed < 35.0f && current < 60.0f) speedNow = 0.0f;  // no slow shuffling in place
					d.lastSpeed = speedNow;
					d.speedNow = speedNow;
					move = want.Length() > 1.0f ? want / want.Length() : Polar(target, 0.0f, 1.0f, zh) - targetPos;
					move.z = 0.0f;
					if (move.Length() > 0.0f) move = move / move.Length();
					face = speedNow > 40.0f ? Heading(move) : Heading(Polar(target, 0.0f, 100.0f, zh) - pos);
					// sprint when the player does, or whenever she runs faster than a jog (catching up)
					d.wantSprint = (sprinting && speedNow > d.order.speed * 1.05f) || speedNow > (d.sprinting ? 480.0f : 560.0f);
					break;
				}
			case Kind::kStrafe:
				{
					RE::NiPoint3 toGoal = d.order.landing - pos;
					toGoal.z = 0.0f;
					float left = toGoal.Length();
					if (left < 30.0f) return "arrived";
					move = toGoal / left;
					face = Heading(toDir);  // keep the enemy in front
					d.speedNow = std::min(d.order.speed, left * 4.0f + 80.0f);
					break;
				}
			default:  // run to
				if (dist <= d.order.stopDistance) return "arrived";
				move = toDir;
				face = Heading(toDir);
				break;
			}

			float dt = std::min(a_delta, 0.05f);
			if (!d.hasFacing) {
				d.facing = a->GetAngleZ();
				d.hasFacing = true;
			}
			float heading = d.facing = TurnToward(d.facing, face, (d.order.kind == Kind::kFormation ? 4.0f : 8.0f) * dt);
			float        speed = d.speedNow >= 0.0f ? d.speedNow : d.order.speed;
			d.speedNow = -1.0f;
			float        step = speed * dt;
			RE::NiPoint3 next = pos + move * step;

			// walls, rocks and bodies in the way: hand back to the AI (and to traversal mods)
			RE::NiPoint3 chest{ 0.0f, 0.0f, 60.0f };
			auto         ahead = Cast(a, world, pos + chest, pos + chest + move * (step + 40.0f));
			if (ahead.hit && ahead.ref != a) {
				if (ahead.ref == target && d.order.kind != Kind::kFormation) return "arrived";
				// people (the player, other followers) step aside or get pushed; only walls and rocks stop a drive
				if (!Living(ahead.ref) && !ShoveBody(a, ahead.ref)) {
					// stepping out of a swing with a wall behind: the other diagonal, or sideways, as a player
					// would; give up only when boxed in
					bool found = false;
					if (d.order.kind == Kind::kStepBack && d.stepTries < 3) {
						static constexpr int kCandidates[][2] = { { -1, 0 }, { 1, 1 }, { -1, 1 } };  // {side flip, mode}
						for (auto [flip, mode] : kCandidates) {
							int  side = flip < 0 ? -d.side : d.side;
							auto dir = StepDir(toDir, d.order.weave, side, mode);
							if (side == d.side && mode == d.stepMode) continue;
							if (!ClearStep(a, world, pos, dir, 120.0f)) continue;
							d.side = side;
							d.stepMode = mode;
							move = dir;
							next = pos + move * step;
							found = true;
							break;
						}
						++d.stepTries;
					}
					if (!found) return "blocked";
				}
			}
			// stay on the ground; don't step off ledges or up big steps
			auto ground = Cast(a, world, next + RE::NiPoint3{ 0.0f, 0.0f, 80.0f }, next - RE::NiPoint3{ 0.0f, 0.0f, 160.0f });
			if (!ground.hit) return "edge";
			// a stair step down is a few units per frame even at a sprint; more than that is a ledge (she
			// ran off one at 590 u/s with the old 90-unit allowance)
			if (ground.point.z - pos.z > 45.0f || pos.z - ground.point.z > 35.0f + step) return "step";
			next.z = ground.point.z;


			a->SetPosition(next, true);
			a->SetRotationZ(heading);
			float scale = RE::bhkWorld::GetWorldScale();
			RE::NiPoint3 velocity = move * speed;
			controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
			controller->SetLinearVelocityImpl(RE::hkVector4(velocity.x * scale, velocity.y * scale, 0.0f, 0.0f));
			controller->fallStartHeight = next.z;
			controller->fallTime = 0.0f;

			if (auto slot = ClaimSlot(reinterpret_cast<std::uintptr_t>(a))) {
				slot->speed.store(speed, std::memory_order_relaxed);
				// the channel counts in turns, 0..1 (0 = forward, 0.5 = backward), not radians
				float turns = Wrap(Heading(move) - heading) / 6.2831853f;
				if (turns < 0.0f) turns += 1.0f;
				slot->direction.store(turns, std::memory_order_relaxed);
				slot->frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
			// Restart the legs when they may have dropped to a standing pose: setting off again after
			// standing, drawing or sheathing a weapon (resets the graph), and every second while moving.
			if (speed < 20.0f) {
				if (!d.standing) d.standingSince = d.elapsed;
				d.standing = true;
				// standing in her spot for a moment: legs to the standing pose (or they shuffle in place)
				if (!d.stopSent && d.elapsed - d.standingSince > 0.3f) {
					Send(a, "moveStop"sv);
					d.stopSent = true;
				}
			} else if (speed > 60.0f) {
				d.stopSent = false;
				auto weaponState = static_cast<int>(state->GetWeaponState());
				// setting off: end any idle she was playing while standing (or she slides in its pose)
				if (d.standing || !d.idleCleared) {
					Send(a, "IdleStop"sv);
					d.idleCleared = true;
				}
				if (d.standing || weaponState != d.lastWeaponState || d.elapsed - d.lastMoveKick > 1.0f) {
					d.moveSent = false;
					d.lastMoveKick = d.elapsed;
				}
				d.standing = false;
				d.lastWeaponState = weaponState;
			}
			if (!d.moveSent && d.elapsed >= d.moveRetryAt) {
				bool took = Send(a, "moveStart"sv);
				d.moveSent = true;
				if (!took) {
					// still finishing a swing or a stagger: without the legs going she glides, so keep asking
					// every tenth of a second instead of once a second
					d.moveSent = false;
					d.moveRetryAt = d.elapsed + 0.1f;
					if (++d.moveRefusals == 1 && d.order.kind != Kind::kFormation)
						logger::info("{}: moveStart refused by the graph", a->GetDisplayFullName());
				} else if (d.moveRefusals > 0) {
					if (d.order.kind != Kind::kFormation)
						logger::info("{}: legs going after {} refusals", a->GetDisplayFullName(), d.moveRefusals);
					d.moveRefusals = 0;
				}
			}
			Sprint(a, d, d.order.sprint || (d.order.kind == Kind::kFormation && d.wantSprint));
			return nullptr;
		}
	}

	bool Install()
	{
		REL::Relocation<std::uintptr_t> speed{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorSpeedChannel_[0] };
		ChannelHook<Channel::kSpeed>::original = reinterpret_cast<ChannelHook<Channel::kSpeed>::Fn>(speed.write_vfunc(1, ChannelHook<Channel::kSpeed>::Thunk));
		REL::Relocation<std::uintptr_t> direction{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorDirectionChannel_[0] };
		ChannelHook<Channel::kDirection>::original =
			reinterpret_cast<ChannelHook<Channel::kDirection>::Fn>(direction.write_vfunc(1, ChannelHook<Channel::kDirection>::Thunk));
		REL::Relocation<std::uintptr_t> turn{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorTurnDeltaChannel_[0] };
		ChannelHook<Channel::kTurn>::original = reinterpret_cast<ChannelHook<Channel::kTurn>::Fn>(turn.write_vfunc(1, ChannelHook<Channel::kTurn>::Thunk));
		REL::Relocation<std::uintptr_t> graph{ RE::VTABLE_Character[3] };
		GraphHook::original = reinterpret_cast<GraphHook::Fn>(graph.write_vfunc(1, GraphHook::Thunk));
		REL::Relocation<std::uintptr_t> action{ RE::VTABLE_TESActionData[0] };
		ActionHook::original = reinterpret_cast<ActionHook::Fn>(action.write_vfunc(5, ActionHook::Thunk));
		logger::info("Movement driver installed");
		return true;
	}

	void Uninstall()
	{
		StopAll();
		REL::Relocation<std::uintptr_t> speed{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorSpeedChannel_[0] };
		speed.write_vfunc(1, reinterpret_cast<std::uintptr_t>(ChannelHook<Channel::kSpeed>::original));
		REL::Relocation<std::uintptr_t> direction{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorDirectionChannel_[0] };
		direction.write_vfunc(1, reinterpret_cast<std::uintptr_t>(ChannelHook<Channel::kDirection>::original));
		REL::Relocation<std::uintptr_t> turn{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorTurnDeltaChannel_[0] };
		turn.write_vfunc(1, reinterpret_cast<std::uintptr_t>(ChannelHook<Channel::kTurn>::original));
		REL::Relocation<std::uintptr_t> graph{ RE::VTABLE_Character[3] };
		graph.write_vfunc(1, reinterpret_cast<std::uintptr_t>(GraphHook::original));
		REL::Relocation<std::uintptr_t> action{ RE::VTABLE_TESActionData[0] };
		action.write_vfunc(5, reinterpret_cast<std::uintptr_t>(ActionHook::original));
		logger::info("Movement driver removed");
	}

	void Frame(float a_delta)
	{
		g_frame.fetch_add(1, std::memory_order_relaxed);
		for (auto it = g_drives.begin(); it != g_drives.end();) {
			auto actor = RE::TESForm::LookupByID<RE::Actor>(it->first);
			const char* why = actor ? Step(actor, it->second, a_delta) : "gone";
			if (why) {
				if (actor) {
					End(actor, it->second, why);
				} else {
					SetDriven(it->first, false);
				}
				it = g_drives.erase(it);
			} else {
				++it;
			}
		}
		static float logTimer = 0.0f;
		logTimer += a_delta;
		if (logTimer > 30.0f) {
			logTimer = 0.0f;
			logger::info("direction channel values seen: {:.3f} .. {:.3f}", g_dirSeenMin.load(), g_dirSeenMax.load());
		}
	}

	bool Start(RE::Actor* a_actor, const Order& a_order)
	{
		if (!a_actor || a_actor->IsPlayerRef() || !a_actor->GetCharController()) return false;
		if (!ClaimSlot(reinterpret_cast<std::uintptr_t>(a_actor))) return false;
		auto& d = g_drives[a_actor->GetFormID()];
		d = Drive{};
		d.order = a_order;
		d.side = (std::rand() % 2) ? 1 : -1;
		// stepping out of a swing: pick a way with room for it (walls, pillars, clutter behind her)
		if (a_order.kind == Kind::kStepBack) {
			auto target = a_order.target.get();
			auto cell = a_actor->GetParentCell();
			auto world = cell ? cell->GetbhkWorld() : nullptr;
			if (target && world) {
				RE::NiPoint3 to = target->GetPosition() - a_actor->GetPosition();
				to.z = 0.0f;
				if (to.Length() > 1.0f) {
					to = to / to.Length();
					const std::pair<int, int> options[] = { { d.side, 0 }, { -d.side, 0 }, { d.side, 1 }, { -d.side, 1 } };
					for (auto [side, mode] : options) {
						if (ClearStep(a_actor, world, a_actor->GetPosition(), StepDir(to, a_order.weave, side, mode), 150.0f)) {
							d.side = side;
							d.stepMode = mode;
							break;
						}
					}
				}
			}
		}
		SetDriven(a_actor->GetFormID(), true);
		return true;
	}

	void Stop(RE::Actor* a_actor)
	{
		if (!a_actor) return;
		auto it = g_drives.find(a_actor->GetFormID());
		if (it == g_drives.end()) return;
		End(a_actor, it->second, "stopped");
		g_drives.erase(it);
	}

	void StopAll()
	{
		for (auto& [id, d] : g_drives) {
			if (auto actor = RE::TESForm::LookupByID<RE::Actor>(id)) End(actor, d, "reset");
		}
		g_drives.clear();
		std::scoped_lock lock{ g_drivenLock };
		g_drivenIds.clear();
	}

	bool ClearLine(RE::Actor* a_actor, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		auto cell = a_actor ? a_actor->GetParentCell() : nullptr;
		auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) return true;
		RE::NiPoint3 chest{ 0.0f, 0.0f, 60.0f };
		auto hit = Cast(a_actor, world, a_from + chest, a_to + chest);
		return !hit.hit || hit.ref == a_actor || (hit.ref && hit.ref->As<RE::Actor>());  // people and bodies move
	}

	bool GroundAt(RE::Actor* a_actor, RE::NiPoint3& a_point, float a_range)
	{
		auto cell = a_actor ? a_actor->GetParentCell() : nullptr;
		auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) return false;
		auto hit = Cast(a_actor, world, a_point + RE::NiPoint3{ 0.0f, 0.0f, a_range }, a_point - RE::NiPoint3{ 0.0f, 0.0f, a_range });
		if (!hit.hit || (hit.ref && hit.ref->Is(RE::FormType::ActorCharacter))) return false;
		a_point.z = hit.point.z;
		return true;
	}

	bool GroundAlong(RE::Actor* a_actor, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		auto cell = a_actor ? a_actor->GetParentCell() : nullptr;
		auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) return true;
		RE::NiPoint3 flat = a_to - a_from;
		flat.z = 0.0f;
		int   steps = std::max(1, static_cast<int>(flat.Length() / 60.0f));
		float top = std::max(a_from.z, a_to.z) + 70.0f;
		float low = std::min(a_from.z, a_to.z) - 120.0f;
		for (int i = 1; i <= steps; ++i) {
			auto p = a_from + (a_to - a_from) * (static_cast<float>(i) / steps);
			auto hit = Cast(a_actor, world, RE::NiPoint3{ p.x, p.y, top }, RE::NiPoint3{ p.x, p.y, low });
			if (!hit.hit) return false;
		}
		return true;
	}

	float ZoneGap(const Order& a_order, RE::Actor* a_actor, RE::Actor* a_target, RE::NiPoint3* a_nearest)
	{
		float angle, range;
		ToPolar(a_actor, a_target, angle, range);
		float centre = ZoneCentre(a_order, angle);
		float ca = centre + std::clamp(AngleDiff(angle, centre), -a_order.zoneHalf, a_order.zoneHalf);
		float cr = std::clamp(range, a_order.zoneNear, a_order.zoneFar);
		auto  nearest = Polar(a_target, ca, cr);
		if (a_nearest) *a_nearest = nearest;
		RE::NiPoint3 d = nearest - a_actor->GetPosition();
		d.z = 0.0f;
		return d.Length();
	}

	void SetLooseness(RE::Actor* a_actor, float a_looseness)
	{
		if (!a_actor) return;
		auto it = g_drives.find(a_actor->GetFormID());
		if (it != g_drives.end()) it->second.order.looseness = a_looseness;
	}

	bool IsDriving(RE::Actor* a_actor) { return a_actor && g_drives.contains(a_actor->GetFormID()); }

	const char* Dodge(RE::Actor* a_actor, RE::Actor* a_attacker, std::string_view a_prefer)
	{
		if (!a_actor || !a_attacker) return nullptr;
		auto cell = a_actor->GetParentCell();
		auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) return nullptr;
		Stop(a_actor);
		RE::NiPoint3 to = a_attacker->GetPosition() - a_actor->GetPosition();
		to.z = 0.0f;
		if (to.Length() < 1.0f) return nullptr;
		to = to / to.Length();
		// the dodge animations go relative to where she faces: face the attacker first
		a_actor->SetRotationZ(Heading(to));
		RE::NiPoint3 right{ to.y, -to.x, 0.0f };
		int          first = a_prefer == "right" ? 1 : a_prefer == "left" ? -1 : ((std::rand() % 2) ? 1 : -1);
		struct Option
		{
			RE::NiPoint3 dir;
			const char*  event;
			const char*  name;
		};
		Option options[] = {
			{ right * static_cast<float>(first), first > 0 ? "TKDodgeRight" : "TKDodgeLeft", first > 0 ? "right" : "left" },
			{ right * static_cast<float>(-first), first > 0 ? "TKDodgeLeft" : "TKDodgeRight", first > 0 ? "left" : "right" },
			{ to * -1.0f, "TKDodgeBack", "back" },
		};
		if (a_prefer == "back") std::rotate(std::begin(options), std::begin(options) + 2, std::end(options));
		for (auto& o : options) {
			if (!ClearStep(a_actor, world, a_actor->GetPosition(), o.dir, 220.0f)) continue;
			if (Send(a_actor, o.event)) return o.name;
			return nullptr;  // the graph has no such dodge (or can't now): the caller steps back instead
		}
		return nullptr;
	}

	float Room(RE::Actor* a_actor, const RE::NiPoint3& a_from, const RE::NiPoint3& a_dir, float a_max)
	{
		auto cell = a_actor ? a_actor->GetParentCell() : nullptr;
		auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) return a_max;
		float free = 0.0f;
		for (float len = 80.0f; len <= a_max + 1.0f; len += 80.0f) {
			if (!ClearStep(a_actor, world, a_from, a_dir, len)) break;
			free = len;
		}
		return free;
	}

	const char* Current(RE::Actor* a_actor)
	{
		if (!a_actor) return "";
		auto it = g_drives.find(a_actor->GetFormID());
		if (it == g_drives.end()) return "";
		switch (it->second.order.kind) {
		case Kind::kCharge: return "charge";
		case Kind::kStepBack: return "stepback";
		case Kind::kRunTo: return "runto";
		case Kind::kFormation: return "formation";
		case Kind::kHop: return "hop";
		case Kind::kPath: return "path";
		case Kind::kStrafe: return "strafe";
		}
		return "?";
	}

	const char* LastEnd(RE::Actor* a_actor)
	{
		if (!a_actor) return "";
		auto it = g_lastEnd.find(a_actor->GetFormID());
		return it == g_lastEnd.end() ? "" : it->second.c_str();
	}
}
