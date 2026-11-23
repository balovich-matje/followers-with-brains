#include "Papyrus.h"
#include "Settings.h"

namespace Papyrus
{
	namespace
	{
		constexpr auto kNativeClass = "FWB_Native"sv;
		constexpr auto kActionsClass = "FWB_Actions"sv;

		std::int32_t GetInt(RE::StaticFunctionTag*, RE::BSFixedString a_key) { return Settings::GetInt(a_key.c_str()); }
		void SetInt(RE::StaticFunctionTag*, RE::BSFixedString a_key, std::int32_t a_value) { Settings::SetInt(a_key.c_str(), a_value); }
		float GetFloat(RE::StaticFunctionTag*, RE::BSFixedString a_key) { return Settings::GetFloat(a_key.c_str()); }
		void SetFloat(RE::StaticFunctionTag*, RE::BSFixedString a_key, float a_value) { Settings::SetFloat(a_key.c_str(), a_value); }
		void Save(RE::StaticFunctionTag*) { Settings::Save(); }

		template <class... Args>
		void Call(std::string_view a_fn, Args... a_args)
		{
			auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			if (!vm) return;
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
			auto args = RE::MakeFunctionArguments(std::move(a_args)...);
			vm->DispatchStaticCall(kActionsClass, a_fn, args, callback);
		}
	}

	bool Register(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("GetInt"sv, kNativeClass, GetInt);
		a_vm->RegisterFunction("SetInt"sv, kNativeClass, SetInt);
		a_vm->RegisterFunction("GetFloat"sv, kNativeClass, GetFloat);
		a_vm->RegisterFunction("SetFloat"sv, kNativeClass, SetFloat);
		a_vm->RegisterFunction("Save"sv, kNativeClass, Save);
		return true;
	}

	void StartCombat(RE::Actor* a_actor, RE::Actor* a_target) { Call("StartCombat"sv, a_actor, a_target); }
	void StopCombat(RE::Actor* a_actor) { Call("StopCombat"sv, a_actor); }
	void KeepOffset(RE::Actor* a_actor, RE::Actor* a_target, float a_x, float a_y, float a_catchUp, float a_follow, float a_angle)
	{
		Call("KeepOffset"sv, a_actor, a_target, a_x, a_y, a_catchUp, a_follow, a_angle);
	}
	void ClearOffset(RE::Actor* a_actor) { Call("ClearOffset"sv, a_actor); }
	void MoveTo(RE::Actor* a_actor, RE::Actor* a_target, float a_x, float a_y) { Call("MoveTo"sv, a_actor, a_target, a_x, a_y); }
}
