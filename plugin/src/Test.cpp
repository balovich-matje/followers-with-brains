#include "Test.h"
#include "Followers.h"
#include "Papyrus.h"

namespace Test
{
	namespace
	{
		const std::filesystem::path kConfig{ "Data/SKSE/Plugins/FollowersWithBrains_Test.ini" };

		struct Spawn
		{
			RE::FormID base;
			float      forward;
			float      side;
		};

		// Base actors from Skyrim.esm (leveled to the player like any bandit).
		constexpr RE::FormID kBanditMelee = 0x1BCD8;   // EncBandit01Melee1HNordM
		constexpr RE::FormID kBanditArcher = 0x37C00;  // EncBandit01MissileNordM

		std::vector<Spawn> Scenario(std::string_view a_name)
		{
			if (a_name == "melee_rush") return { { kBanditMelee, 1500.0f, -150.0f }, { kBanditMelee, 1600.0f, 150.0f } };
			if (a_name == "duel") return { { kBanditMelee, 900.0f, 0.0f } };
			if (a_name == "archers") return { { kBanditArcher, 2200.0f, -300.0f }, { kBanditArcher, 2300.0f, 300.0f } };
			// mixed: one melee charging, two archers behind
			return { { kBanditMelee, 1400.0f, 0.0f }, { kBanditArcher, 2200.0f, -400.0f }, { kBanditArcher, 2300.0f, 400.0f } };
		}

		struct Config
		{
			bool        active = false;
			std::string save = "FWBTest";
			std::string scenario = "mixed";
			int         runs = 3;
			float       duration = 60.0f;
			float       enemyHealth = 1500.0f;  // extra health for spawned enemies, so fights last
		} g_cfg;

		enum class Phase
		{
			kIdle,
			kWaitMenu,
			kLoading,
			kSettle,
			kRunning,
			kCooldown,
			kDone
		};

		Phase g_phase = Phase::kIdle;
		float g_timer = 0.0f;
		int   g_run = 0;
		bool  g_menuSeen = false;
		std::vector<RE::ActorHandle> g_spawned;
		struct Placement
		{
			float distance;
			float angle;     // around the player, relative to their facing
			int   attempts = 0;
		};
		std::vector<Placement> g_placements;  // parallel to g_spawned
		int   g_relocated = 0;
		float g_waterCheck = 0.0f;

		void Place(RE::Actor* a_actor, const Placement& a_place)
		{
			auto  player = RE::PlayerCharacter::GetSingleton();
			float z = player->GetAngleZ() + a_place.angle;
			Papyrus::MoveTo(a_actor, player, std::sin(z) * a_place.distance, std::cos(z) * a_place.distance);
		}

		// Bandits that land in water can't be reached: move them around the player until they are dry.
		void FixWater()
		{
			for (std::size_t i = 0; i < g_spawned.size(); ++i) {
				auto ptr = g_spawned[i].get();
				auto a = ptr.get();
				if (!a || a->IsDead()) continue;
				if (!a->IsInWater() && !a->AsActorState()->IsSwimming()) continue;
				auto& place = g_placements[i];
				if (++place.attempts > 7) continue;
				place.angle += 0.785f;  // 45 degrees further round
				Place(a, place);
				++g_relocated;
				logger::info("[test] {} was in water, moved (try {})", a->GetDisplayFullName(), place.attempts);
			}
		}
		std::ofstream g_trace;
		std::ofstream g_report;

		// per-run numbers
		float g_firstDamage = -1.0f;
		float g_meleeNearTime = 0.0f;   // enemy within 700 and holding melee
		float g_rangedNearTime = 0.0f;  // enemy within 700 and holding a ranged weapon
		float g_meleeFarTime = 0.0f;
		float g_rangedFarTime = 0.0f;
		int   g_switches = 0;
		int   g_lastKind = -1;
		float g_idleNearTime = 0.0f;    // in combat, enemy within 700, not moving and not attacking

