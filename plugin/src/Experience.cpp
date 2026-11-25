#include "Experience.h"
#include "Tuning.h"

namespace Experience
{
	namespace
	{
		struct Entry
		{
			float       xp = 0.0f;       // attacks faced, weighted by how fast she learns
			float       lastDay = -1.0f;  // game days passed when she last met this kind
			float       tries[4]{};
			float       avoided[4]{};
			std::string name;
		};

		using Key = std::pair<RE::FormID, std::uint32_t>;  // (follower, kind)
		std::map<Key, Entry> g_data;
		std::map<Key, Entry> g_loaded;  // from the save, until the load is complete

		std::uint32_t Fnv(std::string_view a_text, std::uint32_t a_hash = 2166136261u)
		{
			for (unsigned char c : a_text) a_hash = (a_hash ^ c) * 16777619u;
			return a_hash;
		}

		float Today()
		{
			auto calendar = RE::Calendar::GetSingleton();
			return calendar ? calendar->GetDaysPassed() : 0.0f;
		}

		// How much of what she knew is still there after a_days without meeting that kind: half
		// fades in two weeks of game time, a quarter always stays.
		float Kept(const Entry& a_entry)
		{
			if (a_entry.lastDay < 0.0f) return 1.0f;
			float days = std::max(0.0f, Today() - a_entry.lastDay);
			return 0.25f + 0.75f * std::pow(0.5f, days / 14.0f);
		}

		// Bring an entry up to date (fading) before learning more.
		void Refresh(Entry& a_entry)
		{
			float keep = Kept(a_entry);
			if (keep < 0.999f) {
				a_entry.xp *= keep;
				for (int w = 0; w < 4; ++w) {
					a_entry.tries[w] *= keep;
					a_entry.avoided[w] *= keep;
				}
			}
			a_entry.lastDay = Today();
		}

		std::string WeaponKind(RE::Actor* a)
		{
			auto right = a->GetEquippedObject(false);
			auto left = a->GetEquippedObject(true);
			auto weap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
			if (!weap) return left && left->IsArmor() ? "shield" : "unarmed";
			if (weap->IsBow() || weap->IsCrossbow()) return "bow";
			if (weap->IsStaff()) return "staff";
			if (weap->IsTwoHandedSword() || weap->IsTwoHandedAxe()) return "two-handed";
			if (left && left->IsArmor()) return "weapon and shield";
			if (left && left->As<RE::TESObjectWEAP>()) return "two weapons";
			return "one-handed";
		}

		std::string KindName(RE::Actor* a)
		{
			auto race = a->GetRace();
			std::string name = race && race->GetName() && *race->GetName() ? race->GetName() : "enemy";
			if (race && race->HasKeywordString("ActorTypeNPC")) name = "people with " + WeaponKind(a);
			return name;
		}

		// How fast she learns: the skill of what she fights with (only read, never changed).
		float LearnRate(RE::Actor* f)
		{
			auto  avo = f->AsActorValueOwner();
			auto  right = f->GetEquippedObject(false);
			auto  weap = right ? right->As<RE::TESObjectWEAP>() : nullptr;
			float skill = std::max(avo->GetActorValue(RE::ActorValue::kOneHanded), avo->GetActorValue(RE::ActorValue::kTwoHanded));
			if (weap && (weap->IsTwoHandedSword() || weap->IsTwoHandedAxe())) skill = avo->GetActorValue(RE::ActorValue::kTwoHanded);
			else if (weap && weap->IsOneHandedSword()) skill = avo->GetActorValue(RE::ActorValue::kOneHanded);
			return 0.5f + std::clamp(skill, 0.0f, 100.0f) / 100.0f;
		}

		Entry& Get(RE::Actor* f, RE::Actor* e)
		{
			auto& entry = g_data[{ f->GetFormID(), KindOf(e) }];
			if (entry.name.empty()) entry.name = KindName(e);
			return entry;
		}
	}

	std::uint32_t KindOf(RE::Actor* a_enemy)
	{
		auto race = a_enemy ? a_enemy->GetRace() : nullptr;
		if (!race) return 0;
		std::string_view project = race->behaviorGraphProjectNames[0].c_str();
		std::uint32_t    key = project.empty() ? race->GetFormID() : Fnv(project);
		if (race->HasKeywordString("ActorTypeNPC")) key = Fnv(WeaponKind(a_enemy), key);
		return key;
	}

	Reading Read(RE::Actor* a_follower, RE::Actor* a_enemy)
	{
		if (Tuning::Get().practice > 0.0f) return { 1.0f, 0.0f };  // test switch: perfect reads
		auto  it = g_data.find({ a_follower->GetFormID(), KindOf(a_enemy) });
		float xp = it == g_data.end() ? 0.0f : it->second.xp * Kept(it->second);
		// a fight is some 20-30 attacks: ~45% learned after one, ~85% after three or four
		float learned = 1.0f - std::exp(-xp / std::max(1.0f, Tuning::Get().learnScale));
		return { 0.3f + 0.55f * learned, 0.05f + 0.22f * (1.0f - learned) };
	}

	void SawAttack(RE::Actor* a_follower, RE::Actor* a_enemy)
	{
		auto& entry = Get(a_follower, a_enemy);
		Refresh(entry);
		entry.xp += LearnRate(a_follower);
	}

