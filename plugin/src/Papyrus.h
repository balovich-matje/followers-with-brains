#pragma once

// Bridge to Papyrus: native functions for the MCM, and engine actions that CommonLib does not
// expose directly (they are run through the global functions in FWB_Actions.psc).
namespace Papyrus
{
	bool Register(RE::BSScript::IVirtualMachine* a_vm);

	void StartCombat(RE::Actor* a_actor, RE::Actor* a_target);
	void StopCombat(RE::Actor* a_actor);
	void KeepOffset(RE::Actor* a_actor, RE::Actor* a_target, float a_x, float a_y, float a_catchUp, float a_follow);
	void ClearOffset(RE::Actor* a_actor);
	void MoveTo(RE::Actor* a_actor, RE::Actor* a_target, float a_x, float a_y);
}