		RE::NiPoint3 g_lastPos;
		float        g_idleWindow = 0.0f;

		std::string Trim(std::string_view a_text)
		{
			auto b = a_text.find_first_not_of(" \t\r\n");
			auto e = a_text.find_last_not_of(" \t\r\n");
			return b == std::string_view::npos ? std::string{} : std::string{ a_text.substr(b, e - b + 1) };
		}

		void ReadConfig()
		{
			std::ifstream in{ kConfig };
			if (!in) return;
			g_cfg.active = true;
			std::string line;
			while (std::getline(in, line)) {
				auto eq = line.find('=');
				if (eq == std::string::npos || line[0] == ';') continue;
				auto key = Trim(std::string_view{ line }.substr(0, eq));
				auto value = Trim(std::string_view{ line }.substr(eq + 1));
				if (key == "Save") g_cfg.save = value;
				else if (key == "Scenario") g_cfg.scenario = value;
				else if (key == "Runs") g_cfg.runs = std::max(1, std::atoi(value.c_str()));
				else if (key == "Duration") g_cfg.duration = static_cast<float>(std::atof(value.c_str()));
				else if (key == "EnemyHealth") g_cfg.enemyHealth = static_cast<float>(std::atof(value.c_str()));
			}
		}

		void Console(std::string_view a_command)
		{
			auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto script = factory ? factory->Create() : nullptr;
			if (!script) return;
			script->SetCommand(a_command);
			script->CompileAndRun(nullptr);
			delete script;
		}

		void LoadSave()
		{
			logger::info("[test] loading save {} (run {}/{})", g_cfg.save, g_run + 1, g_cfg.runs);
			g_phase = Phase::kLoading;
			if (auto manager = RE::BGSSaveLoadManager::GetSingleton()) manager->Load(g_cfg.save.c_str(), false);
		}

		RE::Actor* FirstFollower()
		{
			auto player = RE::PlayerCharacter::GetSingleton();
			for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
				auto ptr = handle.get();
				auto a = ptr.get();
				if (a && a != player && !a->IsDead() && a->IsPlayerTeammate() && !a->IsCommandedActor() && !a->IsAMount()) return a;
			}
			return nullptr;
		}

		const char* Name(RE::Actor* a) { return a ? a->GetDisplayFullName() : "-"; }

		int HeldKind(RE::Actor* a)
		{
			auto obj = a->GetEquippedObject(false);
			if (!obj) return 0;
			if (auto weap = obj->As<RE::TESObjectWEAP>()) return (weap->IsBow() || weap->IsCrossbow() || weap->IsStaff()) ? 2 : 1;
			if (obj->As<RE::SpellItem>()) return 2;
			return 0;
		}

