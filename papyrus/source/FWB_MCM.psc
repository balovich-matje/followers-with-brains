Scriptname FWB_MCM extends SKI_ConfigBase
{Settings menu for Followers With Brains. Values live in the SKSE plugin (FWB_Native).}

String[] _distanceNames
Int _distanceOID
Int _combatDistanceOID
Int _retreatDistanceOID

String[] _toggleKeys
String[] _toggleNames
String[] _toggleInfo
Int[] _toggleOIDs

Int Function GetVersion()
	Return 2
EndFunction

Event OnVersionUpdate(Int a_version)
	If a_version > 1
		OnConfigInit()
	EndIf
EndEvent

Event OnConfigInit()
	ModName = "Followers With Brains"
	Pages = new String[2]
	Pages[0] = "Movement"
	Pages[1] = "Combat"

	_distanceNames = new String[3]
	_distanceNames[0] = "Close"
	_distanceNames[1] = "Medium"
	_distanceNames[2] = "Far"

	_toggleKeys = new String[11]
	_toggleNames = new String[11]
	_toggleInfo = new String[11]
	_toggleOIDs = new Int[11]
	SetToggle(0, "CatchUp", "Catch-up speed boost", "Followers who fall behind move faster until they catch up.")
	SetToggle(1, "Teleport", "Teleport when far behind", "A follower who is still far behind after a few seconds, and out of your sight, is moved behind you.")
	SetToggle(2, "CombatLeash", "Stay close in combat", "Followers stop chasing enemies that are far away from you.")
	SetToggle(3, "Awareness", "Combat awareness", "Followers get ready when you aim at or watch an enemy, attack the moment you do, and join fights you are in.")
	SetToggle(4, "Retarget", "Next target right away", "When their target dies, followers move straight on to the next enemy fighting you.")
	SetToggle(5, "FocusSummoners", "Focus summoners", "Followers go for the summoner instead of the summon when they can reach them. If the summon keeps hitting them, they fight it.")
	SetToggle(6, "MeleeSwitch", "Switch target after misses", "A melee follower who misses twice while another enemy hits them switches to that enemy. A follower stuck short of its target tries another way or fights whoever is hitting it.")
	SetToggle(7, "Retreat", "Retreat with you", "When you back away from a fight, followers come with you. Ranged followers keep shooting while they are close to you. Healing your party does not count as retreating.")
	SetToggle(8, "Debug", "Debug messages", "Show what followers decide, as notifications.")
	SetToggle(9, "SmartWeapons", "Ranged or melee by distance", "Followers who carry both pick by skill and distance: they shoot while enemies are far and switch to melee as enemies arrive (or the other way round for archers and mages).")
	SetToggle(10, "MatchWeapons", "Match your weapon", "Out of combat, when you draw a bow or an attack spell your followers draw their ranged weapon, and switch back when you draw a melee weapon.")
EndEvent

Function SetToggle(Int i, String asKey, String asName, String asInfo)
	_toggleKeys[i] = asKey
	_toggleNames[i] = asName
	_toggleInfo[i] = asInfo
EndFunction

Function AddToggle(Int i)
	_toggleOIDs[i] = AddToggleOption(_toggleNames[i], FWB_Native.GetInt(_toggleKeys[i]) != 0)
EndFunction

Int Function FindToggle(Int a_option)
	Int i = 0
	While i < _toggleOIDs.Length
		If _toggleOIDs[i] == a_option
			Return i
		EndIf
		i += 1
	EndWhile
	Return -1
EndFunction

