Scriptname FWB_MCM extends SKI_ConfigBase
{Settings menu for Followers With Brains. Values live in the SKSE plugin (FWB_Native).}

String[] _distanceNames
String[] _placementNames
Int _combatDistanceOID
Int _followDistanceOID
Int _placementOID
Int _retreatDistanceOID

String[] _toggleKeys
String[] _toggleNames
String[] _toggleInfo
Int[] _toggleOIDs

Int Function GetVersion()
	Return 7
EndFunction

Event OnVersionUpdate(Int a_version)
	OnConfigInit()
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

	_placementNames = new String[4]
	_placementNames[0] = "Behind you"
	_placementNames[1] = "At your side"
	_placementNames[2] = "Ahead of you"
	_placementNames[3] = "Around you"

	_toggleKeys = new String[12]
	_toggleNames = new String[12]
	_toggleInfo = new String[12]
	_toggleOIDs = new Int[12]
	SetToggle(0, "CombatMovement", "Control movement in combat", "The mod moves followers in fights: charging shooters, stepping out of big hits, keeping to the combat distance, retreating with you and taking your route to enemies the game can't path to. Off: the game's own combat movement.")
	SetToggle(1, "OutsideMovement", "Control movement out of combat", "The mod moves followers outside fights: keeping to the following distance and formation, catching up, jumping after you, taking your route and relaxing in safe places. Off: the game's own following.")
	SetToggle(2, "Awareness", "Combat awareness", "Followers get ready when you aim at or watch an enemy, attack the moment you do, and join fights you are in.")
	SetToggle(3, "Retarget", "Next target right away", "When their target dies, followers move straight on to the next enemy fighting you.")
	SetToggle(4, "FocusSummoners", "Focus summoners", "Followers go for the summoner instead of the summon when they can reach them. If the summon keeps hitting them, they fight it.")
	SetToggle(5, "MeleeSwitch", "Switch target after misses", "A melee follower who misses twice while another enemy hits them switches to that enemy.")
	SetToggle(6, "Reflexes", "Reflexes", "The moment an enemy starts a big attack on them, followers step out of reach and strike back as it ends. They also strike at once when an enemy staggers.")
	SetToggle(7, "Zigzag", "Charge shooters", "Melee followers sprint at archers and casters on a clear, level line instead of walking in under fire.")
	SetToggle(8, "SmartWeapons", "Ranged or melee by situation", "Followers who carry both pick by damage and distance: fighters charge and shoot only at enemies they can't reach; archers and mages switch to melee only when an enemy is in their face.")
	SetToggle(9, "MatchWeapons", "Match your weapon", "Out of combat, when you draw a bow or an attack spell your followers draw their ranged weapon, and switch back when you draw a melee weapon.")
	SetToggle(10, "Retreat", "Retreat with you", "When you sprint away from a fight for 5 seconds without attacking, followers come with you.")
	SetToggle(11, "Debug", "Debug messages", "Show what followers decide, as notifications.")
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

Int Function FlagIf(Bool abEnabled)
	If abEnabled
		Return OPTION_FLAG_NONE
	EndIf
	Return OPTION_FLAG_DISABLED
EndFunction

Event OnPageReset(String a_page)
	If !_toggleKeys || _toggleKeys.Length != 12 || !_placementNames
		OnConfigInit()
	EndIf
	SetCursorFillMode(TOP_TO_BOTTOM)
	Int i = 0
	While i < _toggleOIDs.Length
		_toggleOIDs[i] = -1
		i += 1
	EndWhile
	_combatDistanceOID = -1
	_followDistanceOID = -1
	_placementOID = -1
	_retreatDistanceOID = -1

	If a_page == "Combat"
		AddHeaderOption("Awareness")
		AddToggle(2)
		AddToggle(3)
		AddToggle(4)
		AddToggle(5)
		AddHeaderOption("Moves")
		AddToggle(6)
		AddToggle(7)
		AddHeaderOption("Weapons")
		AddToggle(8)
		AddToggle(9)
		AddHeaderOption("Retreat")
		AddToggle(10)
		_retreatDistanceOID = AddSliderOption("Ranged keep shooting within", FWB_Native.GetFloat("RangedRetreatDistance"), "{0} units")
		AddHeaderOption("Other")
		AddToggle(11)
	Else
		Bool combat = FWB_Native.GetInt("CombatMovement") != 0
		Bool outside = FWB_Native.GetInt("OutsideMovement") != 0
		Int follow = FWB_Native.GetInt("FollowDistance")
		AddHeaderOption("In combat")
		AddToggle(0)
		_combatDistanceOID = AddMenuOption("Combat distance", _distanceNames[FWB_Native.GetInt("CombatDistance")], FlagIf(combat))
		AddHeaderOption("Out of combat")
		AddToggle(1)
		_followDistanceOID = AddMenuOption("Following distance", _distanceNames[follow], FlagIf(outside))
		_placementOID = AddMenuOption("Formation", _placementNames[FWB_Native.GetInt("Placement")], FlagIf(outside && follow == 0))
	EndIf
EndEvent

Event OnOptionSelect(Int a_option)
	Int i = FindToggle(a_option)
	If i >= 0
		Bool value = FWB_Native.GetInt(_toggleKeys[i]) == 0
		FWB_Native.SetInt(_toggleKeys[i], value as Int)
		FWB_Native.Save()
		SetToggleOptionValue(a_option, value)
		If i <= 1
			ForcePageReset()
		EndIf
	EndIf
EndEvent

Event OnOptionMenuOpen(Int a_option)
	If a_option == _combatDistanceOID
		SetMenuDialogOptions(_distanceNames)
		SetMenuDialogStartIndex(FWB_Native.GetInt("CombatDistance"))
		SetMenuDialogDefaultIndex(0)
	ElseIf a_option == _followDistanceOID
		SetMenuDialogOptions(_distanceNames)
		SetMenuDialogStartIndex(FWB_Native.GetInt("FollowDistance"))
		SetMenuDialogDefaultIndex(0)
	ElseIf a_option == _placementOID
		SetMenuDialogOptions(_placementNames)
		SetMenuDialogStartIndex(FWB_Native.GetInt("Placement"))
		SetMenuDialogDefaultIndex(0)
	EndIf
EndEvent

Event OnOptionMenuAccept(Int a_option, Int a_index)
	If a_index < 0
		Return
	EndIf
	If a_option == _combatDistanceOID
		FWB_Native.SetInt("CombatDistance", a_index)
		FWB_Native.Save()
		SetMenuOptionValue(a_option, _distanceNames[a_index])
	ElseIf a_option == _followDistanceOID
		FWB_Native.SetInt("FollowDistance", a_index)
		FWB_Native.Save()
		ForcePageReset()
	ElseIf a_option == _placementOID
		FWB_Native.SetInt("Placement", a_index)
		FWB_Native.Save()
		SetMenuOptionValue(a_option, _placementNames[a_index])
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
	ElseIf a_option == _combatDistanceOID
		SetInfoText("How far from you followers fight. While they are above half health or being healed they may go much further.")
	ElseIf a_option == _followDistanceOID
		SetInfoText("Close: followers stay right by you and move the moment you do.\nMedium: around you, further out.\nFar: the game's own follow distance.")
	ElseIf a_option == _placementOID
		SetInfoText("Where followers stay with Close following: behind you, at your side, ahead of you, or anywhere around you. Each is an area, not a fixed spot.")
	ElseIf a_option == _retreatDistanceOID
		SetInfoText("While you retreat, a ranged follower closer to you than this keeps fighting as it backs off. Farther away, it just runs to you.")
	EndIf
EndEvent