		void StartRun()
		{
			auto player = RE::PlayerCharacter::GetSingleton();
			auto follower = FirstFollower();
			logger::info("[test] run {} start, follower {}", g_run + 1, Name(follower));
			Console("tgm");               // keep the player alive; the follower is left as is
			Console("set KillMove to 0");  // kill animations would end a fight before anything happens
			g_spawned.clear();
			g_placements.clear();
			g_relocated = 0;
			g_waterCheck = 0.0f;
			for (auto& spawn : Scenario(g_cfg.scenario)) {
				auto base = RE::TESForm::LookupByID<RE::TESNPC>(spawn.base);
				if (!base) continue;
				auto ref = player->PlaceObjectAtMe(base, false);
				auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor) continue;
				Placement place{ std::sqrt(spawn.forward * spawn.forward + spawn.side * spawn.side), std::atan2(spawn.side, spawn.forward) };
				Place(actor, place);
				actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kHealth, g_cfg.enemyHealth);
				Papyrus::StartCombat(actor, player);
				g_spawned.push_back(actor->GetHandle());
				g_placements.push_back(place);
			}
			g_firstDamage = -1.0f;
			g_meleeNearTime = g_rangedNearTime = g_meleeFarTime = g_rangedFarTime = g_idleNearTime = 0.0f;
			g_switches = 0;
			g_lastKind = -1;
			if (follower) g_lastPos = follower->GetPosition();
			g_trace << "# run " << g_run + 1 << " scenario " << g_cfg.scenario << '\n';
			g_phase = Phase::kRunning;
			g_timer = 0.0f;
		}

		int AliveSpawned()
		{
			int alive = 0;
			for (auto& h : g_spawned) {
				auto p = h.get();
				if (p && !p->IsDead()) ++alive;
			}
			return alive;
		}

		void EndRun(std::string_view a_why)
		{
			auto follower = FirstFollower();
			auto info = Followers::GetDebugInfo(follower);
			g_report << std::format(
				"run {} ({}): {} after {:.1f}s, {} of {} spawned enemies alive, {} moved out of water\n"
				"  first damage dealt at {}\n"
				"  enemy within 700: melee {:.1f}s, ranged {:.1f}s, standing idle {:.1f}s\n"
				"  enemy further away: melee {:.1f}s, ranged {:.1f}s\n"
				"  weapon switches seen: {}, mod wanted: {}\n"
				"  damage dealt: melee {:.0f} ({} hits), ranged {:.0f} ({} hits); follower health {:.0f}\n"
				"  reflexes: {} stepped out, {} blocked, {} bashed, {} taken, {} punishes, {} zig-zag approaches; damage taken {:.0f}\n",
				g_run + 1, g_cfg.scenario, a_why, g_timer, AliveSpawned(), g_spawned.size(), g_relocated,
				g_firstDamage < 0.0f ? std::string{ "never" } : std::format("{:.1f}s", g_firstDamage),
				g_meleeNearTime, g_rangedNearTime, g_idleNearTime, g_meleeFarTime, g_rangedFarTime,
				g_switches, info.pref == 1 ? "melee" : (info.pref == 2 ? "ranged" : "nothing"),
				info.dealt[0], info.hits[0], info.dealt[1], info.hits[1],
				follower ? follower->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) : 0.0f,
				info.stepOuts, info.blocks, info.bashes, info.taken, info.punishes, info.zigzags, info.damageTaken);
			g_report.flush();
			logger::info("[test] run {} ended: {}", g_run + 1, a_why);
			++g_run;
			if (g_run >= g_cfg.runs) {
				g_report << "done\n";
				g_report.flush();
				g_phase = Phase::kDone;
				g_timer = 0.0f;
			} else {
				g_phase = Phase::kCooldown;  // let death and dismemberment work finish before reloading
				g_timer = 0.0f;
			}
		}

		void Sample(float a_delta)
		{
			auto player = RE::PlayerCharacter::GetSingleton();
			auto f = FirstFollower();
			if (!f) return;
			auto info = Followers::GetDebugInfo(f);
			if (g_firstDamage < 0.0f && info.hits[0] + info.hits[1] > 0) g_firstDamage = g_timer;

			RE::Actor* nearest = nullptr;
			float      nearestDist = std::numeric_limits<float>::max();
			for (auto& h : g_spawned) {
				auto p = h.get();
				auto e = p.get();
				if (!e || e->IsDead()) continue;
				float d = f->GetPosition().GetDistance(e->GetPosition());
				if (d < nearestDist) {
					nearestDist = d;
					nearest = e;
				}
			}
			int kind = HeldKind(f);
			if (g_lastKind >= 0 && kind != g_lastKind && kind != 0) ++g_switches;
			if (kind != 0) g_lastKind = kind;
			bool near = nearest && nearestDist < 700.0f;
			if (kind == 1) (near ? g_meleeNearTime : g_meleeFarTime) += a_delta;
			if (kind == 2) (near ? g_rangedNearTime : g_rangedFarTime) += a_delta;
			// idle = barely moved over half a second, with no attack and no block going on
			g_idleWindow += a_delta;
			if (g_idleWindow >= 0.5f) {
				float moved = f->GetPosition().GetDistance(g_lastPos);
				g_lastPos = f->GetPosition();
				auto  state = f->AsActorState();
				bool  busy = state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone || f->IsBlocking();
				if (near && f->IsInCombat() && moved < 30.0f && !busy) g_idleNearTime += g_idleWindow;
				g_idleWindow = 0.0f;
			}

			auto targetPtr = f->GetActorRuntimeData().currentCombatTarget.get();
			auto target = targetPtr.get();
			g_trace << std::format("{:.1f},{:.0f},{},{:.0f},{},{},{},{:.0f},{:.0f},{}\n", g_timer,
				f->GetPosition().GetDistance(player->GetPosition()), Name(target),
				target ? f->GetPosition().GetDistance(target->GetPosition()) : -1.0f,
				f->IsInCombat() ? 1 : 0, kind, info.pref, info.dealt[0], info.dealt[1],
				f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth));
		}

		class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (a_event && a_event->opening && a_event->menuName == RE::MainMenu::MENU_NAME && !g_menuSeen && g_phase == Phase::kWaitMenu) {
					g_menuSeen = true;
					// the player update does not run in the main menu: load from a task a few seconds later
					std::thread([] {
						std::this_thread::sleep_for(std::chrono::seconds(3));
						SKSE::GetTaskInterface()->AddTask([] { LoadSave(); });
					}).detach();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	bool Active() { return g_cfg.active; }

	void Install()
	{
		ReadConfig();
		if (!g_cfg.active) return;
		logger::info("[test] test mode: save {}, scenario {}, {} runs of {:.0f}s", g_cfg.save, g_cfg.scenario, g_cfg.runs, g_cfg.duration);
		std::error_code ec;
		std::filesystem::create_directories("Data/SKSE/Plugins/FollowersWithBrains_Test", ec);
		g_trace.open("Data/SKSE/Plugins/FollowersWithBrains_Test/trace.csv", std::ios::trunc);
		g_trace << "time,distPlayer,target,distTarget,inCombat,held(1melee/2ranged),wanted,dealtMelee,dealtRanged,health\n";
		g_report.open("Data/SKSE/Plugins/FollowersWithBrains_Test/report.txt", std::ios::trunc);
		static MenuSink sink;
		RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&sink);
		g_phase = Phase::kWaitMenu;
	}

	void OnGameLoaded()
	{
		if (!g_cfg.active || g_phase != Phase::kLoading) return;
		g_phase = Phase::kSettle;
		g_timer = 0.0f;
	}

	// Called every frame from the player update while in game.
	void Tick(float a_delta)
	{
		if (!g_cfg.active) return;
		g_timer += a_delta;
		switch (g_phase) {
		case Phase::kSettle:
			if (g_timer > 4.0f) StartRun();
			break;
		case Phase::kRunning:
			if (g_timer < 6.0f && g_timer - g_waterCheck >= 0.5f) {
				g_waterCheck = g_timer;
				FixWater();
			}
			Sample(a_delta);
			if (g_spawned.empty()) {
				EndRun("nothing spawned");
			} else if (AliveSpawned() == 0) {
				EndRun("all enemies dead");
			} else if (g_timer > g_cfg.duration) {
				EndRun("time limit");
			} else if (auto f = FirstFollower(); !f) {
				EndRun("no follower");
			}
			break;
		case Phase::kCooldown:
			if (g_timer > 4.0f) LoadSave();
			break;
		case Phase::kDone:
			if (g_timer > 1.0f) {
				logger::info("[test] all runs done, quitting");
				RE::Main::GetSingleton()->quitGame = true;
				g_phase = Phase::kIdle;
			}
			break;
		default:
			break;
		}
	}
}
