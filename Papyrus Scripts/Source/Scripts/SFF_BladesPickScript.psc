Scriptname SFF_BladesPickScript Extends TopicInfo Hidden

Int Property SlotIndex Auto
Quest Property FreeformSkyhavenTempleA Auto
Quest Property DialogueFollower Auto

Function Fragment_0(ObjectReference akSpeakerRef)
	Actor chosen = (DialogueFollower.GetAlias(SlotIndex) as ReferenceAlias).GetActorRef()
	If !chosen
		Return
	EndIf
	(DialogueFollower as DialogueFollowerScript).SFF_SetLastSpeaker(chosen)
	(FreeformSkyhavenTempleA as FreeformSkyHavenTempleAScript).RecruitBlade(chosen)
EndFunction