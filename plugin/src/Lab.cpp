#include "Lab.h"

extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char* a_name, const char* a_value);
#include "Driver.h"
#include "Followers.h"
#include "Papyrus.h"
#include "Settings.h"
#include "Tuning.h"

namespace Lab
{
	namespace
	{
		const std::filesystem::path kConfig{ "Data/SKSE/Plugins/FollowersWithBrains_Lab.ini" };

		bool                  g_on = false;
		std::filesystem::path g_dir;
		std::ofstream         g_track;
		float                 g_sampleTimer = 0.0f;
		float                 g_pollTimer = 0.0f;
		std::uintmax_t        g_outOffset = 0;
		bool                  g_playerCombat = false;
		RE::ActorHandle       g_last;  // last actor spawned by a command

		// local time of day in seconds (matches the [HH:MM:SS.mmm] stamps of the plugin log)
		double Now()
		{
			auto now = std::chrono::system_clock::now();
			auto t = std::chrono::system_clock::to_time_t(now);
			std::tm tm{};
			localtime_s(&tm, &t);
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
			return tm.tm_hour * 3600.0 + tm.tm_min * 60.0 + tm.tm_sec + ms / 1000.0;
		}

		std::string Clean(std::string_view a_text)
		{
			std::string out{ a_text };
			for (auto& c : out) {
				if (c == ',' || c == '\n' || c == '\r') c = ' ';
			}
			return out;
		}

		const char* Name(RE::TESObjectREFR* a) { return a ? a->GetDisplayFullName() : "none"; }

		void Print(std::string_view a_text)
		{
			if (auto console = RE::ConsoleLog::GetSingleton()) console->Print("[FWB] %s", std::string{ a_text }.c_str());
		}

		void Say(std::string_view a_text)
		{
			Print(a_text);
			RE::DebugNotification(std::string{ a_text }.c_str());
			logger::info("LAB say: {}", a_text);
		}

		void RunConsole(std::string_view a_command)
		{
			auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto script = factory ? factory->Create() : nullptr;
			if (!script) {
				Print("can't run console commands");
				return;
			}
			script->SetCommand(a_command);
			script->CompileAndRun(nullptr);
			delete script;
			logger::info("LAB console: {}", a_command);
		}

		// ---------------------------------------------------------------- recorder
		void Sample(RE::Actor* a, char a_role)
		{
			if (!a || !a->Is3DLoaded()) return;
			auto  pos = a->GetPosition();
			auto  st = a->AsActorState();
			auto  avo = a->AsActorValueOwner();
			float hp = avo->GetActorValue(RE::ActorValue::kHealth);
			float maxHp = avo->GetPermanentActorValue(RE::ActorValue::kHealth);
			auto  target = a->GetActorRuntimeData().currentCombatTarget.get();
			bool  stagger = false;
			a->GetGraphVariableBool("IsStaggering", stagger);
			// both hands, with the kind of item (names are localised and easy to misread)
			auto hand = [](RE::TESForm* a_item) -> std::string {
				if (!a_item) return "-";
				std::string kind = "item";
				if (auto w = a_item->As<RE::TESObjectWEAP>()) {
					kind = w->IsBow() ? "bow" : w->IsCrossbow() ? "crossbow" : w->IsStaff() ? "staff" :
					       w->IsTwoHandedSword() ? "2h sword" : w->IsTwoHandedAxe() ? "2h axe/hammer" :
					       w->IsOneHandedSword() ? "1h sword" : w->IsOneHandedAxe() ? "1h axe" : w->IsOneHandedMace() ? "1h mace" :
					       w->IsOneHandedDagger() ? "dagger" : w->IsHandToHandMelee() ? "fists" : "weapon";
				} else if (a_item->IsArmor()) {
					kind = "shield";
				} else if (a_item->Is(RE::FormType::Spell)) {
					kind = "spell";
				}
				return std::format("{} ({})", Clean(a_item->GetName()), kind);
			};
			std::string weapon = hand(a->GetEquippedObject(false)) + " | " + hand(a->GetEquippedObject(true));
			RE::NiPoint3 vel;
			a->GetLinearVelocity(vel);
			vel.z = 0.0f;
			g_track << std::format("{:.3f},{:08X},{},{},{:.0f},{:.0f},{:.0f},{:.0f},{:.0f},{:.0f},{:.2f},{},{:08X},{},{}{}{}{}{}{}{},{},{}\n",
				Now(), a->GetFormID(), a_role, Clean(a->GetDisplayFullName()), pos.x, pos.y, pos.z,
				a->GetAngleZ() * 57.2958f, vel.Length(), hp, maxHp > 0.0f ? hp / maxHp : 0.0f, a->IsInCombat() ? 1 : 0,
				target ? target->GetFormID() : 0u, static_cast<int>(st->GetAttackState()),
				st->IsWeaponDrawn() ? "D" : "", st->IsSprinting() ? "S" : "", st->IsSneaking() ? "K" : "",
				a->IsBlocking() ? "B" : "", stagger ? "G" : "", st->IsBleedingOut() ? "L" : "", a->IsDead() ? "X" : "",
				Driver::Current(a), weapon);
		}

