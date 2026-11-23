#include "Driver.h"

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

		struct ActionHook
		{
			using Fn = bool (*)(RE::TESActionData*);
			static inline Fn original{ nullptr };

			// whichever thread runs the actor's AI: no swings or blocks of its own while we drive it
			static bool Thunk(RE::TESActionData* a_this)
			{
				if (a_this && a_this->source && IsDrivenId(a_this->source->GetFormID())) return false;
				return original(a_this);
			}
		};

		// ------------------------------------------------------------------ drives
		struct Drive
		{
			Order       order;
			float       elapsed = 0.0f;
			float       nextFlip = 0.0f;
			int         side = 1;
			bool        moveSent = false;
			bool        sprinting = false;
		};
		std::unordered_map<RE::FormID, Drive>        g_drives;
		std::unordered_map<RE::FormID, std::string>  g_lastEnd;

		float Heading(const RE::NiPoint3& a_dir) { return std::atan2(a_dir.x, a_dir.y); }

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
			if (a_drive.sprinting) {
				a_actor->AsActorState()->actorState1.sprinting = 0;
				a_actor->NotifyAnimationGraph("SprintStop"sv);
			}
			ReleaseSlot(reinterpret_cast<std::uintptr_t>(a_actor));
			SetDriven(id, false);
			g_lastEnd[id] = std::string{ a_why };
			logger::info("{}: drive ended ({}, {:.1f}s)", a_actor->GetDisplayFullName(), a_why, a_drive.elapsed);
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

			auto targetPtr = d.order.target.get();
			auto target = targetPtr.get();
			if (!target || target->IsDead()) return "target gone";

			d.elapsed += a_delta;
			if (d.elapsed > d.order.duration) return "time";

			RE::NiPoint3 pos = a->GetPosition();
			RE::NiPoint3 to = target->GetPosition() - pos;
			to.z = 0.0f;
			float dist = to.Length();
			if (dist < 1.0f) return "arrived";
			RE::NiPoint3 toDir = to / dist;

			RE::NiPoint3 move;
			float        face;
			switch (d.order.kind) {
			case Kind::kCharge:
				{
					if (dist <= d.order.stopDistance) return "arrived";
					if (d.elapsed >= d.nextFlip) {
						d.side = -d.side;
						d.nextFlip = d.elapsed + 0.6f + static_cast<float>(std::rand() % 40) / 100.0f;
					}
					RE::NiPoint3 across{ -toDir.y, toDir.x, 0.0f };
					// weave less as the target gets close
					float        weave = std::min(1.0f, dist / 1200.0f) * 0.9f;
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
					if (dist >= d.order.stopDistance) {
						if (!d.order.holdWhileAttacked) return "clear";
						// out of reach: wait there, facing the attacker, until its swing is over
						a->SetRotationZ(TurnToward(a->GetAngleZ(), Heading(toDir), 8.0f * std::min(a_delta, 0.05f)));
						if (auto slot = ClaimSlot(reinterpret_cast<std::uintptr_t>(a))) {
							slot->speed.store(0.0f, std::memory_order_relaxed);
							slot->direction.store(0.0f, std::memory_order_relaxed);
							slot->frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
						}
						return nullptr;
					}
					move = toDir * -1.0f;
					face = Heading(toDir);
					break;
				}
			default:  // run to
				if (dist <= d.order.stopDistance) return "arrived";
				move = toDir;
				face = Heading(toDir);
				break;
			}

			float        dt = std::min(a_delta, 0.05f);
			float        step = d.order.speed * dt;
			RE::NiPoint3 next = pos + move * step;

			// walls, rocks and bodies in the way: hand back to the AI (and to traversal mods)
			RE::NiPoint3 chest{ 0.0f, 0.0f, 60.0f };
			auto         ahead = Cast(a, world, pos + chest, pos + chest + move * (step + 40.0f));
			if (ahead.hit && ahead.ref != a) {
				if (ahead.ref == target) return "arrived";
				return "blocked";
			}
			// stay on the ground; don't step off ledges or up big steps
			auto ground = Cast(a, world, next + RE::NiPoint3{ 0.0f, 0.0f, 80.0f }, next - RE::NiPoint3{ 0.0f, 0.0f, 160.0f });
			if (!ground.hit) return "edge";
			if (ground.point.z - pos.z > 45.0f || pos.z - ground.point.z > 90.0f) return "step";
			next.z = ground.point.z;

			float heading = TurnToward(a->GetAngleZ(), face, 8.0f * dt);
			a->SetPosition(next, true);
			a->SetRotationZ(heading);
			float scale = RE::bhkWorld::GetWorldScale();
			RE::NiPoint3 velocity = move * d.order.speed;
			controller->context.currentState = RE::hkpCharacterStateType::kOnGround;
			controller->SetLinearVelocityImpl(RE::hkVector4(velocity.x * scale, velocity.y * scale, 0.0f, 0.0f));
			controller->fallStartHeight = next.z;
			controller->fallTime = 0.0f;

			if (auto slot = ClaimSlot(reinterpret_cast<std::uintptr_t>(a))) {
				slot->speed.store(d.order.speed, std::memory_order_relaxed);
				// the channel counts in turns, 0..1 (0 = forward, 0.5 = backward), not radians
				float turns = Wrap(Heading(move) - heading) / 6.2831853f;
				if (turns < 0.0f) turns += 1.0f;
				slot->direction.store(turns, std::memory_order_relaxed);
				slot->frame.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
			}
			if (!d.moveSent) {
				a->NotifyAnimationGraph("moveStart"sv);
				d.moveSent = true;
			}
			if (d.order.sprint && !d.sprinting) {
				state->actorState1.sprinting = 1;
				a->NotifyAnimationGraph("SprintStart"sv);
				d.sprinting = true;
			}
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
		REL::Relocation<std::uintptr_t> action{ RE::VTABLE_TESActionData[0] };
		ActionHook::original = reinterpret_cast<ActionHook::Fn>(action.write_vfunc(5, ActionHook::Thunk));
		logger::info("Movement driver installed");
		return true;
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

	bool IsDriving(RE::Actor* a_actor) { return a_actor && g_drives.contains(a_actor->GetFormID()); }

	const char* LastEnd(RE::Actor* a_actor)
	{
		if (!a_actor) return "";
		auto it = g_lastEnd.find(a_actor->GetFormID());
		return it == g_lastEnd.end() ? "" : it->second.c_str();
	}
}
