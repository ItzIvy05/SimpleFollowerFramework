Scriptname SFF_HomeTopicScript Extends TopicInfo Hidden

Int Property HomeAction Auto
Quest Property HomeQuest Auto
Faction Property HomeFaction Auto

Function Fragment_0(ObjectReference akSpeakerRef)
	Actor who = akSpeakerRef as Actor
	Int i = 0
	While i < 8
		ReferenceAlias slot = HomeQuest.GetAlias(i) as ReferenceAlias
		If slot.GetActorRef() == who
			slot.Clear()
		EndIf
		i += 1
	EndWhile
	who.RemoveFromFaction(HomeFaction)
	i = 0
	While HomeAction == 0 && i < 8
		ReferenceAlias slot = HomeQuest.GetAlias(i) as ReferenceAlias
		If !slot.GetActorRef()
			(HomeQuest.GetAlias(i + 8) as ReferenceAlias).GetReference().MoveTo(who)
			slot.ForceRefTo(who)
			who.AddToFaction(HomeFaction)
			i = 8
		EndIf
		i += 1
	EndWhile
	who.EvaluatePackage()
EndFunction