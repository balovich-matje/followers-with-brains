Scriptname FWB_MCM extends SKI_ConfigBase

String[] _distanceNames
Int _distanceOID

Event OnConfigInit()
	ModName = "Followers With Brains"
	Pages = new String[1]
	Pages[0] = "Movement"
	_distanceNames = new String[3]
	_distanceNames[0] = "Close"
	_distanceNames[1] = "Medium"
	_distanceNames[2] = "Far"
EndEvent

Event OnPageReset(String a_page)
	SetCursorFillMode(TOP_TO_BOTTOM)
	AddHeaderOption("Following")
	_distanceOID = AddMenuOption("Follow distance", _distanceNames[FWB_Native.GetInt("FollowDistance")])
EndEvent

Event OnOptionMenuOpen(Int a_option)
	If a_option == _distanceOID
		SetMenuDialogOptions(_distanceNames)
		SetMenuDialogStartIndex(FWB_Native.GetInt("FollowDistance"))
		SetMenuDialogDefaultIndex(0)
	EndIf
EndEvent

Event OnOptionMenuAccept(Int a_option, Int a_index)
	If a_option == _distanceOID
		FWB_Native.SetInt("FollowDistance", a_index)
		FWB_Native.Save()
		SetMenuOptionValue(a_option, _distanceNames[a_index])
	EndIf
EndEvent

Event OnOptionHighlight(Int a_option)
	If a_option == _distanceOID
		SetInfoText("Close: followers stay right next to you and move the moment you do.\nMedium: a looser follow.\nFar: the game's own follow distance.")
	EndIf
EndEvent