		void Record()
		{
			auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->Is3DLoaded()) return;
			std::vector<RE::Actor*> followers, enemies;
			Followers::Tracked(followers, enemies);
			Sample(player, 'P');
			for (auto f : followers) Sample(f, 'F');
			for (auto e : enemies) {
				if (e->GetPosition().GetDistance(player->GetPosition()) < 6000.0f) Sample(e, 'E');
			}
			if (auto last = g_last.get().get(); last && std::find(enemies.begin(), enemies.end(), last) == enemies.end()) {
				Sample(last, 'S');
			}
			bool combat = player->IsInCombat();
			if (combat != g_playerCombat) {
				g_playerCombat = combat;
				logger::info("LAB combat {}", combat ? "start" : "end");
			}
			static int flush = 0;
			if (++flush % 10 == 0) g_track.flush();
		}

		// hits and deaths near the player, into the plugin log
		struct Events :
			public RE::BSTEventSink<RE::TESHitEvent>,
			public RE::BSTEventSink<RE::TESDeathEvent>
		{
			static Events* Get()
			{
				static Events instance;
				return &instance;
			}

			static bool Near(RE::TESObjectREFR* a)
			{
				auto player = RE::PlayerCharacter::GetSingleton();
				return a && player && a->GetPosition().GetDistance(player->GetPosition()) < 6000.0f;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !g_on) return RE::BSEventNotifyControl::kContinue;
				auto cause = a_event->cause.get();  // raw pointers
				auto target = a_event->target.get();
				if (!Near(target)) return RE::BSEventNotifyControl::kContinue;
				auto        flags = a_event->flags;
				std::string what;
				if (flags.any(RE::TESHitEvent::Flag::kPowerAttack)) what += " power";
				if (flags.any(RE::TESHitEvent::Flag::kBashAttack)) what += " bash";
				if (flags.any(RE::TESHitEvent::Flag::kHitBlocked)) what += " blocked";
				if (flags.any(RE::TESHitEvent::Flag::kSneakAttack)) what += " sneak";
				auto source = RE::TESForm::LookupByID(a_event->source);
				logger::info("LAB hit {} ({:08X}) -> {} ({:08X}){} [{}]", Name(cause), cause ? cause->GetFormID() : 0u,
					Name(target), target->GetFormID(), what, source ? source->GetName() : "");
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (!a_event || !g_on || !a_event->dead) return RE::BSEventNotifyControl::kContinue;
				auto dead = a_event->actorDying.get();
				if (Near(dead)) logger::info("LAB death {} ({:08X})", Name(dead), dead->GetFormID());
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// ---------------------------------------------------------------- commands from the helper
		RE::Actor* Who(std::string_view a_token)
		{
			auto player = RE::PlayerCharacter::GetSingleton();
			if (a_token == "player") return player;
			std::vector<RE::Actor*> followers, enemies;
			Followers::Tracked(followers, enemies);
			if (a_token.starts_with("follower")) {
				std::size_t i = a_token.size() > 8 ? static_cast<std::size_t>(a_token[8] - '1') : 0;
				return i < followers.size() ? followers[i] : nullptr;
			}
			if (a_token == "last") return g_last.get().get();
			if (a_token == "enemy") {
				RE::Actor* best = nullptr;
				float      bestDist = 1e9f;
				for (auto e : enemies) {
					float d = e->GetPosition().GetDistance(player->GetPosition());
					if (!e->IsDead() && d < bestDist) {
						best = e;
						bestDist = d;
					}
				}
				return best;
			}
			try {
				auto form = RE::TESForm::LookupByID(static_cast<RE::FormID>(std::stoul(std::string{ a_token }, nullptr, 16)));
				return form ? form->As<RE::Actor>() : nullptr;
			} catch (...) {
				return nullptr;
			}
		}

		RE::FormID BaseFor(std::string_view a_token)
		{
			// level 25 bandits from Skyrim.esm (same as the test harness)
			if (a_token == "bandit") return 0x39D21;
			if (a_token == "archer") return 0x37C47;
			if (a_token == "brute") return 0x3DE83;
			// creatures and vampires (Skyrim.esm; levels as RFAB sets them)
			if (a_token == "sabrecat") return 0x23AB5;    // 20
			if (a_token == "snowsabre") return 0x23AB6;   // 40
			if (a_token == "bear") return 0x23A8A;        // 20
			if (a_token == "cavebear") return 0x23A8B;    // 30
			if (a_token == "snowbear") return 0x23A8C;    // 40
			if (a_token == "troll") return 0x23ABA;       // 20
			if (a_token == "frosttroll") return 0x23ABB;  // 40
			if (a_token == "vampire75") return 0x2EAC1;   // Volkihar clan (EncVampire06BossNordM)
			if (a_token == "vampire90") return 0x3392F;   // night hunter (EncVampire06NordF)
			if (a_token == "draugr") return 0x3B548;       // RFAB draugr, level 30, one-handed
			if (a_token == "draugr2h") return 0x5B752;     // RFAB draugr, level 37, two-handed
			if (a_token == "spider") return 0x41FB4;       // large frostbite spider, level 25
			if (a_token == "giantspider") return 0x23AAB;  // giant frostbite spider, level 50
			try {
				return static_cast<RE::FormID>(std::stoul(std::string{ a_token }, nullptr, 16));
			} catch (...) {
				return 0;
			}
		}

		std::unordered_map<RE::FormID, float> g_extraHealth;

		// Sets the extra health (0 = back to her real health). It used to add, and repeated test
		// commands piled up to +18000.
		void AddHealth(RE::Actor* a, float a_amount)
		{
			auto& extra = g_extraHealth[a->GetFormID()];
			float target = std::max(0.0f, a_amount);
			float delta = target - extra;
			extra = target;
			auto avo = a->AsActorValueOwner();
			avo->ModActorValue(RE::ActorValue::kHealth, delta);
			avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, 1e6f);
		}