	int PickWay(RE::Actor* a_follower, RE::Actor* a_enemy)
	{
		if (std::rand() % 100 < 15) return std::rand() % 4;  // try something else now and then
		auto& entry = Get(a_follower, a_enemy);
		float keep = Kept(entry);
		int   best = 0;
		float bestScore = -1.0f;
		for (int w = 0; w < 4; ++w) {
			float score = (entry.avoided[w] * keep + 1.0f) / (entry.tries[w] * keep + 2.0f) + (std::rand() % 100) * 0.0001f;
			if (score > bestScore) {
				bestScore = score;
				best = w;
			}
		}
		return best;
	}

	void Outcome(RE::FormID a_follower, std::uint32_t a_kind, int a_way, bool a_avoided)
	{
		auto it = g_data.find({ a_follower, a_kind });
		if (it == g_data.end() || a_way < 0 || a_way > 3) return;
		Refresh(it->second);
		it->second.tries[a_way] += 1.0f;
		if (a_avoided) it->second.avoided[a_way] += 1.0f;
	}

	std::string Summary(RE::Actor* a_follower, RE::Actor* a_enemy)
	{
		auto it = g_data.find({ a_follower->GetFormID(), KindOf(a_enemy) });
		if (it == g_data.end()) return "never fought these";
		auto& entry = it->second;
		auto  read = Read(a_follower, a_enemy);
		int   best = -1;
		float bestRate = -1.0f;
		for (int w = 0; w < 4; ++w) {
			if (entry.tries[w] < 2.0f) continue;
			float rate = entry.avoided[w] / entry.tries[w];
			if (rate > bestRate) {
				bestRate = rate;
				best = w;
			}
		}
		auto text = std::format("{}: sees {:.0f}% of their attacks coming, reacts in {:.2f}s", entry.name, read.chance * 100.0f, read.delay);
		if (best >= 0) text += std::format("; best way out: {} ({:.0f} of {:.0f})", kWays[best], entry.avoided[best], entry.tries[best]);
		return text;
	}

	// ------------------------------------------------------------------ saving
	namespace
	{
		template <class W>
		void WriteAll(W&& a_write, const std::map<Key, Entry>& a_data)
		{
			a_write(static_cast<std::uint32_t>(a_data.size()));
			for (auto& [key, e] : a_data) {
				a_write(key.first);
				a_write(key.second);
				a_write(e.xp);
				a_write(e.lastDay);
				for (int w = 0; w < 4; ++w) a_write(e.tries[w]);
				for (int w = 0; w < 4; ++w) a_write(e.avoided[w]);
				auto len = static_cast<std::uint32_t>(std::min<std::size_t>(e.name.size(), 200));
				a_write(len);
				for (std::uint32_t i = 0; i < len; ++i) a_write(e.name[i]);
			}
		}

		template <class R>
		std::map<Key, Entry> ReadAll(R&& a_read, bool a_resolve, SKSE::SerializationInterface* a_intfc)
		{
			std::map<Key, Entry> out;
			std::uint32_t        count = 0;
			if (!a_read(count)) return out;
			for (std::uint32_t i = 0; i < count && i < 10000; ++i) {
				RE::FormID    id = 0;
				std::uint32_t kind = 0, len = 0;
				Entry         e;
				bool ok = a_read(id) && a_read(kind) && a_read(e.xp) && a_read(e.lastDay);
				for (int w = 0; w < 4 && ok; ++w) ok = a_read(e.tries[w]);
				for (int w = 0; w < 4 && ok; ++w) ok = a_read(e.avoided[w]);
				ok = ok && a_read(len) && len <= 200;
				e.name.resize(ok ? len : 0);
				for (std::uint32_t c = 0; c < len && ok; ++c) ok = a_read(e.name[c]);
				if (!ok) break;
				RE::FormID resolved = id;
				if (a_resolve && !a_intfc->ResolveFormID(id, resolved)) continue;
				out[{ resolved, kind }] = std::move(e);
			}
			return out;
		}

		std::filesystem::path StashPath() { return std::filesystem::temp_directory_path() / "fwb-experience.bin"; }
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc->OpenRecord(kRecord, 1)) return;
		WriteAll([&](auto a_value) { a_intfc->WriteRecordData(a_value); }, g_data);
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t, std::uint32_t)
	{
		g_loaded = ReadAll([&](auto& a_value) { return a_intfc->ReadRecordData(a_value) == sizeof(a_value); }, true, a_intfc);
		logger::info("Experience loaded: {} kinds of enemy", g_loaded.size());
	}

	void Revert() { g_loaded.clear(); }

	void Apply()
	{
		g_data = std::move(g_loaded);
		g_loaded.clear();
	}

	void Stash()
	{
		std::ofstream out{ StashPath(), std::ios::binary | std::ios::trunc };
		WriteAll([&](auto a_value) { out.write(reinterpret_cast<const char*>(&a_value), sizeof(a_value)); }, g_data);
	}

	void Unstash()
	{
		std::ifstream in{ StashPath(), std::ios::binary };
		if (!in) return;
		g_data = ReadAll([&](auto& a_value) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&a_value), sizeof(a_value))); }, false, nullptr);
		in.close();
		std::error_code ec;
		std::filesystem::remove(StashPath(), ec);
		logger::info("Experience kept across the reload: {} kinds of enemy", g_data.size());
	}
}
