#pragma once

// SKSE services the brain uses, handed over by the loader. (SKSE's own setup may only run while
// plugins load, so a brain loaded later, by a hot reload, can't ask SKSE for them itself.)
namespace Host
{
	inline const SKSE::TaskInterface*              g_tasks = nullptr;
	inline RE::BSTEventSource<SKSE::ActionEvent>*  g_actionEvents = nullptr;

	inline const SKSE::TaskInterface*             Tasks() { return g_tasks; }
	inline RE::BSTEventSource<SKSE::ActionEvent>* ActionEvents() { return g_actionEvents; }
}
