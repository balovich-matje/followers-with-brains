Scriptname FWB_Actions Hidden
{Engine actions requested by the Followers With Brains SKSE plugin.}

Function StartCombat(Actor akActor, Actor akTarget) Global
	If akActor && akTarget
		akActor.StartCombat(akTarget)
	EndIf
EndFunction

Function StopCombat(Actor akActor) Global
	If akActor
		akActor.StopCombat()
	EndIf
EndFunction

Function KeepOffset(Actor akActor, Actor akTarget, Float afX, Float afY, Float afCatchUp, Float afFollow) Global
	If akActor && akTarget
		akActor.KeepOffsetFromActor(akTarget, afX, afY, 0.0, 0.0, 0.0, 0.0, afCatchUp, afFollow)
	EndIf
EndFunction

Function ClearOffset(Actor akActor) Global
	If akActor
		akActor.ClearKeepOffsetFromActor()
	EndIf
EndFunction

Function MoveTo(Actor akActor, Actor akTarget, Float afX, Float afY) Global
	If akActor && akTarget
		akActor.MoveTo(akTarget, afX, afY, 0.0, False)
	EndIf
EndFunction