		void Run(std::string_view a_line)
		{
			std::vector<std::string> words;
			{
				std::istringstream in{ std::string{ a_line } };
				for (std::string w; in >> w;) words.push_back(w);
			}
			if (words.empty()) return;
			auto  rest = [&](std::size_t i) {
                std::size_t pos = 0;
                for (std::size_t k = 0; k < i && pos != std::string_view::npos; ++k) {
                    pos = a_line.find_first_not_of(" \t", pos);
                    pos = a_line.find_first_of(" \t", pos);
                }
                if (pos == std::string_view::npos) return std::string{};
                pos = a_line.find_first_not_of(" \t", pos);
                return pos == std::string_view::npos ? std::string{} : std::string{ a_line.substr(pos) };
			};
			auto  num = [&](std::size_t i, float a_default) {
				try {
					return i < words.size() ? std::stof(words[i]) : a_default;
				} catch (...) {
					return a_default;
				}
			};
			auto  player = RE::PlayerCharacter::GetSingleton();
			auto& cmd = words[0];
			logger::info("LAB command: {}", a_line);

			if (cmd == "say") {
				Say(rest(1));
			} else if (cmd == "console") {
				RunConsole(rest(1));
			} else if (cmd == "shot") {
				logger::info("LAB shot {}", rest(1));
			} else if (cmd == "spawn" && words.size() >= 2) {
				// spawn <bandit|archer|brute|base id> [extra health] [distance ahead] [side offset]
				auto base = RE::TESForm::LookupByID<RE::TESBoundObject>(BaseFor(words[1]));
				if (!base) {
					Say(std::format("spawn: no such base {}", words[1]));
					return;
				}
				auto ref = player->PlaceObjectAtMe(base, false);
				auto actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor) {
					Say("spawn failed");
					return;
				}
				float extra = num(2, 0.0f), dist = num(3, 700.0f), side = num(4, 0.0f);
				if (extra > 0.0f) AddHealth(actor, extra);
				float z = player->GetAngleZ();
				Papyrus::MoveTo(actor, player, std::sin(z) * dist + std::cos(z) * side, std::cos(z) * dist - std::sin(z) * side);
				g_last = actor->GetHandle();
				Say(std::format("spawned {} {:08X}", actor->GetDisplayFullName(), actor->GetFormID()));
			} else if (cmd == "fight" && words.size() >= 3) {
				// fight <who> <target> [both]
				auto a = Who(words[1]);
				auto b = Who(words[2]);
				if (!a || !b) {
					Say("fight: who?");
					return;
				}
				Papyrus::StartCombat(a, b);
				if (words.size() >= 4 && words[3] == "both" && !b->IsPlayerRef()) Papyrus::StartCombat(b, a);
				Say(std::format("{} fights {}", a->GetDisplayFullName(), b->GetDisplayFullName()));
			} else if (cmd == "health" && words.size() >= 3) {
				// health <who> <extra>: this much extra health (0 = none), fully healed
				if (auto a = Who(words[1])) {
					AddHealth(a, num(2, 0.0f));
					Say(std::format("{} health {:.0f}", a->GetDisplayFullName(), a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth)));
				}
			} else if (cmd == "base" && words.size() >= 2) {
				// base <ref id>: what a placed actor is made from (to spawn it again later)
				RE::Actor* a = nullptr;
				try {
					a = RE::TESForm::LookupByID<RE::Actor>(static_cast<RE::FormID>(std::stoul(words[1], nullptr, 16)));
				} catch (...) {}
				auto base = a ? a->GetActorBase() : nullptr;
				auto lvl = a && a->extraList.GetByType<RE::ExtraLeveledCreature>() ? a->extraList.GetByType<RE::ExtraLeveledCreature>()->originalBase : nullptr;
				Say(base ? std::format("{} base {:08X}{}", a->GetDisplayFullName(), base->GetFormID(),
				               lvl ? std::format(" (leveled from {:08X})", lvl->GetFormID()) : std::string{}) : "base: no such actor");
			} else if (cmd == "stop" && words.size() >= 2) {
				if (auto a = Who(words[1])) {
					Papyrus::StopCombat(a);
					Say(std::format("{} stops fighting", a->GetDisplayFullName()));
				}
			} else if (cmd == "equip" && words.size() >= 3) {
				// equip <who> <item id>: given if she doesn't have it, then equipped (two-handers clear both hands)
				auto who = Who(words[1]);
				RE::TESBoundObject* item = nullptr;
				try {
					item = RE::TESForm::LookupByID<RE::TESBoundObject>(static_cast<RE::FormID>(std::stoul(words[2], nullptr, 16)));
				} catch (...) {}
				if (!who || !item) {
					Say("equip: who or what?");
					return;
				}
				auto counts = who->GetInventoryCounts();
				int want = words.size() >= 4 ? static_cast<int>(num(3, 1.0f)) : 1;  // equip <who> <id> [count]: arrows
				if (counts[item] < want) who->AddObjectToContainer(item, nullptr, want - std::max(0, counts[item]), nullptr);
				RE::ActorEquipManager::GetSingleton()->EquipObject(who, item);
				Say(std::format("{} equips {}", who->GetDisplayFullName(), item->GetName()));
			} else if (cmd == "unequip" && words.size() >= 3) {
				// unequip <who> <left|right>
				auto who = Who(words[1]);
				auto obj = who ? who->GetEquippedObject(words[2] == "left") : nullptr;
				if (auto bound = obj ? obj->As<RE::TESBoundObject>() : nullptr) {
					RE::ActorEquipManager::GetSingleton()->UnequipObject(who, bound);
					Say(std::format("{} unequips {}", who->GetDisplayFullName(), bound->GetName()));
				}
			} else if (cmd == "learn" && words.size() >= 3) {
				// learn <who> <spell id>
				auto who = Who(words[1]);
				RE::SpellItem* spell = nullptr;
				try {
					spell = RE::TESForm::LookupByID<RE::SpellItem>(static_cast<RE::FormID>(std::stoul(words[2], nullptr, 16)));
				} catch (...) {}
				if (who && spell) {
					who->AddSpell(spell);
					Say(std::format("{} learns {}", who->GetDisplayFullName(), spell->GetName()));
				}
			} else if (cmd == "clearbodies") {
				// clearbodies [radius]: remove the dead (not followers) around the player
				int  removed = 0;
				auto tes = RE::TES::GetSingleton();
				if (tes) {
					std::vector<RE::Actor*> dead;
					tes->ForEachReferenceInRange(player, num(1, 4000.0f), [&](RE::TESObjectREFR& a_ref) {
						auto actor = a_ref.As<RE::Actor>();
						if (actor && actor->IsDead() && !actor->IsPlayerTeammate() && !actor->IsPlayerRef()) dead.push_back(actor);
						return RE::BSContainer::ForEachResult::kContinue;
					});
					for (auto actor : dead) {
						actor->Disable();
						++removed;
					}
				}
				Say(std::format("removed {} bodies", removed));
			} else if (cmd == "tune" && words.size() >= 2) {
				// tune <Key> <value> | tune <Key> (back to default) | tune reset: written to the lab's
				// tuning.ini, which is read after the normal tuning (kept across loads and reloads)
				auto                                     path = g_dir / "tuning.ini";
				std::vector<std::pair<std::string, std::string>> values;
				{
					std::ifstream in{ path };
					for (std::string line; std::getline(in, line);) {
						auto eq = line.find('=');
						if (eq == std::string::npos) continue;
						auto k = line.substr(0, eq), v = line.substr(eq + 1);
						std::erase(k, ' ');
						std::erase(v, ' ');
						if (words[1] != "reset" && k != words[1]) values.emplace_back(k, v);
					}
				}
				if (words[1] != "reset" && !Tuning::IsKey(words[1])) {
					Say(std::format("tune: unknown value {}", words[1]));
					return;
				}
				if (words[1] != "reset" && words.size() >= 3) values.emplace_back(words[1], words[2]);
				{
					std::ofstream out{ path, std::ios::trunc };
					out << "; lab tuning (tools/fwb-lab-send.sh \"tune Key value\")\n";
					for (auto& [k, v] : values) out << k << " = " << v << "\n";
				}
				Tuning::Load();
				Say(words[1] == "reset" ? std::string{ "tuning back to defaults" } :
				    words.size() >= 3   ? std::format("{} = {}", words[1], words[2]) : std::format("{} back to default", words[1]));
			} else if (cmd == "set" && words.size() >= 3) {
				// set <Setting> <value>: same as changing it in the MCM
				if (words[2].find('.') != std::string::npos) {
					Settings::SetFloat(words[1], num(2, 0.0f));
				} else {
					Settings::SetInt(words[1], static_cast<int>(num(2, 0.0f)));
				}
				Settings::Save();
				Say(std::format("{} = {}", words[1], words[2]));
			} else {
				Say(std::format("unknown lab command: {}", cmd));
			}
		}

		void Poll()
		{
			auto            path = g_dir / "out.txt";
			std::error_code ec;
			auto            size = std::filesystem::file_size(path, ec);
			if (ec || size == g_outOffset) return;
			if (size < g_outOffset) g_outOffset = 0;  // file replaced
			std::ifstream in{ path, std::ios::binary };
			in.seekg(static_cast<std::streamoff>(g_outOffset));
			std::string chunk(static_cast<std::size_t>(size - g_outOffset), '\0');
			in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
			auto end = chunk.rfind('\n');
			if (end == std::string::npos) return;  // wait for a whole line
			g_outOffset += end + 1;
			std::istringstream lines{ chunk.substr(0, end) };
			for (std::string line; std::getline(lines, line);) {
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (!line.empty()) Run(line);
			}
		}
	}

	bool On() { return g_on; }

	void ForgetHealth() { g_extraHealth.clear(); }

	float ExtraHealth(RE::Actor* a_actor)
	{
		auto it = a_actor ? g_extraHealth.find(a_actor->GetFormID()) : g_extraHealth.end();
		return it == g_extraHealth.end() ? 0.0f : it->second;
	}

	void Install()
	{
		if (!std::filesystem::exists(kConfig)) return;
		std::ifstream in{ kConfig };
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.starts_with("Dir=")) g_dir = line.substr(4);
		}
		if (g_dir.empty()) return;
		std::error_code ec;
		auto            now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm         tm{};
		localtime_s(&tm, &now);
		// one session per game launch: the loader names it, so a hot reload keeps writing to it
		std::string stamp = std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
		char env[64]{};
		if (GetEnvironmentVariable("FWB_SESSION", env, sizeof(env)) > 0) stamp = env;
		auto session = g_dir / "sessions" / stamp;
		std::filesystem::create_directories(session, ec);
		bool fresh = !std::filesystem::exists(session / "track.csv");
		g_track.open(session / "track.csv", std::ios::app);
		if (!g_track) {
			logger::warn("LAB: can't write to {}", session.string());
			return;
		}
		if (fresh) g_track << "t,id,role,name,x,y,z,heading,speed,hp,hpPct,combat,target,attack,flags,drive,weapon\n";
		g_outOffset = std::filesystem::file_size(g_dir / "out.txt", ec);
		if (ec) g_outOffset = 0;
		// a hot reload: carry on where the old brain stopped reading (commands sent meanwhile still run)
		char offset[32]{};
		if (GetEnvironmentVariable("FWB_OUT_OFFSET", offset, sizeof(offset)) > 0) g_outOffset = std::strtoull(offset, nullptr, 10);
		char extra[4096]{};
		if (GetEnvironmentVariable("FWB_EXTRA_HEALTH", extra, sizeof(extra)) > 0) {
			std::istringstream in{ std::string{ extra } };
			for (std::string item; std::getline(in, item, ';');) {
				auto eq = item.find('=');
				if (eq == std::string::npos) continue;
				g_extraHealth[static_cast<RE::FormID>(std::stoul(item.substr(0, eq), nullptr, 16))] = std::stof(item.substr(eq + 1));
			}
		}
		auto events = RE::ScriptEventSourceHolder::GetSingleton();
		events->AddEventSink<RE::TESHitEvent>(Events::Get());
		events->AddEventSink<RE::TESDeathEvent>(Events::Get());
		Tuning::SetExtraFile(g_dir / "tuning.ini");
		Tuning::Load();
		g_on = true;
		logger::info("LAB on: {}", session.string());
	}

	void Shutdown()
	{
		if (!g_on) return;
		auto events = RE::ScriptEventSourceHolder::GetSingleton();
		events->RemoveEventSink<RE::TESHitEvent>(Events::Get());
		events->RemoveEventSink<RE::TESDeathEvent>(Events::Get());
		g_track.close();
		SetEnvironmentVariableA("FWB_OUT_OFFSET", std::to_string(g_outOffset).c_str());
		// test health added by commands, for the next brain
		std::string extra;
		for (auto& [id, amount] : g_extraHealth) extra += std::format("{:08X}={:.0f};", id, amount);
		SetEnvironmentVariableA("FWB_EXTRA_HEALTH", extra.c_str());
		g_on = false;
	}

	void Frame(float a_delta)
	{
		if (!g_on) return;
		g_sampleTimer += a_delta;
		g_pollTimer += a_delta;
		if (g_sampleTimer >= 0.1f) {
			g_sampleTimer = 0.0f;
			Record();
		}
		if (g_pollTimer >= 0.25f) {
			g_pollTimer = 0.0f;
			Poll();
		}
	}
}
