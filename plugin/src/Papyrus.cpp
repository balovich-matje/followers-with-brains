#include "Papyrus.h"

namespace Papyrus
{
	namespace
	{
		constexpr auto kActionsClass = "FWB_Actions"sv;

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

	void StartCombat(RE::Actor* a_actor, RE::Actor* a_target) { Call("StartCombat"sv, a_actor, a_target); }
	void StopCombat(RE::Actor* a_actor) { Call("StopCombat"sv, a_actor); }
	void KeepOffset(RE::Actor* a_actor, RE::Actor* a_target, float a_x, float a_y, float a_catchUp, float a_follow, float a_angle)
	{
		Call("KeepOffset"sv, a_actor, a_target, a_x, a_y, a_catchUp, a_follow, a_angle);
	}
	void ClearOffset(RE::Actor* a_actor) { Call("ClearOffset"sv, a_actor); }
	void MoveTo(RE::Actor* a_actor, RE::Actor* a_target, float a_x, float a_y) { Call("MoveTo"sv, a_actor, a_target, a_x, a_y); }
}