Event OnPageReset(String a_page)
	SetCursorFillMode(TOP_TO_BOTTOM)
	Int i = 0
	While i < _toggleOIDs.Length
		_toggleOIDs[i] = -1
		i += 1
	EndWhile
	_distanceOID = -1
	_combatDistanceOID = -1
	_retreatDistanceOID = -1

	If a_page == "Combat"
		AddHeaderOption("Awareness")
		AddToggle(3)
		AddToggle(4)
		AddToggle(5)
		AddToggle(6)
		AddHeaderOption("Weapons")
		AddToggle(9)
		AddToggle(10)
		AddHeaderOption("Retreat")
		AddToggle(7)
		_retreatDistanceOID = AddSliderOption("Ranged keep shooting within", FWB_Native.GetFloat("RangedRetreatDistance"), "{0} units")
	Else
		AddHeaderOption("Following")
		_distanceOID = AddMenuOption("Follow distance", _distanceNames[FWB_Native.GetInt("FollowDistance")])
		_combatDistanceOID = AddMenuOption("Combat distance", _distanceNames[FWB_Native.GetInt("CombatDistance")])
		AddToggle(0)
		AddToggle(1)
		AddToggle(2)
		AddHeaderOption("Other")
		AddToggle(8)
	EndIf
EndEvent

Event OnOptionSelect(Int a_option)
	Int i = FindToggle(a_option)
	If i >= 0
		Bool value = FWB_Native.GetInt(_toggleKeys[i]) == 0
		FWB_Native.SetInt(_toggleKeys[i], value as Int)
		FWB_Native.Save()
		SetToggleOptionValue(a_option, value)
	EndIf
EndEvent

Event OnOptionMenuOpen(Int a_option)
	If a_option == _distanceOID
		SetMenuDialogOptions(_distanceNames)
		SetMenuDialogStartIndex(FWB_Native.GetInt("FollowDistance"))
		SetMenuDialogDefaultIndex(0)
	ElseIf a_option == _combatDistanceOID
		SetMenuDialogOptions(_distanceNames)
		SetMenuDialogStartIndex(FWB_Native.GetInt("CombatDistance"))
		SetMenuDialogDefaultIndex(0)
	EndIf
EndEvent

Event OnOptionMenuAccept(Int a_option, Int a_index)
	If a_option == _distanceOID && a_index >= 0
		FWB_Native.SetInt("FollowDistance", a_index)
		FWB_Native.Save()
		SetMenuOptionValue(a_option, _distanceNames[a_index])
	ElseIf a_option == _combatDistanceOID && a_index >= 0
		FWB_Native.SetInt("CombatDistance", a_index)
		FWB_Native.Save()
		SetMenuOptionValue(a_option, _distanceNames[a_index])
	EndIf
EndEvent

Event OnOptionSliderOpen(Int a_option)
	If a_option == _retreatDistanceOID
		SetSliderDialogStartValue(FWB_Native.GetFloat("RangedRetreatDistance"))
		SetSliderDialogDefaultValue(1200.0)
		SetSliderDialogRange(300.0, 4000.0)
		SetSliderDialogInterval(100.0)
	EndIf
EndEvent

Event OnOptionSliderAccept(Int a_option, Float a_value)
	If a_option == _retreatDistanceOID
		FWB_Native.SetFloat("RangedRetreatDistance", a_value)
		FWB_Native.Save()
		SetSliderOptionValue(a_option, a_value, "{0} units")
	EndIf
EndEvent

Event OnOptionHighlight(Int a_option)
	Int i = FindToggle(a_option)
	If i >= 0
		SetInfoText(_toggleInfo[i])
	ElseIf a_option == _distanceOID
		SetInfoText("Out of combat. Close: followers stay right next to you and move the moment you do.\nMedium: a looser follow.\nFar: the game's own follow distance.")
	ElseIf a_option == _combatDistanceOID
		SetInfoText("How far from you followers fight. Close: only enemies near you. Far: they range further. For 5 seconds after you heal a follower, they may go one step further.")
	ElseIf a_option == _retreatDistanceOID
		SetInfoText("While you retreat, a ranged follower closer to you than this keeps fighting as it backs off. Farther away, it just runs to you.")
	EndIf
EndEvent
