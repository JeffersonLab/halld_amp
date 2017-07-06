#include "ANALYSIS/DSourceComboer.h"
#include "ANALYSIS/DSourceComboVertexer.h"
#include "ANALYSIS/DSourceComboTimeHandler.h"

namespace DAnalysis
{

//Abandon all hope, ye who enter here.

/****************************************************** COMBOING STRATEGY ******************************************************
*
* Creating all possible combos can be very time- and memory-intensive if not done properly.
* For example, consider a 4pi0 analysis and 20 (N) reconstructed showers (it happens).
* If you make all possible pairs of photons (for pi0's), you get 19 + 18 + 17 + ... 1 = (N - 1)*N/2 = 190 pi0 combos.
* Now, consider that you have 4 pi0s: On the order of 190^4/16: On the order of a 80 million combos (although less once you guard against photon reuse)
*
* So, we must do everything we can to reduce the # of possible combos in ADVANCE of actually attempting to make them.
* And, we have to make sure we don't do anything twice (e.g. two different users have 4pi0s in their channel).
* The key to this being efficient (besides splitting the BCAL photons into vertex-z bins and placing timing cuts) is combo re-use.
*
* For example, suppose a channel needs 3 pi0s.
* First this will build all combos for 1 pi0, then all combos for 2 pi0s, then 3.  Placing mass cuts along the way.
* The results after each of these steps is saved.  That way, if someone then requests 2 pi0s, we merely have to return the results from the previous work.
* Also, if someone later requests 4pi0s, then we just take the 3pi0 results and expand them by 1 pi0.
* Or, if someone requests p3pi, we take the 1 pi0 combos and combine them with a proton, pi+, and pi-.  Etc., etc.
*
* For more details on how this is done, see the comments in the Create_SourceCombos_Unknown function
* But ultimately, this results in a clusterfuck of recursive calls.
* Also, because of how the combo-info classes are structured (decaying PID NOT a member), you have be extremely careful not to get into an infinite loop.
* So, modify this code at your own peril. Just try not to take the rest of the collaboration down with you.
*
* Now, technically, when we construct combos for a (e.g.) pi0, we are saving 2 different results:
*    The combos of 2 photons, and which of those combos survive the pi0 mass cut.
* That way, if later someone wants to build an eta, all we have to do is take 2-photon combos and place eta mass cuts.
*
* Combos are created on-demand, used immediately, and once they are cut the memory is recycled for the next combo in that event.
*
*
* The BCAL photons are evaluated in different vertex-z bins for calculating their kinematics (momentum & timing).
* This is because their kinematics have a strong dependence on vertex-z, while the FCAL showers do not (see above derivations).
* Whereas the FCAL photons have only a small dependence, so their kinematics are regardless of vertex-z.
* For more discussion the above, see the derivations in the DSourceComboTimeHandler and DSourceComboP4Handler classes.
*
*
*
*
*
* Note that combos are constructed separately for different beam bunches.
* This is because photons only survive their timing cuts for certain beam bunches.
* Comboing only within a given beam bunch reduces the #photons we need to combo, and is thus faster.
*
* When comboing, first all of the FCAL showers alone are used to build the requested combos.
* Then, the BCAL showers surviving the timing cuts within the input vertex-z bin are used to build the requested combos.
* Finally, combos are created using a mix of these BCAL & FCAL showers.
* The results from this comboing is saved for all cases, that way they can be easily retrieved and combined as needed for similar requests.
*
*
*******************************************************************************************************************************/

/****************************************************** DESIGN MOTIVATION ******************************************************
*
*
*
* 1) Re-use comboing results between DReactions.
*    If working on each DReaction individually, it is difficult (takes time & memory) to figure out what has already been done, and what to share
*    So instead, first break down the DReactions to their combo-building components, and share those components.
*    Then build combos out of the components, and distribute the results for each DReaction.
*
* 2) Reduce the time spent trying combos that we can know in advance won't work.
*    We can do this by placing cuts IMMEDIATELY on:
*    a) Time difference between charged tracks
*    b) Time difference between photons and possible RF bunches (discussed more below).
*    c) Invariant mass cuts for various decaying particles (e.g. pi0, eta, omega, phi, lambda, etc.)
*    Also, when building combos of charged tracks, we could only loop over PIDs of the right type, rather than all hypotheses
*
* 3) The only way to do both 1) and 2) is to make the loose time & mass cuts reaction-independent.
*    Users can always specify reaction-dependent tighter cuts later, but they cannot specify looser ones.
*    However, these cuts should be tweakable on the command line in case someone wants to change them.
*
*******************************************************************************************************************************/


/*****
 * COMBOING PHOTONS AND RF BUNCHES
 *
 * So, this is tricky.
 * Start out by allowing ALL beam bunches, regardless of what the charged tracks want.
 * Then, as each photon is chosen, reduce the set of possible photons to choose next: only those that agree on at least one RF bunch
 * As combos are made, the valid RF bunches are saved along with the combo
 * That way, as combos are combined with other combos/particles, we make sure that only valid possibilities are chosen.
 *
 * We can't start with those only valid for the charged tracks because:
 * When we generate combos for a given info, we want to generate ALL combos at once.
 * E.g. some charged tracks may want pi0s with beam bunch = 1, but another group might want pi0s with bunch 1 OR 2.
 * Dealing with the overlap is a nightmare.  This avoids the problem entirely.
 *
 * BEWARE: Massive-neutral-particle momentum depends on the RF bunch. So a cut on the invariant mass with a neutron is effectively a cut on the RF bunches
 * Suppose: Sigma+ -> pi+ n
 * You first generate combos for -> pi+ n, and save them for the use X -> pi+, n
 * We then re-use the combos for the use Sigma+ -> pi+ n
 * But then a cut on the Sigma+ mass reduces the #valid RF bunches. So now we need a new combo!
 * We could decouple the RF bunches from the combo: e.g. save in map from combo_use -> rf bunches
 * However, this would result in many duplicate entries: e.g. X -> 2g, pi0 -> 2g, eta -> 2g, etc.
 * Users choosing final-state neutrons or KLongs is pretty rare compared to everything else: we are better off just creating new combos
 *
 * BEWARE: Massive-neutral-particle momentum depends on the RF bunch. So a cut on the invariant mass with a neutron is effectively a cut on the RF bunches.
 * So we can't actually vote on RF bunches until we choose our massive-neutral particles!!!
 */

/********************************************************************* CONSTRUCTOR **********************************************************************/

DSourceComboer::DSourceComboer(JEventLoop* locEventLoop)
{
	//GET THE GEOMETRY
	DApplication* locApplication = dynamic_cast<DApplication*>(locEventLoop->GetJApplication());
	DGeometry* locGeometry = locApplication->GetDGeometry(locEventLoop->GetJEvent().GetRunNumber());

	//TARGET INFORMATION
	double locTargetCenterZ = 65.0;
	locGeometry->GetTargetZ(locTargetCenterZ);
	dTargetCenter.SetXYZ(0.0, 0.0, locTargetCenterZ);
	double locTargetLength;
	locGeometry->GetTargetLength(locTargetLength);

	//INITIALIZE PHOTON VERTEX-Z EVALUATION BINNING
	//MAKE SURE THAT THE CENTER OF THE TARGET IS THE CENTER OF A BIN
	//this is a little convoluted (and can probably be calculated without loops ...), but it ensures the above
	dPhotonVertexZBinWidth = 10.0;
	size_t locN = 0;
	double locTargetUpstreamZ = dTargetCenter.Z() - locTargetLength/2.0;
	double locTargetDownstreamZ = dTargetCenter.Z() + locTargetLength/2.0;
	do
	{
		++locN;
		dPhotonVertexZRangeLow = dTargetCenter.Z() - double(locN)*dPhotonVertexZBinWidth;
	}
	while(dPhotonVertexZRangeLow + dPhotonVertexZBinWidth > locTargetUpstreamZ);
	while(dPhotonVertexZRangeLow + locN*dPhotonVertexZBinWidth <= locTargetDownstreamZ)
		++locN;
	dNumPhotonVertexZBins = locN + 1; //one extra, for detached vertices

	//Get preselect tag, debug level
	gPARMS->SetDefaultParameter("COMBO:SHOWER_SELECT_TAG", dShowerSelectionTag);
	gPARMS->SetDefaultParameter("COMBO:DEBUG_LEVEL", dDebugLevel);

	//GET THE REACTIONS
	auto locReactions = DAnalysis::Get_Reactions(locEventLoop);

	//CREATE DSourceComboINFO'S
	vector<const DReactionVertexInfo*> locVertexInfos;
	locEventLoop->Get(locVertexInfos);
	for(const auto& locVertexInfo : locVertexInfos)
		Create_SourceComboInfos(locVertexInfo);

	//TRANSFER INFOS FROM SET TO VECTOR
	dSourceComboInfos.reserve(dSourceComboInfoSet.size());
	std::copy(dSourceComboInfoSet.begin(), dSourceComboInfoSet.end(), std::back_inserter(dSourceComboInfos));
	dSourceComboInfoSet.clear(); //free up the memory

	//CREATE HANDLERS
	dSourceComboP4Handler = new DSourceComboP4Handler(locEventLoop, this);
	dSourceComboVertexer = new DSourceComboVertexer(locEventLoop, this, dSourceComboP4Handler);
	dSourceComboTimeHandler = new DSourceComboTimeHandler(locEventLoop, this, dSourceComboVertexer);
	dSourceComboP4Handler->Set_SourceComboTimeHandler(dSourceComboTimeHandler);
	dSourceComboP4Handler->Set_SourceComboVertexer(dSourceComboVertexer);
	dSourceComboVertexer->Set_SourceComboTimeHandler(dSourceComboTimeHandler);
	dParticleComboCreator = new DParticleComboCreator(locEventLoop, this, dSourceComboTimeHandler, dSourceComboVertexer);

	//save rf bunch cuts
	if(gPARMS->Exists("COMBO:NUM_PLUSMINUS_RF_BUNCHES"))
	{
		size_t locNumPlusMinusRFBunches;
		gPARMS->GetParameter("COMBO:NUM_PLUSMINUS_RF_BUNCHES", locNumPlusMinusRFBunches);
		for(const auto& locReaction : locReactions)
			dRFBunchCutsByReaction.emplace(locReaction, locNumPlusMinusRFBunches);
	}
	else //by reaction
	{
		for(const auto& locReaction : locReactions)
		{
			auto locNumBunches = locReaction->Get_NumPlusMinusRFBunches();
			pair<bool, double> locMaxPhotonRFDeltaT = locReaction->Get_MaxPhotonRFDeltaT(); //DEPRECATED!!!
			if(locMaxPhotonRFDeltaT.first)
				locNumBunches = size_t(locMaxPhotonRFDeltaT.second/dSourceComboTimeHandler->Get_BeamBunchPeriod() - 0.50001);
			dRFBunchCutsByReaction.emplace(locReaction, locNumBunches);
		}
	}

	//save max bunch cuts
	for(const auto& locVertexInfo : locVertexInfos)
	{
		dMaxRFBunchCuts.emplace(locVertexInfo, 0);
		for(const auto& locReaction : locReactions)
		{
			if(dRFBunchCutsByReaction[locReaction] > dMaxRFBunchCuts[locVertexInfo])
				dMaxRFBunchCuts[locVertexInfo] = dRFBunchCutsByReaction[locReaction];
		}
	}
}

/******************************************************************* CREATE DSOURCOMBOINFO'S ********************************************************************/

void DSourceComboer::Create_SourceComboInfos(const DReactionVertexInfo* locReactionVertexInfo)
{
	//FULL combo use: Segregate each step into (up to) 3 combos: a fully charged, a fully neutral, and a mixed
	//that way we will combo each separately before combining them horizontally: maximum re-use, especially of time-intensive neutral comboing
		//However, an exception: if a any-# of a single neutral PID (e.g. pi0 or g), promote it to the level where the charged/neutral/mixed are combined
		//Charged is similar, but not the same: if a single DECAYING-to-charged particle, promote it as well
		//Not so for a single detected charged particle though: We want to keep charged separate because that's what defines the vertices: Easier lookup

	/*
	 * suppose reaction is 0) g, p -> omega, p
	 *                     1)         omega -> 3pi
	 *                     2)                   pi0 -> 2g
	 *
	 * It will have uses/infos like:
	 * 0: X -> A, 1 (mixed + charged)
	 *    A: X -> p (charged)
	 * 	1: omega -> B, 2 (mixed)
	 *    	B: X -> pi+, pi- (charged)
	 * 		2: pi0 -> 2g (neutral)
	 */

	if(dDebugLevel > 0)
		cout << "CREATING DSourceComboInfo OBJECTS FOR DREACTION " << locReactionVertexInfo->Get_Reaction()->GetName() << endl;

	//We will register what steps these combos are created for
	map<size_t, DSourceComboUse> locStepComboUseMap; //size_t = step index

	//loop over steps in reverse order
	auto locReaction = locReactionVertexInfo->Get_Reaction();
	auto locReactionSteps = locReaction->Get_ReactionSteps();
	for(auto locStepIterator = locReactionSteps.rbegin(); locStepIterator != locReactionSteps.rend(); ++locStepIterator)
	{
		auto locStep = *locStepIterator;
		auto locStepIndex = locReaction->Get_NumReactionSteps() - std::distance(locReactionSteps.rbegin(), locStepIterator) - 1;
		if(dDebugLevel >= 5)
			cout << "Step index " << locStepIndex << endl;

		//create combo uses for all charged, all neutral, then for any mixed decays
		map<Particle_t, unsigned char> locChargedParticleMap = Build_ParticleMap(locReaction, locStepIndex, d_Charged);
		map<Particle_t, unsigned char> locNeutralParticleMap = Build_ParticleMap(locReaction, locStepIndex, d_Neutral);

		//get combo infos for final-state decaying particles //if not present, ignore parent
		auto locFinalStateDecayingComboUsesPair = Get_FinalStateDecayingComboUses(locReaction, locStepIndex, locStepComboUseMap);
		auto locIncludeParentFlag = locFinalStateDecayingComboUsesPair.first;
		auto& locFurtherDecays = locFinalStateDecayingComboUsesPair.second;

		//split up further-decays into all-charged, all-neutral, and mixed
		map<DSourceComboUse, unsigned char> locFurtherDecays_Charged, locFurtherDecays_Neutral, locFurtherDecays_Mixed;
		for(const auto& locDecayPair : locFurtherDecays)
		{
			auto locChargeContent = dComboInfoChargeContent[std::get<2>(locDecayPair.first)];
			if(locChargeContent == d_Charged)
				locFurtherDecays_Charged.emplace(locDecayPair);
			else if(locChargeContent == d_Neutral)
				locFurtherDecays_Neutral.emplace(locDecayPair);
			else
				locFurtherDecays_Mixed.emplace(locDecayPair);
		}

		//determine whether to include the decay itself in the comboing (or just the products)
		//only include if can make an invariant mass cut (what it's used for here)
		//we will still group these separately from the rest of the particles
		if((locStepIndex != 0) || !DAnalysis::Get_IsFirstStepBeam(locReaction)) //decay
		{
			//ignore parent if products include missing particles
			if(DAnalysis::Check_IfMissingDecayProduct(locReaction, locStepIndex))
				locIncludeParentFlag = false;
		}
		else //direct production
			locIncludeParentFlag = false;

		//create combo uses for each case
		Particle_t locInitPID = locIncludeParentFlag ? locStep->Get_InitialPID() : Unknown;
		bool locNoChargedFlag = (locChargedParticleMap.empty() && locFurtherDecays_Charged.empty());
		bool locNoNeutralFlag = (locNeutralParticleMap.empty() && locFurtherDecays_Neutral.empty());

		DSourceComboUse locPrimaryComboUse(Unknown, DSourceComboInfo::Get_VertexZIndex_ZIndependent(), nullptr);
		if(locNoChargedFlag && locNoNeutralFlag) //only mixed
			locPrimaryComboUse = Make_ComboUse(locInitPID, {}, locFurtherDecays_Mixed);
		else if(locNoNeutralFlag && locFurtherDecays_Mixed.empty()) //only charged
			locPrimaryComboUse = Make_ComboUse(locInitPID, locChargedParticleMap, locFurtherDecays_Charged);
		else if(locNoChargedFlag && locFurtherDecays_Mixed.empty()) //only neutral
			locPrimaryComboUse = Make_ComboUse(locInitPID, locNeutralParticleMap, locFurtherDecays_Neutral);
		else //some combination
		{
			auto locFurtherDecays_All = locFurtherDecays_Mixed;
			map<Particle_t, unsigned char> locParticleMap_All = {};
			//create a combo for each charged group, with init pid = unknown
			if(!locNoChargedFlag)
			{
				//if lone Charged decaying particle, promote to be parallel with mixed
				if(locChargedParticleMap.empty() && (locFurtherDecays_Charged.size() == 1) && (locFurtherDecays_Charged.begin()->second == 1))
					locFurtherDecays_All.emplace(locFurtherDecays_Charged.begin()->first, 1);
				else //multiple Charged decaying particles, group together separately (own use)
				{
					auto locComboUse_Charged = Make_ComboUse(Unknown, locChargedParticleMap, locFurtherDecays_Charged);
					locFurtherDecays_All.emplace(locComboUse_Charged, 1);
				}
			}
			if(!locNoNeutralFlag)
			{
				//if lone neutral PID, promote to be parallel with mixed
				if(locNeutralParticleMap.empty() && (locFurtherDecays_Neutral.size() == 1))
					locFurtherDecays_All.emplace(locFurtherDecays_Neutral.begin()->first, locFurtherDecays_Neutral.begin()->second); //decaying
				else if(locFurtherDecays_Neutral.empty() && (locNeutralParticleMap.size() == 1))
					locParticleMap_All.emplace(locNeutralParticleMap.begin()->first, locNeutralParticleMap.begin()->second); //detected
				else //multiple neutral particles, group together separately (own use)
				{
					auto locComboUse_Neutral = Make_ComboUse(Unknown, locNeutralParticleMap, locFurtherDecays_Neutral);
					locFurtherDecays_All.emplace(locComboUse_Neutral, 1);
				}
			}

			locPrimaryComboUse = Make_ComboUse(locInitPID, locParticleMap_All, locFurtherDecays_All);
		}

		locStepComboUseMap.emplace(locStepIndex, locPrimaryComboUse);
	}

	//Register the results!!
	for(const auto& locStepVertexInfo : locReactionVertexInfo->Get_StepVertexInfos())
		dSourceComboUseReactionMap.emplace(locStepVertexInfo, locStepComboUseMap[locStepVertexInfo->Get_StepIndices().front()]);
	for(const auto& locUseStepPair : locStepComboUseMap)
		dSourceComboInfoStepMap.emplace(std::make_pair(locReactionVertexInfo->Get_StepVertexInfo(locUseStepPair.first), locUseStepPair.second), locUseStepPair.first);
	dSourceComboUseReactionStepMap.emplace(locReaction, locStepComboUseMap);

	if(dDebugLevel > 0)
		cout << "DSourceComboInfo OBJECTS CREATED" << endl;
}

DSourceComboUse DSourceComboer::Create_ZDependentSourceComboUses(const DReactionVertexInfo* locReactionVertexInfo, const DSourceCombo* locReactionChargedCombo)
{
	//this creates new uses, with the specific vertex-z bins needed
	//note that the use can have a different structure from the charged!! (although not likely)
	//E.g. if something crazy like 2 KShorts -> 3pi, each at a different vertex-z bin, then they will no longer be grouped together vertically (separate uses: horizontally instead)

	//see if they've already been created.  if so, just return it.
	auto locIsPrimaryProductionVertex = locReactionVertexInfo->Get_StepVertexInfos().front()->Get_ProductionVertexFlag();
	auto locVertexZBins = dSourceComboVertexer->Get_VertexZBins(locIsPrimaryProductionVertex, locReactionChargedCombo, nullptr);
	auto locCreationPair = std::make_pair(locReactionVertexInfo, locVertexZBins);
	auto locUseIterator = dSourceComboUseVertexZMap.find(locCreationPair);
	if(locUseIterator != dSourceComboUseVertexZMap.end())
		return locUseIterator->second; //already created! we are done

	auto locReaction = locReactionVertexInfo->Get_Reaction();

	//loop over vertex infos in reverse-step order
	unordered_map<size_t, DSourceComboUse> locCreatedUseMap; //size_t: step index
	auto locStepVertexInfos = DAnalysis::Get_StepVertexInfos_ReverseOrderByStep(locReactionVertexInfo);
	for(const auto& locStepVertexInfo : locStepVertexInfos)
	{
		auto locVertexPrimaryCombo = (locReactionChargedCombo != nullptr) ? Get_VertexPrimaryCombo(locReactionChargedCombo, locStepVertexInfo) : nullptr;

		//for this vertex, get the vertex z bin
		auto locIsProductionVertex = locStepVertexInfo->Get_ProductionVertexFlag();
		auto locVertexZBin = (locReactionChargedCombo != nullptr) ? dSourceComboVertexer->Get_VertexZBin(locIsProductionVertex, locVertexPrimaryCombo, nullptr) : Get_VertexZBin_TargetCenter();

		//loop over the steps at this vertex z bin, in reverse order
		auto locStepIndices = locStepVertexInfo->Get_StepIndices();
		for(auto locStepIterator = locStepIndices.rbegin(); locStepIterator != locStepIndices.rend(); ++locStepIterator)
		{
			auto locStepIndex = *locStepIterator;
			auto locStepOrigUse = dSourceComboUseReactionStepMap[locReaction][locStepIndex];

			//build new use for the further decays, setting the vertex z-bins
			auto locNewComboUse = Build_NewZDependentUse(locReaction, locStepIndex, locVertexZBin, locStepOrigUse, locCreatedUseMap);
			locCreatedUseMap.emplace(locStepIndex, locNewComboUse);
		}
	}

	dSourceComboUseVertexZMap.emplace(locCreationPair, locCreatedUseMap[0]);
	return locCreatedUseMap[0];
}

DSourceComboUse DSourceComboer::Build_NewZDependentUse(const DReaction* locReaction, size_t locStepIndex, signed char locVertexZBin, const DSourceComboUse& locOrigUse, const unordered_map<size_t, DSourceComboUse>& locCreatedUseMap)
{
	//each step can be broken up into combo infos with a depth of 2 (grouping charges separately)
	auto locStep = locReaction->Get_ReactionStep(locStepIndex);
	auto locOrigInfo = std::get<2>(locOrigUse);
	if(dComboInfoChargeContent[locOrigInfo] == d_Charged)
	{
		dZDependentUseToIndependentMap.emplace(locOrigUse, locOrigUse);
		return locOrigUse; //no need to change!: no neutrals anyway
	}

	map<DSourceComboUse, unsigned char> locNewFurtherDecays;
	auto locOrigFurtherDecays = locOrigInfo->Get_FurtherDecays();
	for(const auto& locDecayPair : locOrigFurtherDecays)
	{
		const auto& locOrigDecayUse = locDecayPair.first;
		auto locDecayPID = std::get<0>(locOrigDecayUse);
		if(locDecayPID != Unknown)
		{
			//these decays are represented by other steps, and have already been saved
			for(unsigned char locInstance = 1; locInstance <= locDecayPair.second; ++locInstance)
			{
				auto locParticleIndex = DAnalysis::Get_ParticleIndex(locStep, locDecayPID, locInstance);
				auto locDecayStepIndex = DAnalysis::Get_DecayStepIndex(locReaction, locStepIndex, locParticleIndex);
				const auto& locSavedDecayUse = locCreatedUseMap.find(locDecayStepIndex)->second; //is same as locOrigDecayUse, except different zbins along chain

				//save the use for this decay
				auto locUseIterator = locNewFurtherDecays.find(locSavedDecayUse);
				if(locUseIterator != locNewFurtherDecays.end())
					++(locUseIterator->second);
				else
					locNewFurtherDecays.emplace(locSavedDecayUse, 1);
			}
		}
		else //is unknown (and guaranteed to be size 1 since has unknown parent)
		{
			//must dig down, but only one level: their decays must terminate at new steps (or end)
			auto locNewComboUse = Build_NewZDependentUse(locReaction, locStepIndex, locVertexZBin, locOrigDecayUse, locCreatedUseMap);
			//save the use for this decay
			auto locUseIterator = locNewFurtherDecays.find(locNewComboUse);
			if(locUseIterator != locNewFurtherDecays.end())
				++(locUseIterator->second);
			else
				locNewFurtherDecays.emplace(locNewComboUse, 1);
		}
	}

	//build and save new info, use, and return
	vector<pair<DSourceComboUse, unsigned char>> locFurtherDecayVector;
	locFurtherDecayVector.reserve(locNewFurtherDecays.size());
	std::copy(locNewFurtherDecays.begin(), locNewFurtherDecays.end(), std::back_inserter(locFurtherDecayVector));
	auto locNewComboInfo = locNewFurtherDecays.empty() ? locOrigInfo : GetOrMake_SourceComboInfo(locOrigInfo->Get_NumParticles(), locFurtherDecayVector);

	DSourceComboUse locNewComboUse(std::get<0>(locOrigUse), locVertexZBin, locNewComboInfo);
	dZDependentUseToIndependentMap.emplace(locNewComboUse, locOrigUse);
	return locNewComboUse;
}

pair<bool, map<DSourceComboUse, unsigned char>> DSourceComboer::Get_FinalStateDecayingComboUses(const DReaction* locReaction, size_t locStepIndex, const map<size_t, DSourceComboUse>& locStepComboUseMap) const
{
	//get combo infos for final-state decaying particles //if one is not present, ignore parent
	auto locIncludeParentFlag = true; //unless changed below
	map<DSourceComboUse, unsigned char> locFurtherDecays;
	auto locStep = locReaction->Get_ReactionStep(locStepIndex);
	for(size_t loc_i = 0; loc_i < locStep->Get_NumFinalPIDs(); ++loc_i)
	{
		int locDecayStepIndex = DAnalysis::Get_DecayStepIndex(locReaction, locStepIndex, loc_i);
		if(locDecayStepIndex < 0)
			continue;
		auto locUseIterator = locStepComboUseMap.find(size_t(locDecayStepIndex));
		if(locUseIterator == locStepComboUseMap.end())
			locIncludeParentFlag = false;
		else
		{
			//save decay
			auto& locSourceComboUse = locUseIterator->second;
			auto locDecayIterator = locFurtherDecays.find(locSourceComboUse);
			if(locDecayIterator == locFurtherDecays.end())
				locFurtherDecays.emplace(locSourceComboUse, 1);
			else
				++(locDecayIterator->second);
		}
	}

	return std::make_pair(locIncludeParentFlag, locFurtherDecays);
}

map<Particle_t, unsigned char> DSourceComboer::Build_ParticleMap(const DReaction* locReaction, size_t locStepIndex, Charge_t locCharge) const
{
	//build map of charged particles
	map<Particle_t, unsigned char> locNumParticles;
	auto locParticles = locReaction->Get_FinalPIDs(locStepIndex, false, false, locCharge, true); //no missing or decaying, include duplicates
	for(const auto& locPID : locParticles)
	{
		auto locPIDIterator = locNumParticles.find(locPID);
		if(locPIDIterator != locNumParticles.end())
			++(locPIDIterator->second);
		else
			locNumParticles.emplace(locPID, 1);
	}

	return locNumParticles;
}

DSourceComboUse DSourceComboer::Make_ComboUse(Particle_t locInitPID, const map<Particle_t, unsigned char>& locNumParticles, const map<DSourceComboUse, unsigned char>& locFurtherDecays)
{
	//convert locFurtherDecays map to a vector
	vector<pair<DSourceComboUse, unsigned char>> locDecayVector;
	locDecayVector.reserve(locFurtherDecays.size());
	std::copy(locFurtherDecays.begin(), locFurtherDecays.end(), std::back_inserter(locDecayVector));

	//convert locNumParticles map to a vector
	vector<pair<Particle_t, unsigned char>> locParticleVector;
	locParticleVector.reserve(locNumParticles.size());
	std::copy(locNumParticles.begin(), locNumParticles.end(), std::back_inserter(locParticleVector));

	//make or get the combo info
	auto locComboInfo = MakeOrGet_SourceComboInfo(locParticleVector, locDecayVector);
	return DSourceComboUse(locInitPID, DSourceComboInfo::Get_VertexZIndex_ZIndependent(), locComboInfo);
}

const DSourceComboInfo* DSourceComboer::MakeOrGet_SourceComboInfo(const vector<pair<Particle_t, unsigned char>>& locNumParticles, const vector<pair<DSourceComboUse, unsigned char>>& locFurtherDecays)
{
	//to be called (indirectly) by constructor: during the stage when primarily making
	//create the object on the stack
	DSourceComboInfo locSearchForInfo(locNumParticles, locFurtherDecays);

	//then search through the set to retrieve the pointer to the corresponding object if it already exists
	auto locInfoIterator = dSourceComboInfoSet.find(&locSearchForInfo);
	if(locInfoIterator != dSourceComboInfoSet.end())
		return *locInfoIterator; //it exists: return it

	//doesn't exist, make it and insert it into the sorted vector in the correct spot
	auto locComboInfo = new DSourceComboInfo(locNumParticles, locFurtherDecays);
	if(dDebugLevel >= 5)
		Print_SourceComboInfo(locComboInfo);
	dSourceComboInfoSet.insert(locComboInfo);
	dComboInfoChargeContent.emplace(locComboInfo, DAnalysis::Get_ChargeContent(locComboInfo));
	if(dDebugLevel >= 5)
		cout << "charge content = " << dComboInfoChargeContent[locComboInfo];
	if(DAnalysis::Get_HasMassiveNeutrals(locComboInfo))
		dComboInfosWithMassiveNeutrals.insert(locComboInfo);
	return locComboInfo;
}

const DSourceComboInfo* DSourceComboer::GetOrMake_SourceComboInfo(const vector<pair<Particle_t, unsigned char>>& locNumParticles, const vector<pair<DSourceComboUse, unsigned char>>& locFurtherDecays)
{
	//to be called when making combos: during the stage when primarily getting
	//create the object on the stack
	DSourceComboInfo locSearchForInfo(locNumParticles, locFurtherDecays);

	//then search through the vector to retrieve the pointer to the corresponding object if it already exists
	auto locIteratorPair = std::equal_range(dSourceComboInfos.begin(), dSourceComboInfos.end(), &locSearchForInfo, DCompare_SourceComboInfos());
	if(locIteratorPair.first != locIteratorPair.second)
		return *(locIteratorPair.first); //it exists: return it

	//doesn't exist, make it and insert it into the sorted vector in the correct spot
	auto locComboInfo = new DSourceComboInfo(locNumParticles, locFurtherDecays);
	if(dDebugLevel >= 5)
		Print_SourceComboInfo(locComboInfo);
	dSourceComboInfos.emplace(locIteratorPair.first, locComboInfo);
	dComboInfoChargeContent.emplace(locComboInfo, DAnalysis::Get_ChargeContent(locComboInfo));
	if(dDebugLevel >= 5)
		cout << "charge content = " << dComboInfoChargeContent[locComboInfo];
	if(DAnalysis::Get_HasMassiveNeutrals(locComboInfo))
		dComboInfosWithMassiveNeutrals.insert(locComboInfo);
	return locComboInfo;
}

/********************************************************************** SETUP FOR NEW EVENT ***********************************************************************/

void DSourceComboer::Reset_NewEvent(JEventLoop* locEventLoop)
{
	//check if it's actually a new event
	auto locEventNumber = locEventLoop->GetJEvent().GetEventNumber();
	if(locEventNumber == dEventNumber)
		return; //nope
	dEventNumber = locEventNumber;

	/************************************************************* RECYCLE AND RESET **************************************************************/

	//RECYCLE COMBO & VECTOR POINTERS
	//be careful! don't recycle combos with a use pid != unknown, because they are just copies! not unique pointers!

	//HANDLERS AND VERTEXERS
	dSourceComboP4Handler->Reset();
	dSourceComboTimeHandler->Reset();
	dSourceComboVertexer->Reset();
	dParticleComboCreator->Reset();

	//PARTICLES
	dTracksByPID.clear();
	dShowersByBeamBunchByZBin.clear();

	//RECYCLE THE DSOURCECOMBO OBJECTS
	for(auto& locComboPair : dMixedCombosByUseByChargedCombo)
		Recycle_ComboResources(locComboPair.second);
	Recycle_ComboResources(dSourceCombosByUse_Charged);

	//COMBOING RESULTS:
	dSourceCombosByUse_Charged.clear(); //BEWARE, CONTAINS VECTORS
	dMixedCombosByUseByChargedCombo.clear(); //BEWARE, CONTAINS VECTORS
	dSourceCombosByBeamBunchByUse.clear();
	dVertexPrimaryComboMap.clear();
	dValidRFBunches_ByCombo.clear();

	//COMBOING RESUME/SEARCH-AFTER TRACKING
	dResumeSearchAfterIterators_Particles.clear();
	dResumeSearchAfterIterators_Combos.clear();
	dResumeSearchAfterMap_Combos.clear();
	dResumeSearchAfterMap_Particles.clear();

	/************************************************************ SETUP FOR NEW EVENT *************************************************************/

	//GET JANA OBJECTS
	vector<const DNeutralShower*> locNeutralShowers;
	locEventLoop->Get(locNeutralShowers, dShowerSelectionTag.c_str());

	vector<const DChargedTrack*> locChargedTracks;
	locEventLoop->Get(locChargedTracks, "Combo");

	vector<const DBeamPhoton*> locBeamPhotons;
	locEventLoop->Get(locBeamPhotons);

	const DEventRFBunch* locInitialRFBunch = nullptr;
	locEventLoop->GetSingle(locInitialRFBunch);

    vector<const DESSkimData*> locESSkimDataVector;
    locEventLoop->Get(locESSkimDataVector);
    dESSkimData = locESSkimDataVector.empty() ? NULL : locESSkimDataVector[0];

	//SETUP NEUTRAL SHOWERS
	dSourceComboTimeHandler->Setup_NeutralShowers(locNeutralShowers, locInitialRFBunch);
	dSourceComboP4Handler->Set_PhotonKinematics(dSourceComboTimeHandler->Get_PhotonKinematics());
	dShowersByBeamBunchByZBin = dSourceComboTimeHandler->Get_ShowersByBeamBunchByZBin();

	//SETUP BEAM PARTICLES
	dSourceComboTimeHandler->Set_BeamParticles(locBeamPhotons);

	//SETUP TRACKS
	for(const auto& locChargedTrack : locChargedTracks)
	{
		for(const auto& locChargedHypo : locChargedTrack->dChargedTrackHypotheses)
			dTracksByPID[locChargedHypo->PID()].push_back(locChargedTrack);
	}
	//sort: not strictly necessary, but probably(?) makes sorting later go faster
	for(auto& locPIDPair : dTracksByPID)
		std::sort(locPIDPair.second.begin(), locPIDPair.second.end());
}

/********************************************************************* CREATE DSOURCOMBO'S **********************************************************************/

DCombosByReaction DSourceComboer::Build_ParticleCombos(const DReactionVertexInfo* locReactionVertexInfo)
{
	//This builds the combos and creates DParticleCombo & DParticleComboSteps (doing whatever is necessary)
	if(dDebugLevel > 0)
		cout << "CREATING DSourceCombo's FOR DREACTION " << locReactionVertexInfo->Get_Reaction()->Get_ReactionName() << endl;

	//Initialize results to be returned
	DCombosByReaction locOutputComboMap;
	auto locReactions = locReactionVertexInfo->Get_Reactions();
	for(auto locReaction : locReactions)
		locOutputComboMap[locReaction] = {};

	//All of the reactions in the vertex-info are guaranteed to have the same channel content
	//They just may differ in actions, or skims
	//So, we can check #particles for just one reaction, but must check skims for all reactions
	if(!Check_NumParticles(locReactions.front()))
	{
		if(dDebugLevel > 0)
			cout << "Not enough particles: No combos." << endl;
		return locOutputComboMap; //no combos!
	}

	auto Skim_Checker = [this](const DReaction* locReaction) -> bool{return !Check_Skims(locReaction);};
	locReactions.erase(std::remove_if(locReactions.begin(), locReactions.end(), Skim_Checker), locReactions.end());
	if(locReactions.empty())
	{
		if(dDebugLevel > 0)
			cout << "Event not in skim: No combos." << endl;
		return locOutputComboMap; //no combos!
	}

	/******************************************************** COMBOING STEPS *******************************************************
	*
	* CHARGED STAGE:
	*
	* OK, we start with charged tracks, because we can't do much with neutrals until we know the vertex to compute the kinematics.
	* So, we create our combos, skipping all neutral particles, but filling in all charged tracks.
	*
	* If mass cuts are needed (e.g. Lambda -> p, pi-), we first create combos of "-> p, pi-", saving them for the USE "X -> p, pi-"
	* We then place the invariant mass cut, and those that pass get copied and saved for the USE "Lambda -> p, pi-"
	* Thus, storing the decay PID separately from the combo means we can reuse the combo without creating new objects in this case.
	*
	* Once we have our charged track combos, we can find (most of) the vertices (will discuss exceptions below).
	* Once we have the vertices, we can compute the time offsets between the vertices (the amount of time a decaying particle took to decay).
	* And we can then place timing cuts on the charged tracks to select which beam bunches are possible.
	* Now, you might be thinking that we can cut on the timing of the charged tracks BEFORE we find the vertices, but in some cases we can't.
	* For a discussion on this, see the comments in DSourceComboTimeHandler.
	*
	*
	*
	* MIXED STAGE: GENERAL
	*
	* OK, now let's combo some neutrals.
	* First, we combo all of the neutrals that are needed with each other, and THEN we combo them with charged tracks.
	* (This is how the DSourceComboInfo objects were constructed).
	* This is because pi0 comboing will take the longest, and we want to make sure it is done largely independent of any charged tracks.
	*
	*
	* MIXED STAGE: VERTEX-Z
	* Now, as discussed earlier, showers can be broken up into z-dependent and z-independent varieties.
	* Z-Independent: FCAL photons
	* Z-Dependent: BCAL showers or FCAL massive neutrals
	* However, since
	* Again, for details, see the comments in DSourceComboTimeHandler and DSourceComboP4Handler.
	*
	* Now, since the z-independent combos can be reused for any vertex-z, they are created first.
	* Then, the z-dependent combos are created, and combined with the z-independent ones.
	* To do this, it turns out it's easier to just try to create combos with ALL showers, and then skip creating the ones we've already created.
	*
	* While building combos, mass cuts are placed along the way, EXCEPT on combos with massive neutral particles.
	* This is because the exact vertex position is needed to get an accurate massive-neutral momentum.
	* While comboing, we want the results to be as re-usable as possible, that's why we use vertex-z bins.
	* But vertex-z bins are not sufficient for this, so we will cut on invariant masses with massive neutrals later.
	*
	*
	* MIXED STAGE: BEAM BUNCHES
	* Now, as far s
	*******************************************************************************************************************************/

	//charged stage: charged only, no neutrals in infos

	//when on mixed stage (existing charged + neutrals, comboing into fully-neutral & mixed):
	//loop over charged combos: calc vertices, then build/convert FULL combo use with given vertex z-bins
	//then, just build the whole combo all it once, almost as before. however, some things are different
		//get charged particles to combo: choice is REDUCED to those from that vertex in the input combo
		//get charged combos to combo: if sub-combo is fully-charged, choice is REDUCED to be the input charged combo contents (almost always size 1)
			//thus we don't use ANY of the saved charged combos any more
			//and when we retrieve mixed combos for further comboing, they are specific (temporary) to this charged combo
				//Mixed results are saved in: unordered_map<mixed_use, unordered_map<charged_combo, vector<mixed_combo>>> (where the keys are the charged contents of the mixed-use step)
				//So that way we can re-use between channels
				//But how to RETRIEVE from here?, we need to get the charged combo from the given use //tricky, but we can do it
		//we do these because we don't want to rebuild the charged combos from scratch: wastes time, choices are restricted by vertex-z, we don't want to recompute vertex-z, we don't want dupe combos

	//combo the mixed stage in two stages:
	//FCAL showers only: z-bin any
	//All showers
		//here, they are comboed with uses having a specific vertex-z set
		//fully-neutral combos saved-to/retrieved-from charged-independent area for re-use (use already contains specific vertex-z bin)
		//first grab fcal combo results from fcal-only use area (or mixed area), first setting the z-bin to -1

	//Massive neutrals:
		//Just combo at the same time with the rest of the neutrals, but hold off on both mass cuts and timing cuts
		//They must be done with a specific vertex-z, rather than just a z-bin

	//MUST BEWARE DUPLICATE COMBOS
	//let's say a combo of charged tracks has 2 valid RF bunches
	//and we need to combo 2 pi0s with them
	//and the shower timing cuts are loose enough that all 4 showers satisfy both RF bunches
	//if we combo the 2 rf bunches separately: WE HAVE DUPLICATE COMBOS
	//and doing the duplicate check AFTER the fact takes FOREVER
	//therefore, we must take the neutral showers for the 2 rfs, COMBINE THEM, and then COMBO AS A UNIT

	//get step vertex infos (sorted in dependency order)
	auto locStepVertexInfos = locReactionVertexInfo->Get_StepVertexInfos();
	auto locPrimaryStepVertexInfo = locReactionVertexInfo->Get_StepVertexInfo(0);
	auto locPrimaryComboUse = dSourceComboUseReactionMap[locPrimaryStepVertexInfo];
	auto locPrimaryComboInfo = std::get<2>(locPrimaryComboUse);

	//handle special case of no charged tracks
	if(dDebugLevel > 0)
		cout << "Combo charge content: " << dComboInfoChargeContent[std::get<2>(locPrimaryComboUse)] << " (charged/neutral are " << d_Charged << "/" << d_Neutral << ")" << endl;
	if(dComboInfoChargeContent[std::get<2>(locPrimaryComboUse)] == d_Neutral)
	{
		if(dDebugLevel > 0)
			cout << "No charged tracks." << endl;
		Combo_WithNeutralsAndBeam(locReactions, locReactionVertexInfo, locPrimaryComboUse, nullptr, {}, locOutputComboMap);
		return locOutputComboMap;
	}

	//Build vertex combos (returns those for the primary vertex, others are stored)
	Create_SourceCombos(locPrimaryComboUse, d_ChargedStage, nullptr);
	const auto& locReactionChargedCombos = *(Get_CombosSoFar(d_ChargedStage, d_Charged, nullptr)[locPrimaryComboUse]);

	//loop over primary vertex combos //each contains decay combos except when dangling
	for(const auto& locReactionChargedCombo : locReactionChargedCombos)
	{
		//Calc all the vertex positions and time offsets for the vertices for these combos (where possible without beam energy)
		dSourceComboVertexer->Calc_VertexTimeOffsets_WithCharged(locReactionVertexInfo, locReactionChargedCombo);

		//For the charged tracks, apply timing cuts to determine which RF bunches are possible
		vector<int> locBeamBunches_Charged;
		if(!dSourceComboTimeHandler->Select_RFBunches_Charged(locReactionVertexInfo, locReactionChargedCombo, locBeamBunches_Charged))
			continue; //failed PID timing cuts!

		//Special case of FULLY charged
		auto locChargeContent = dComboInfoChargeContent[locPrimaryComboInfo];
		if(locChargeContent == d_Charged)
		{
			if(dDebugLevel > 0)
				cout << "Fully charged." << endl;

			//Select final RF bunch
			auto locRFBunch = dSourceComboTimeHandler->Select_RFBunch_Full(locReactionVertexInfo, locReactionChargedCombo, locBeamBunches_Charged);
			if(dDebugLevel > 0)
				cout << "Selected rf bunch." << endl;

			//combo with beam and save results!!! (if no beam needed, just saves and returns)
			Combo_WithBeam(locReactions, locReactionVertexInfo, locReactionChargedCombo, locRFBunch, locOutputComboMap);
			continue;
		}

		//Combo with neutrals and beam
		Combo_WithNeutralsAndBeam(locReactions, locReactionVertexInfo, locPrimaryComboUse, locReactionChargedCombo, locBeamBunches_Charged, locOutputComboMap);
	}

	if(dDebugLevel > 0)
	{
		for(auto locComboPair : locOutputComboMap)
			cout << "reaction, #combos = " << locComboPair.first->Get_ReactionName() << ", " << locComboPair.second.size() << endl;
	}

	return locOutputComboMap;
}

void DSourceComboer::Combo_WithNeutralsAndBeam(const vector<const DReaction*>& locReactions, const DReactionVertexInfo* locReactionVertexInfo, const DSourceComboUse& locPrimaryComboUse, const DSourceCombo* locReactionChargedCombo, const vector<int>& locBeamBunches_Charged, DCombosByReaction& locOutputComboMap)
{
	if(dDebugLevel > 0)
		cout << "Comboing neutrals." << endl;

	//Create full source-particle combos (including neutrals): First using only FCAL showers, then using all showers
	Create_SourceCombos(locPrimaryComboUse, d_MixedStage_ZIndependent, locReactionChargedCombo);
	auto locZDependentComboUse = Create_ZDependentSourceComboUses(locReactionVertexInfo, locReactionChargedCombo);
	Create_SourceCombos(locZDependentComboUse, d_MixedStage, locReactionChargedCombo);

	//Then, get the full combos, but only those that satisfy the charged RF bunches
	const auto& locReactionFullCombos = Get_CombosForComboing(locZDependentComboUse, d_MixedStage, locBeamBunches_Charged, locReactionChargedCombo);

	//loop over full combos
	for(const auto& locReactionFullCombo : locReactionFullCombos)
	{
		auto locValidRFBunches = dValidRFBunches_ByCombo[locReactionFullCombo]; //get by value, will cut below if massive neutral

		//if not fully neutral, do the below
		if(locReactionChargedCombo != nullptr)
		{
			//Calculate vertex positions & time offsets using photons
			//not likely to have any effect, but it's necessary sometimes (but rarely)
			//E.g. g, p ->  K0, Sigma+    K0 -> 3pi: The selected pi0 photons could help define the production vertex
			dSourceComboVertexer->Calc_VertexTimeOffsets_WithPhotons(locReactionVertexInfo, locReactionChargedCombo, locReactionFullCombo);

			//Now further select rf bunches, using tracks and BCAL photon showers at the vertices we just found
			if(!dSourceComboTimeHandler->Select_RFBunches_PhotonVertices(locReactionVertexInfo, locReactionFullCombo, locValidRFBunches))
				continue; //failed PID timing cuts!
		}

		//PLACE mass cuts on massive neutrals: Effectively narrows down RF bunches
		//do 2 things at once (where vertex is known) (hence the really long function name):
			//calc & cut invariant mass: when massive neutral present
			//calc & cut invariant mass: when vertex-z was unknown with only charged tracks, but is known now, and contains BCAL photons (won't happen very often)
		if(!dSourceComboP4Handler->Cut_InvariantMass_HasMassiveNeutral_OrPhotonVertex(locReactionVertexInfo, locReactionFullCombo, locValidRFBunches))
			continue; //failed cut!

		//Select final RF bunch //this is not a cut: at least one has passed all cuts (check by the Get_CombosForComboing function & the mass cuts)
		auto locRFBunch = dSourceComboTimeHandler->Select_RFBunch_Full(locReactionVertexInfo, locReactionFullCombo, locValidRFBunches);

		//combo with beam and save results!!! (if no beam needed, just saves and returns)
		Combo_WithBeam(locReactions, locReactionVertexInfo, locReactionFullCombo, locRFBunch, locOutputComboMap);
	}
}

void DSourceComboer::Combo_WithBeam(const vector<const DReaction*>& locReactions, const DReactionVertexInfo* locReactionVertexInfo, const DSourceCombo* locReactionFullCombo, int locRFBunch, DCombosByReaction& locOutputComboMap)
{
	if(dDebugLevel > 0)
		cout << "Comboing beam." << endl;

	//if no beam then we are done!
	if(!locReactionVertexInfo->Get_StepVertexInfos().front()->Get_ProductionVertexFlag())
	{
		if(dDebugLevel > 0)
			cout << "No beam particles, we are done!" << endl;
		for(const auto& locReaction : locReactions)
			locOutputComboMap[locReaction].push_back(dParticleComboCreator->Build_ParticleCombo(locReactionVertexInfo, locReactionFullCombo, nullptr, locRFBunch, locReaction->Get_KinFitType()));
		return;
	}

	//Select beam particles
	auto locBeamParticles = dSourceComboTimeHandler->Get_BeamParticlesByRFBunch(locRFBunch, dMaxRFBunchCuts[locReactionVertexInfo]);
	if(dDebugLevel > 0)
		cout<< "rf bunch, max #rf bunches, #beams = " << locRFBunch << ", " << dMaxRFBunchCuts[locReactionVertexInfo] << ", " << locBeamParticles.size() << endl;
	if(locBeamParticles.empty())
		return; //no valid beam particles!!

	//loop over beam particles
	for(const auto& locBeamParticle : locBeamParticles)
	{
		//Calculate remaining vertex positions (that needed to be done via missing mass)
		dSourceComboVertexer->Calc_VertexTimeOffsets_WithBeam(locReactionVertexInfo, locReactionFullCombo, locBeamParticle);

		//placing timing cuts on the particles at these vertices
		if(!dSourceComboTimeHandler->Cut_Timing_MissingMassVertices(locReactionVertexInfo, locReactionFullCombo, locBeamParticle, locRFBunch))
			continue; //FAILED TIME CUTS!

		//place invariant mass cuts on the particles at these vertices (if they had z-dependent neutral showers (BCAL or massive))
		if(!dSourceComboP4Handler->Cut_InvariantMass_MissingMassVertex(locReactionVertexInfo, locReactionFullCombo, locBeamParticle, locRFBunch))
			continue; //FAILED MASS CUTS!

		//build particle combo & save for the apporpriate reactions
		auto locBeamRFBunch = dSourceComboTimeHandler->Calc_RFBunchShift(locBeamParticle->time());
		size_t locDeltaRFBunch = abs(locRFBunch - locBeamRFBunch);
		for(const auto& locReaction : locReactions)
		{
			if(dDebugLevel > 0)
				cout<< "beam rf bunch, delta rf bunch, reaction, max for reaction = " << locBeamRFBunch << ", " << locDeltaRFBunch << ", " << locReaction->Get_ReactionName() << ", " << dRFBunchCutsByReaction[locReaction] << endl;
			if(locDeltaRFBunch <= dRFBunchCutsByReaction[locReaction])
				locOutputComboMap[locReaction].push_back(dParticleComboCreator->Build_ParticleCombo(locReactionVertexInfo, locReactionFullCombo, locBeamParticle, locRFBunch, locReaction->Get_KinFitType()));
		}
	}
}

/**************************************************************** BUILD SOURCE COMBOS - GENERAL *****************************************************************/

/*
 * suppose reaction is 0) g, p -> omega, p
 *                     1)         omega -> 3pi
 *                     2)                   pi0 -> 2g
 *
 * It will have uses/infos like:
 * 0: X -> 1, A (mixed + charged) (both are listed as further decays)
 *    A: X -> p (charged)
 * 	1: omega -> B, 2 (mixed) (both are listed as further decays)
 *    	B: X -> pi+, pi- (charged)
 * 		2: pi0 -> 2g (neutral)
 *
 * So, it will be comboed as:
 *
 * CHARGED STAGE:
 * 0: Combo_Vertically_AllDecays() -> Call Create_SourceCombos() with 1
 * 		1: Combo_Vertically_AllDecays() -> Call Create_SourceCombos() with B
 * 			B: Combo_Horizontally_All() -> Call Combo_Horizontally_All() (near end, after particle loop) with X -> pi-
 * 				X -> pi-: Create_Combo_OneParticle()
 * 			B: Call Combo_Horizontally_AddParticle() (near end of Combo_Horizontally_All(), after call to create X -> pi-)
 * 		1: Combo_Vertically_AllDecays() -> Skip 2 since contains all neutrals
 * 		1: Combo_Horizontally_All() -> in further decay loop, save combos of B as 1 (since only missing pi0s, which are fully neutral)
 * 0: Combo_Vertically_AllDecays() -> Call Create_SourceCombos() with A
 * 		A: Combo_Horizontally_All() -> Create_Combo_OneParticle()
 * 0: Combo_Horizontally_All() -> in further decay loop call Combo_Horizontally_AddCombo()
 *
 * MIXED STAGE:
 * 0: Combo_Vertically_AllDecays() -> Call Create_SourceCombos() with 1
 * 		1: Combo_Vertically_AllDecays() -> Skip B since already created
 * 		1: Combo_Vertically_AllDecays() -> Call Create_SourceCombos() with 2
 * 			2: Combo_Vertically_AllParticles() -> Combo_Vertically_NParticles()
 * 		1: Combo_Horizontally_All() -> in further decay loop call Combo_Horizontally_AddCombo()
 * 0: Combo_Vertically_AllDecays() -> Skip A since already created
 * 0: Combo_Horizontally_All() -> further decay loop -> Combo_Horizontally_AddCombo()
 *
 * The purpose of passing through the charged combo:
 * 1) To retrieve the correct charged combo when comboing it to neutrals to create mixed
 * 2) To save the mixed comboing results in a way that they can be reused
 *
 * The charged combos will be:
 * 0: X -> A, 1				//presiding = 0, withnow = A
 *    A: X -> p				//both = nullptr
 * 	1: omega -> B, 2		//presiding = 1, withnow = B
 *    	B: X -> pi+, pi-	//both = nullptr
 *			2: pi0 -> 2g	//both = nullptr
 *
 */

void DSourceComboer::Create_SourceCombos(const DSourceComboUse& locComboUseToCreate, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_Presiding)
{
	//if on mixed stage, it is impossible for this function to be called with a fully-charged use (already exists!!)
	const auto& locDecayPID = std::get<0>(locComboUseToCreate);
	const auto& locVertexZBin = std::get<1>(locComboUseToCreate);
	const auto& locSourceComboInfo = std::get<2>(locComboUseToCreate);

	//we will create these combos for an "Unknown" decay (i.e. no decay, just a direct grouping)
	//then, when we return from this function, we can cut on the invariant mass of the system for any decay we might need it for
	DSourceComboUse locUnknownComboUse(Unknown, locVertexZBin, locSourceComboInfo);
	Create_SourceCombos_Unknown(locUnknownComboUse, locComboingStage, locChargedCombo_Presiding);

	//if all we want is a direct grouping (unknown), then the combos have already been made: return
	if(locDecayPID == Unknown)
		return;

	//Get combos so far
	auto locChargedCombo_WithNow = Get_ChargedCombo_WithNow(locChargedCombo_Presiding);
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, dComboInfoChargeContent[locSourceComboInfo], locChargedCombo_WithNow);
	auto locInfoChargeContent = dComboInfoChargeContent[locSourceComboInfo];

	//get the combos that we just created
	auto locSourceCombos = locSourceCombosByUseSoFar[locUnknownComboUse];

	if((locComboingStage == d_ChargedStage) && (locInfoChargeContent != d_Charged))
	{
		//don't cut yet! we don't have the neutrals! just copy results and return
		locSourceCombosByUseSoFar.emplace(locComboUseToCreate, locSourceCombos);
		return;
	}

	//get combos by beam bunch
	auto* locSourceCombosByBeamBunchByUse = (locComboingStage != d_ChargedStage) ? &(Get_SourceCombosByBeamBunchByUse(locInfoChargeContent, locChargedCombo_WithNow)) : nullptr;
	auto* locCombosByBeamBunch = (locComboingStage != d_ChargedStage) ? &((*locSourceCombosByBeamBunchByUse)[locComboUseToCreate]) : nullptr;

	//cannot place an invariant mass cut on massive neutrals yet, because:
		//vertex position must first be defined
		//although we probably HAVE the vertex position, if it's a fully neutral combo, we don't want to use it:
			//results are stored in vertex-z-bins and independent of charged combo: if we cut, we won't be able to reuse the results (because we need PRECISE position, not just a z-bin)
		//if it is a mixed combo with known vertex, we can conceivably cut, but there aren't too many of those: Just put off the cuts until later
	if(Get_HasMassiveNeutrals(locSourceComboInfo))
	{
		locSourceCombosByUseSoFar.emplace(locComboUseToCreate, locSourceCombos);
		(*locSourceCombosByBeamBunchByUse)[locComboUseToCreate] = (*locSourceCombosByBeamBunchByUse)[locUnknownComboUse];
		return;
	}

	//if on the all-showers stage, first copy over ALL fcal-only results
	if(locComboingStage == d_MixedStage)
		Copy_ZIndependentMixedResults(locComboUseToCreate, locChargedCombo_WithNow);
	else //initialize vector for storing results
	{
		locSourceCombosByUseSoFar.emplace(locComboUseToCreate, dResourcePool_SourceComboVector.Get_Resource());
		locSourceCombosByUseSoFar[locComboUseToCreate]->reserve(dInitialComboVectorCapacity);
	}

	if((locComboingStage == d_MixedStage) && (locVertexZBin == DSourceComboInfo::Get_VertexZIndex_Unknown()))
	{
		//we need a zbin for BCAL showers, but it is unknown: can't cut yet!
		//copy the new z-dependent results into the existing vector (because FCAL cuts were already placed!)

		//in locSourceCombos, all of the fcal results are stored at the front
		//so find where the end of that vector is, and copy over the new ones
		auto locComboUseFCAL = std::make_tuple(Unknown, DSourceComboInfo::Get_VertexZIndex_ZIndependent(), locSourceComboInfo);
		const auto& locFCALComboVector = *(locSourceCombosByUseSoFar[locComboUseFCAL]);
		auto& locBothComboVector = *(locSourceCombosByUseSoFar[locComboUseToCreate]);
		locBothComboVector.insert(locBothComboVector.end(), locSourceCombos->begin() + locFCALComboVector.size(), locSourceCombos->end());

		//now the combos by beam bunch
		auto& locFCALCombosByBeamBunch = (*locSourceCombosByBeamBunchByUse)[locComboUseFCAL];
		map<vector<int>, vector<const DSourceCombo*>>& locBothCombosByBeamBunch = (*locSourceCombosByBeamBunchByUse)[locComboUseToCreate];
		auto& locUnknownBothCombosByBeamBunch = (*locSourceCombosByBeamBunchByUse)[locUnknownComboUse];
		for(const auto& locUnknownComboBeamBunchPair : locUnknownBothCombosByBeamBunch)
		{
			const auto& locRFBunches = locUnknownComboBeamBunchPair.first;
			const auto& locUnknownBunchCombos = locUnknownComboBeamBunchPair.second;
			locBothCombosByBeamBunch[locRFBunches].insert(locBothCombosByBeamBunch[locRFBunches].end(), locUnknownBunchCombos.begin() + locFCALCombosByBeamBunch[locRFBunches].size(), locUnknownBunchCombos.end());
		}
		return;
	}

	//initialize vector for storing results
	locSourceCombosByUseSoFar.emplace(locComboUseToCreate, dResourcePool_SourceComboVector.Get_Resource());
	locSourceCombosByUseSoFar[locComboUseToCreate]->reserve(dInitialComboVectorCapacity);

	//place an invariant mass cut & save the results
	for(const auto& locSourceCombo : *locSourceCombos)
	{
		if(!dSourceComboP4Handler->Cut_InvariantMass_NoMassiveNeutrals(locSourceCombo, locDecayPID, locVertexZBin))
			continue;

		//save the results
		locSourceCombosByUseSoFar[locComboUseToCreate]->push_back(locSourceCombo);
		if(locComboingStage == d_ChargedStage)
			continue;

		//register beam bunches
		const auto& locBeamBunches = dValidRFBunches_ByCombo[locSourceCombo];
		for(const auto& locBeamBunch : locBeamBunches)
			(*locCombosByBeamBunch)[{locBeamBunch}].push_back(locSourceCombo);
		if(locBeamBunches.empty())
			(*locCombosByBeamBunch)[locBeamBunches].push_back(locSourceCombo);
	}
}

void DSourceComboer::Create_SourceCombos_Unknown(const DSourceComboUse& locComboUseToCreate, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_Presiding)
{
	/****************************************************** COMBOING PARTICLES *****************************************************
	*
	* First combo VERTICALLY, and then HORIZONTALLY
	* What does this mean?
	* Vertically: Make combos of size N of each PID needed (e.g. 3 pi0s)
	* Horizontally: Make combos of different PIDs (e.g. 2pi0, pi+, pi-, p)
	*
	* Why start with vertical comboing?
	* because the thing that takes the most time is when someone decides to analyze (e.g.) 2pi0, 3pi0, then 3pi0 eta, 3pi0 something else, 4pi0, etc.
	* we want to make the Npi0 combos as needed, then reuse the Npi0s when making combos of other types
	* thus we want to build vertically (pi0s together, then etas together), and THEN horizontally (combine pi0s & etas, etc)
	* plus, when building vertically, it's easier to keep track of things since the PID / decay-parent is the same
	*
	* Build all possible combos for all NEEDED GROUPINGS for each of the FURTHER DECAYS (if not done already)
	* this becomes a series of recursive calls
	* e.g. if need 3 pi0s, call for 2pi0s, which calls for 1pi0, which calls for 2g
	* then do the actual pi0 groupings on the return
	*
	* Note, if we combo vertically (e.g. 3pi0s, 2pi+'s, etc.), they are created with a use that is strictly that content.
	* Then, when we combo them horizontally, they are promoted out of the vertical combo, at the same level as everything else in the new horizontal combo.
	* This reduces the depth-complexity of the combos.
	*
	*******************************************************************************************************************************/


	Combo_Vertically_AllDecays(locComboUseToCreate, locComboingStage, locChargedCombo_Presiding);
	if((locComboingStage == d_ChargedStage) || (dComboInfoChargeContent[std::get<2>(locComboUseToCreate)] == d_Neutral))
		Combo_Vertically_AllParticles(locComboUseToCreate, locComboingStage); //no such thing as a "mixed" particle

	//OK, now build horizontally!! //group particles with different PIDs
	Combo_Horizontally_All(locComboUseToCreate, locComboingStage, locChargedCombo_Presiding);
}

/************************************************************** BUILD PHOTON COMBOS - VERTICALLY ****************************************************************/

void DSourceComboer::Combo_Vertically_AllDecays(const DSourceComboUse& locComboUseToCreate, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_Presiding)
{
	auto locChargedCombo_WithNow = Get_ChargedCombo_WithNow(locChargedCombo_Presiding);

	//Get combos so far
	auto locComboInfo = std::get<2>(locComboUseToCreate);
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, dComboInfoChargeContent[locComboInfo], locChargedCombo_WithNow);

	//get combo use contents
	auto locVertexZBin = std::get<1>(locComboUseToCreate);
	auto locNumParticlesNeeded = locComboInfo->Get_NumParticles();
	auto locFurtherDecays = locComboInfo->Get_FurtherDecays();

	//for each further decay map entry (e.g. pi0, 3), this is a collection of the uses representing those groupings //e.g. Unknown -> 3pi0
	for(const auto& locFurtherDecayPair : locFurtherDecays)
	{
		auto& locSourceComboDecayUse = locFurtherDecayPair.first; //e.g. pi0, -> 2g
		auto& locNumDecaysNeeded = locFurtherDecayPair.second; //N of the above decay (e.g. pi0s)
		auto locDecayChargeContent = dComboInfoChargeContent[std::get<2>(locSourceComboDecayUse)];

		if((locComboingStage == d_ChargedStage) && (locDecayChargeContent == d_Neutral))
			continue; //skip for now!!

		if(locNumDecaysNeeded == 1)
		{
			//if on a mixed stage, and the to-build combo info is fully charged, skip it: it's already been done
			if((locComboingStage != d_ChargedStage) && (locDecayChargeContent == d_Charged))
				continue;

			//build the decay combo directly
			if(locSourceCombosByUseSoFar.find(locSourceComboDecayUse) != locSourceCombosByUseSoFar.end()) //if not done already!
			{
				//must dive down to get the next charged combo
				//building for the first time: the first one (later ones will be grabbed when building these combos vertically (in Combo_Vertically_NDecays))
				auto locChargedCombo_NextPresiding = Get_Presiding_ChargedCombo(locChargedCombo_Presiding, locSourceComboDecayUse, locComboingStage, 1);

				//must return to top-level combo function to build this decay, as this may have any structure
				Create_SourceCombos(locSourceComboDecayUse, locComboingStage, locChargedCombo_NextPresiding);
			}
			continue;
		}

		//OK, so we need a grouping of N > 1 decays (e.g. pi0s)
		//so, let's create a use of Unknown -> N pi0s (e.g.)
		//if we can just utilize the use from the input combo-info, then we will. if not, we'll make a new one
		auto locNeededGroupingUse = locComboUseToCreate;
		if((locFurtherDecays.size() > 1) || !locNumParticlesNeeded.empty()) //if true: can't use the input
		{
			auto locGroupingComboInfo = GetOrMake_SourceComboInfo({}, {std::make_pair(locSourceComboDecayUse, locNumDecaysNeeded)}); // -> N pi0s (e.g.)
			locNeededGroupingUse = std::make_tuple(Unknown, locVertexZBin, locGroupingComboInfo); // Unknown -> Npi0s (e.g.)
		}

		// Now, see whether the combos for this grouping have already been done
		if(locSourceCombosByUseSoFar.find(locNeededGroupingUse) != locSourceCombosByUseSoFar.end())
			continue; //it's already done!!

		//it's not already done.  darn it.
		//build an info and a use for a direct grouping of N - 1 decays //e.g. 2pi0s
		auto locNMinus1ComboUse = locSourceComboDecayUse; //initialize (is valid if #needed == 2, otherwise will create it)
		if(locNumDecaysNeeded > 2)
		{
			auto locNMinus1Info = GetOrMake_SourceComboInfo({}, {std::make_pair(locSourceComboDecayUse, locNumDecaysNeeded - 1)}); // 0 detected particles, N - 1 pi0s (e.g.)
			locNMinus1ComboUse = std::make_tuple(Unknown, locVertexZBin, locNMinus1Info); // Unknown -> N - 1 pi0s (e.g.)
		}

		// Now, see whether the combos for the direct N - 1 grouping have already been done.  If not, create them
		if(locSourceCombosByUseSoFar.find(locNMinus1ComboUse) == locSourceCombosByUseSoFar.end())
			Combo_Vertically_AllDecays(locNMinus1ComboUse, locComboingStage, locChargedCombo_WithNow); //no need to go to top-level combo function since just N - 1: can re-call this one

		//Finally, we can actually DO the grouping, between the N - 1 combos and the one-off combos
		Combo_Vertically_NDecays(locNeededGroupingUse, locNMinus1ComboUse, locSourceComboDecayUse, locComboingStage, locChargedCombo_WithNow);
	}
}

void DSourceComboer::Combo_Vertically_NDecays(const DSourceComboUse& locComboUseToCreate, const DSourceComboUse& locNMinus1ComboUse, const DSourceComboUse& locSourceComboDecayUse, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_Presiding)
{
	auto locVertexZBin = std::get<1>(locComboUseToCreate);
	auto locNIs2Flag = (locNMinus1ComboUse == locSourceComboDecayUse); //true if need exactly 2 decaying particles

	//Get combos so far
	auto locChargeContent = dComboInfoChargeContent[std::get<2>(locComboUseToCreate)];
	auto locChargedCombo_WithNow = Get_ChargedCombo_WithNow(locChargedCombo_Presiding);
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, locChargeContent, locChargedCombo_WithNow);

	//e.g. we are grouping 1 pi0 with N - 1 pi0s to make a combo of N pi0s
	//so, let's get the combos for (e.g.) 1 pi0 and for N - 1 pi0s
	const auto& locCombos_NMinus1 = *locSourceCombosByUseSoFar[locNMinus1ComboUse]; //Combos are a vector of (e.g.): -> N - 1 pi0s
	if(locCombos_NMinus1.empty())
		return; //bail!

	//if on the all-showers stage, first copy over ALL fcal-only results
	if(locComboingStage == d_MixedStage)
		Copy_ZIndependentMixedResults(locComboUseToCreate, locChargedCombo_WithNow);
	else //initialize vector for storing results
	{
		locSourceCombosByUseSoFar.emplace(locComboUseToCreate, dResourcePool_SourceComboVector.Get_Resource());
		locSourceCombosByUseSoFar[locComboUseToCreate]->reserve(dInitialComboVectorCapacity);
	}

	//if comboing N mixed combos (locComboUseToCreate) (which are thus all used in the same step), do this:
	//locChargedCombo_WithNow corresponds to N mixed combos
	auto locInstance = locNIs2Flag ? 2 : locCombos_NMinus1.front()->Get_FurtherDecayCombos()[locSourceComboDecayUse].size() + 1; //numbering starts with 1, not 0
	const DSourceCombo* locChargedCombo_WithPrevious = Get_ChargedCombo_WithNow(Get_Presiding_ChargedCombo(locChargedCombo_Presiding, locSourceComboDecayUse, locComboingStage, locInstance));

	//now, for each combo of N - 1 (e.g.) pi0s, see which of the single-decay combos are a valid grouping
	//valid grouping:
		//TEST 1: If (e.g.) pi0s have names "A", "B", "C", don't include the grouping "ABA", and don't include "ACB" if we already have "ABC"
		//TEST 2: Also, don't re-use a shower we've already used (e.g. if A & C each contain the same photon, don't group them together)
		//Technically, if we pass Test 2 we automatically pass Test 1.
		//However, validating for Test 1 is much faster, as discussed below.
	for(const auto& locCombo_NMinus1 : locCombos_NMinus1)
	{
		//loop over potential combos to add to the group, creating a new combo for each valid (non-duplicate) grouping
		//however, we don't have to loop over all of the combos!!

		//first of all, get the potential combos that satisfy the RF bunches for the N - 1 combo
		const auto& locValidRFBunches_NMinus1 = dValidRFBunches_ByCombo[locCombo_NMinus1];
		const auto& locDecayCombos_1 = Get_CombosForComboing(locSourceComboDecayUse, locComboingStage, locValidRFBunches_NMinus1, locChargedCombo_WithPrevious);

		//now, note that all of the combos are stored in the order in which they were created (e.g. A, B, C, D)
		//so (e.g.), groupings of 2 will be created and saved in the order: AB, AC, AD, BC, BD, CD
		//above, on the B-loop, we start the search at "C," not at A, because this was already tested on an earlier pass
		//therefore, start the search one AFTER the LAST (e.g. -> 2 photon) combo of the N - 1 group
		//this will guarantee we pass "TEST 1" without ever checking

		//actually, we already saved the iterator to the first (e.g.) pi0 to test when we saved the N - 1 combo, so just retrieve it
		auto locComboSearchIterator = Get_ResumeAtIterator_Combos(locCombo_NMinus1, locValidRFBunches_NMinus1, locComboingStage, locVertexZBin);
		if(locComboSearchIterator == std::end(locDecayCombos_1))
			continue; //e.g. this combo is "AD" and there are only 4 reconstructed combos (ABCD): no potential matches! move on to the next N - 1 combo

		//before we loop, first get all of the showers used to make the N - 1 grouping, and sort it so that we can quickly search it
		auto locUsedParticles_NMinus1 = DAnalysis::Get_SourceParticles(locCombo_NMinus1->Get_SourceParticles(true)); //true: entire chain
		std::sort(locUsedParticles_NMinus1.begin(), locUsedParticles_NMinus1.end()); //must sort, because when retrieving entire chain is unsorted

		//this function will do our "TEST 2"
		auto Search_Duplicates = [&locUsedParticles_NMinus1](const JObject* locParticle) -> bool
				{return std::binary_search(locUsedParticles_NMinus1.begin(), locUsedParticles_NMinus1.end(), locParticle);};

		auto locIsZIndependent_NMinus1 = locCombo_NMinus1->Get_IsComboingZIndependent();

		//now loop over the potential combos
		for(; locComboSearchIterator != locDecayCombos_1.end(); ++locComboSearchIterator)
		{
			const auto locDecayCombo_1 = *locComboSearchIterator;

			//If on all-showers stage, and combo is fcal-only, don't save (combo already created!!)
			auto locIsZIndependent = locIsZIndependent_NMinus1 && locDecayCombo_1->Get_IsComboingZIndependent();
			if((locComboingStage == d_MixedStage) && locIsZIndependent)
				continue; //this combo has already been created (assuming it was valid): during the FCAL-only stage

			//conduct "TEST 2" search: search the N - 1 shower vector to see if any of the showers in this combo are duplicated
			auto locUsedParticles_1 = DAnalysis::Get_SourceParticles(locDecayCombo_1->Get_SourceParticles(true)); //true: entire chain
			if(std::any_of(locUsedParticles_1.begin(), locUsedParticles_1.end(), Search_Duplicates))
				continue; //at least one photon was a duplicate, this combo won't work

			//no duplicates: this combo is unique.  build a new combo!

			//See which RF bunches match up //guaranteed to be at least one, due to selection in Get_ParticlesForComboing() function
			auto locValidRFBunches = dSourceComboTimeHandler->Get_CommonRFBunches(locValidRFBunches_NMinus1, dValidRFBunches_ByCombo[locDecayCombo_1]);

			//Combine the decay combos
			vector<const DSourceCombo*> locAllDecayCombos;
			if(locNIs2Flag) //N = 2 Two identical combos (e.g. 2 of pi0 -> 2g)
				locAllDecayCombos = {locCombo_NMinus1, locDecayCombo_1};
			else //combine a combo of N - 1 (e.g. pi0) decays to this new one
			{
				//take the vector of N - 1 (e.g. -> 2g) combos and add the new one
				locAllDecayCombos = locCombo_NMinus1->Get_FurtherDecayCombos()[locSourceComboDecayUse];
				locAllDecayCombos.push_back(locDecayCombo_1);
			}

			//then create the new combo
			DSourceCombosByUse_Small locFurtherDecayCombos = {std::make_pair(locSourceComboDecayUse, locAllDecayCombos)}; //arguments (e.g.): (pi0, -> 2g), N combos of: -> 2g
			auto locCombo = dResourcePool_SourceCombo.Get_Resource();
			locCombo->Set_Members({}, locFurtherDecayCombos, locIsZIndependent); // 1 combo of N (e.g.) pi0s

			//save it! //in creation order!
			locSourceCombosByUseSoFar[locComboUseToCreate]->push_back(locCombo);
			Register_ValidRFBunches(locComboUseToCreate, locCombo, locValidRFBunches, locComboingStage, locChargedCombo_WithNow);

			//finally, in case we add more (e.g.) pi0s later (N + 1), save the last pi0
			//so that we will start the search for the next (e.g.) pi0 in the location after the last one
			dResumeSearchAfterMap_Combos[locCombo] = locDecayCombo_1;
		}
	}
}

void DSourceComboer::Combo_Vertically_AllParticles(const DSourceComboUse& locComboUseToCreate, ComboingStage_t locComboingStage)
{
	//get combo use contents
	auto locVertexZBin = std::get<1>(locComboUseToCreate);
	auto locNumParticlesNeeded = std::get<2>(locComboUseToCreate)->Get_NumParticles();
	auto locFurtherDecays = std::get<2>(locComboUseToCreate)->Get_FurtherDecays();

	//Get combos so far //guaranteed not to be mixed
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, d_Neutral); //if not neutral then is on charged stage: argument doesn't matter

	//for each further decay map entry (e.g. pi0, 3), this is a collection of the uses representing those groupings //e.g. Unknown -> 3pi0
	for(const auto& locParticlePair : locNumParticlesNeeded)
	{
		//get PID information
		auto& locPID = locParticlePair.first; //e.g. pi0, -> 2g
		auto& locNumPIDNeeded = locParticlePair.second; //N of the above decay (e.g. pi0s)

		if(locNumPIDNeeded == 1)
			continue; //nothing to do vertically; we will combo this horizontally later

		if((locComboingStage == d_ChargedStage) && (ParticleCharge(locPID) == 0))
			continue; //skip for now!!

		//OK, so we need a grouping of N > 1 particles with the same PID (e.g. g's)
		//so, let's create a use of Unknown -> N g's (e.g.)
		//if we can just utilize the use from the input combo-info, then we will. if not, we'll make a new one
		DSourceComboUse locNeededGroupingUse = locComboUseToCreate;
		if((locNumParticlesNeeded.size() > 1) || !locFurtherDecays.empty()) //if true: can't use the input
		{
			auto locGroupingComboInfo = GetOrMake_SourceComboInfo({std::make_pair(locPID, locNumPIDNeeded)}, {}); // -> N g's (e.g.)
			locNeededGroupingUse = std::make_tuple(Unknown, locVertexZBin, locGroupingComboInfo); // Unknown -> N g's (e.g.)
		}

		//See whether the combos for this grouping have already been done
		if(locSourceCombosByUseSoFar.find(locNeededGroupingUse) != locSourceCombosByUseSoFar.end())
			continue; //it's already done!!

		//it's not already done.  darn it.
		//if it's a direct combo of 2 particles, just make it and continue
		if(locNumPIDNeeded == 2)
		{
			Combo_Vertically_NParticles(locNeededGroupingUse, DSourceComboUse(), locComboingStage);
			continue;
		}

		//build an info and a use for a direct grouping of N - 1 particles //e.g. 3 g's
		auto locNMinus1Info = GetOrMake_SourceComboInfo({std::make_pair(locPID, locNumPIDNeeded - 1)}, {}); // N - 1 g's (e.g.), no decaying particles
		DSourceComboUse locNMinus1ComboUse(Unknown, locVertexZBin, locNMinus1Info); // Unknown -> N - 1 g's (e.g.)

		// Now, see whether the combos for the direct N - 1 grouping have already been done.  If not, create them
		if(locSourceCombosByUseSoFar.find(locNMinus1ComboUse) != locSourceCombosByUseSoFar.end())
			Combo_Vertically_AllParticles(locNMinus1ComboUse, locComboingStage); //no need to go to top-level combo function since just N - 1: can re-call this one

		//Finally, we can actually DO the grouping, between the N - 1 particles and one more particle
		Combo_Vertically_NParticles(locNeededGroupingUse, locNMinus1ComboUse, locComboingStage);
	}
}

void DSourceComboer::Combo_Vertically_NParticles(const DSourceComboUse& locComboUseToCreate, const DSourceComboUse& locNMinus1ComboUse, ComboingStage_t locComboingStage)
{
	//either: combining two particles with the same PID to create a new combo, or combining a combo of N particles (with the same PID) with one more particle
	auto locComboInfo = std::get<2>(locComboUseToCreate);
	auto locParticlePair = locComboInfo->Get_NumParticles().back(); //is guaranteed to be size 1
	auto locPID = locParticlePair.first;
	auto locNumParticles = locParticlePair.second;
	auto locVertexZBin = std::get<1>(locComboUseToCreate);

	//Get combos so far //guaranteed not to be mixed
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, d_Neutral); //if not neutral then is on charged stage: argument doesn't matter

	//if on the all-showers stage, first copy over ALL fcal-only results
	if(locComboingStage == d_MixedStage)
		Copy_ZIndependentMixedResults(locComboUseToCreate, nullptr);
	else //initialize vector for storing results
	{
		locSourceCombosByUseSoFar.emplace(locComboUseToCreate, dResourcePool_SourceComboVector.Get_Resource());
		locSourceCombosByUseSoFar[locComboUseToCreate]->reserve(dInitialComboVectorCapacity);
	}

	if(locNumParticles == 2)
	{
		//Get particles for comboing
		const auto& locParticles = Get_ParticlesForComboing(locPID, locComboingStage, {}, locVertexZBin);

		auto locLastIteratorToCheck = std::prev(locParticles.end());
		for(auto locFirstIterator = locParticles.begin(); locFirstIterator != locLastIteratorToCheck; ++locFirstIterator)
		{
			auto locRFBunches_First = (locPID == Gamma) ? dSourceComboTimeHandler->Get_ValidRFBunches(*locFirstIterator, locVertexZBin) : vector<int>{};
			for(auto locSecondIterator = std::next(locFirstIterator); locSecondIterator != locParticles.end(); ++locSecondIterator)
			{
				auto locIsZIndependent = (locComboingStage == d_MixedStage_ZIndependent) || (Get_IsComboingZIndependent(*locFirstIterator, locPID) && Get_IsComboingZIndependent(*locSecondIterator, locPID));
				if((locComboingStage == d_MixedStage) && locIsZIndependent)
					continue; //this combo has already been created (assuming it was valid): during the FCAL-only stage

				//See which RF bunches match up, if any //if charged or massive neutrals, ignore (they don't choose at this stage)
				auto locValidRFBunches = (locPID != Gamma) ? vector<int>{} : dSourceComboTimeHandler->Get_CommonRFBunches(locRFBunches_First, *locSecondIterator, locVertexZBin);
				if((locPID == Gamma) && locValidRFBunches.empty())
					continue;

				auto locCombo = dResourcePool_SourceCombo.Get_Resource();
				locCombo->Set_Members({std::make_pair(locPID, *locFirstIterator), std::make_pair(locPID, *locSecondIterator)}, {}, locIsZIndependent);
				locSourceCombosByUseSoFar[locComboUseToCreate]->push_back(locCombo); //save it //in creation order

				Register_ValidRFBunches(locComboUseToCreate, locCombo, locValidRFBunches, locComboingStage, nullptr);

				//in case we add more particles with the same PID later (N + 1), save last object with this PID
				//so that we will start the search for the next particle one spot after it
				dResumeSearchAfterMap_Particles[locCombo] = *locSecondIterator;
			}
		}
		return;
	}

	//create combo of N same-PID-particles by adding one particle to previously-created combos of N - 1 same-PID-particles
	const auto& locCombos_NMinus1 = *locSourceCombosByUseSoFar[locNMinus1ComboUse]; //Each combo contains a vector of N - 1 same-PID-particles
	for(const auto& locCombo_NMinus1 : locCombos_NMinus1)
	{
		//Get particles for comboing
		const auto& locValidRFBunches_NMinus1 = dValidRFBunches_ByCombo[locCombo_NMinus1];
		const auto& locParticles = Get_ParticlesForComboing(locPID, locComboingStage, locValidRFBunches_NMinus1, locVertexZBin);

		//retrieve where to begin the search
		auto locParticleSearchIterator = Get_ResumeAtIterator_Particles(locCombo_NMinus1, locValidRFBunches_NMinus1);
		if(locParticleSearchIterator == std::end(locParticles))
			continue; //e.g. this combo is "AD" and there are only 4 reconstructed combos (ABCD): no potential matches! move on to the next N - 1 combo

		auto locIsZIndependent_NMinus1 = locCombo_NMinus1->Get_IsComboingZIndependent();

		for(; locParticleSearchIterator != locParticles.end(); ++locParticleSearchIterator)
		{
			auto locIsZIndependent = (locComboingStage == d_MixedStage_ZIndependent) || (locIsZIndependent_NMinus1 && Get_IsComboingZIndependent(*locParticleSearchIterator, locPID));
			if((locComboingStage == d_MixedStage) && locIsZIndependent)
				continue; //this combo has already been created (assuming it was valid): during the FCAL-only stage

			//See which RF bunches match up //guaranteed to be at least one, due to selection in Get_ParticlesForComboing() function
			//if charged or massive neutrals, ignore (they don't choose at this stage)
			auto locValidRFBunches = (locPID != Gamma) ? vector<int>{} : dSourceComboTimeHandler->Get_CommonRFBunches(locValidRFBunches_NMinus1, *locParticleSearchIterator, locVertexZBin);

			auto locComboParticlePairs = locCombo_NMinus1->Get_SourceParticles();
			locComboParticlePairs.emplace_back(locPID, *locParticleSearchIterator);
			auto locCombo = dResourcePool_SourceCombo.Get_Resource();
			locCombo->Set_Members(locComboParticlePairs, {}, locIsZIndependent);
			locSourceCombosByUseSoFar[locComboUseToCreate]->push_back(locCombo); //save it //in creation order

			Register_ValidRFBunches(locComboUseToCreate, locCombo, locValidRFBunches, locComboingStage, nullptr);

			//in case we add more particles with the same PID later (N + 1), save last object with this PID
			//so that we will start the search for the next particle one spot after it
			dResumeSearchAfterMap_Particles[locCombo] = *locParticleSearchIterator;
		}
	}
}

/************************************************************* BUILD PHOTON COMBOS - HORIZONTALLY ***************************************************************/

void DSourceComboer::Combo_Horizontally_All(const DSourceComboUse& locComboUseToCreate, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_Presiding)
{
	//get combo use contents
	auto locVertexZBin = std::get<1>(locComboUseToCreate);
	const auto& locComboInfoToCreate = std::get<2>(locComboUseToCreate);
	auto locNumParticlesNeeded = locComboInfoToCreate->Get_NumParticles();
	auto locFurtherDecays = locComboInfoToCreate->Get_FurtherDecays();

	//first handle special cases:
	if(locNumParticlesNeeded.empty() && (locFurtherDecays.size() == 1))
		return; //e.g. we just need N pi0s together: already done when comboing vertically!!
	if(locFurtherDecays.empty() && (locNumParticlesNeeded.size() == 1))
	{
		//we just need N (e.g.) photons together
		auto& locParticlePair = locNumParticlesNeeded.front();
		if(locParticlePair.second > 1)
			return; //already done when comboing vertically!!

		//not much of a combo if there's only 1, is it? //e.g. 1 charged track at a vertex
		if((locComboingStage == d_ChargedStage) && (ParticleCharge(locParticlePair.first) == 0))
			return; //skip for now!!
		Create_Combo_OneParticle(locComboUseToCreate, locComboingStage);
		return;
	}

	//see if there is another combo that already exists that is a subset of what we requested
	//e.g. if we need a charged combo, a neutral combo, and a mixed: search for:
		//charged + neutral (no mixed)
		//charged + mixed (no neutral)
		//neutral + mixed (no charged)
	//e.g. if we need 2pi0s, one omega, and 1g: search for:
		//2pi0s, one omega: if exists, just combo that with 1g
		//2pi0s, one photon: if exists, just combo with one omega
		//etc.

	//save in case need to create these
	DSourceComboUse locComboUse_SubsetToBuild(Unknown, locVertexZBin, nullptr);

	//for each further decay map entry (e.g. pi0, 3), this is a collection of the uses representing those groupings //e.g. Unknown -> 3pi0
	//decays are sorted by: mixed-charge first, then fully-neutral, then fully-charged
	//within a charge: loop from heaviest-mass to least (most likely to be missing)
	auto locChargedCombo_WithNow = Get_ChargedCombo_WithNow(locChargedCombo_Presiding);
	for(auto locDecayIterator = locFurtherDecays.begin(); locDecayIterator != locFurtherDecays.end(); ++locDecayIterator)
	{
		//build a DSourceComboUse with everything EXCEPT this set of decays, and see if it already exists
		//build the further-decays, removing this decay
		auto locFurtherDecaysToSearchFor = locFurtherDecays;
		const auto& locSourceComboUse_ThisDecay = locDecayIterator->first;
		auto locChargeContent_ThisDecay = dComboInfoChargeContent[std::get<2>(locSourceComboUse_ThisDecay)];
		locFurtherDecaysToSearchFor.erase(locFurtherDecaysToSearchFor.begin() + std::distance(locFurtherDecays.begin(), locDecayIterator));

		//build the all-but-1 DSourceComboUse
		auto locAllBut1ComboInfo = GetOrMake_SourceComboInfo(locNumParticlesNeeded, locFurtherDecaysToSearchFor);
		DSourceComboUse locAllBut1ComboUse{Unknown, locVertexZBin, locAllBut1ComboInfo}; // Unknown -> everything but this decay

		auto locAllBut1ChargeContent = dComboInfoChargeContent[std::get<2>(locAllBut1ComboUse)];
		if((locComboingStage == d_ChargedStage) && (locAllBut1ChargeContent == d_Neutral))
			continue; //this won't be done yet!

		if((locComboingStage != d_ChargedStage) && (locAllBut1ChargeContent == d_Charged))
		{
			//yes, it's already been done!
			//just combo the All-but-1 combos to those from this decay and return the results
			//don't promote particles or expand all-but-1: create new combo ABOVE all-but-1, that will contain all-but-1 and to-add side-by-side
			Combo_Horizontally_AddCombo(locComboUseToCreate, locAllBut1ComboUse, locSourceComboUse_ThisDecay, locComboingStage, locChargedCombo_WithNow, false);
			return;
		}

		//Get combos so far
		auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, dComboInfoChargeContent[locAllBut1ComboInfo], locChargedCombo_WithNow);

		// Now, see whether the combos for this grouping have already been done
		if(locSourceCombosByUseSoFar.find(locAllBut1ComboUse) == locSourceCombosByUseSoFar.end()) //if true: not yet
		{
			//if on the first one (heaviest mass), save this subset in case we need to create it (if nothing else already done)
			if(locDecayIterator == locFurtherDecays.begin())
				locComboUse_SubsetToBuild = locAllBut1ComboUse;
			continue; // try the next decay
		}

		//yes, it's already been done!
		//just combo the All-but-1 combos to those from this decay and save the results
		if((locComboingStage == d_ChargedStage) && (locChargeContent_ThisDecay == d_Neutral))
		{
			//this won't be done yet! just copy the all-but-1 as the desired combos
			auto& locAllBut1Combos = locSourceCombosByUseSoFar[locAllBut1ComboUse];
			locSourceCombosByUseSoFar.emplace(locComboUseToCreate, locAllBut1Combos);
		}
		else
		{
			bool locExpandAllBut1Flag = (locAllBut1ComboInfo->Get_NumParticles().size() + locAllBut1ComboInfo->Get_FurtherDecays().size()) > 1; //true: has already been comboed horizontally once
			Combo_Horizontally_AddCombo(locComboUseToCreate, locAllBut1ComboUse, locSourceComboUse_ThisDecay, locComboingStage, locChargedCombo_WithNow, locExpandAllBut1Flag);
		}
		return;
	}

	//ok, none of the subsets without a decay has yet been created. let's try subsets without detected particles
	if((locComboingStage == d_ChargedStage) || (dComboInfoChargeContent[locComboInfoToCreate] == d_Neutral)) //no loose particles when mixing charged & neutral
	{
		for(auto locParticleIterator = locNumParticlesNeeded.begin(); locParticleIterator != locNumParticlesNeeded.end(); ++locParticleIterator)
		{
			//build a DSourceComboUse with everything EXCEPT this set of particles, and see if it already exists
			//combo the particle horizontally, removing this PID
			auto locNumParticlesToSearchFor = locNumParticlesNeeded;
			const auto& locParticlePair = *locParticleIterator;
			locNumParticlesToSearchFor.erase(locNumParticlesToSearchFor.begin() + std::distance(locNumParticlesNeeded.begin(), locParticleIterator));

			//build the DSourceComboUse
			auto locAllBut1ComboInfo = GetOrMake_SourceComboInfo(locNumParticlesToSearchFor, locFurtherDecays);
			if((locComboingStage == d_ChargedStage) && (dComboInfoChargeContent[locAllBut1ComboInfo] == d_Neutral))
				continue; //this won't be done yet!
			DSourceComboUse locAllBut1ComboUse(Unknown, locVertexZBin, locAllBut1ComboInfo); // Unknown -> everything but these particles

			//Get combos so far
			auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, d_Neutral); //if not neutral then is on charged stage: argument doesn't matter

			// Now, see whether the combos for this grouping have already been done
			if(locSourceCombosByUseSoFar.find(locAllBut1ComboUse) == locSourceCombosByUseSoFar.end()) //if true: not yet
			{
				//if on the first one and there's no decays, save this subset in case we need to create it (if nothing else already done)
				if((locParticleIterator == locNumParticlesNeeded.begin()) && locFurtherDecays.empty())
					locComboUse_SubsetToBuild = locAllBut1ComboUse;
				continue; // try the next PID
			}

			//yes, it's already been done!
			//just combo the All-but-1 combos to those from this particle and return the results
			if((locComboingStage == d_ChargedStage) && (ParticleCharge(locParticlePair.first) == 0))
			{
				//this won't be done yet! just copy the all-but-1 as the desired combos
				auto& locAllBut1Combos = locSourceCombosByUseSoFar[locAllBut1ComboUse];
				locSourceCombosByUseSoFar.emplace(locComboUseToCreate, locAllBut1Combos);
				return;
			}

			if(locParticlePair.second > 1)
			{
				//create a combo use for X -> N particles of this type
				auto locSourceInfo_NParticles = GetOrMake_SourceComboInfo({locParticlePair}, {});
				DSourceComboUse locSourceComboUse_NParticles(Unknown, locVertexZBin, locSourceInfo_NParticles);
				bool locExpandAllBut1Flag = (locAllBut1ComboInfo->Get_NumParticles().size() + locAllBut1ComboInfo->Get_FurtherDecays().size()) > 1; //true: has already been comboed horizontally once
				Combo_Horizontally_AddCombo(locComboUseToCreate, locAllBut1ComboUse, locSourceComboUse_NParticles, locComboingStage, locChargedCombo_WithNow, locExpandAllBut1Flag);
			}
			else
				Combo_Horizontally_AddParticle(locComboUseToCreate, locAllBut1ComboUse, locParticlePair.first, locComboingStage, locChargedCombo_WithNow);
			return;
		}
	}

	//none of the possible immediate subsets have been created
	//therefore, create one of them (the one without the heaviest particle), and then do the remaining combo
	Combo_Horizontally_All(locComboUse_SubsetToBuild, locComboingStage, locChargedCombo_WithNow);
	auto locComboInfo_SubsetToBuild = std::get<2>(locComboUse_SubsetToBuild);
	bool locExpandAllBut1Flag = (locComboInfo_SubsetToBuild->Get_NumParticles().size() + locComboInfo_SubsetToBuild->Get_FurtherDecays().size()) > 1; //true: has already been comboed horizontally once

	//do the final combo!
	if(locFurtherDecays.empty())
	{
		//subset was missing a detected PID
		const auto& locParticlePair = locNumParticlesNeeded.front();
		if((locComboingStage == d_ChargedStage) && (ParticleCharge(locParticlePair.first) == 0))
		{
			//this won't be done yet! just copy the all-but-1 as the desired combos
			auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, d_Charged);
			auto& locAllBut1Combos = locSourceCombosByUseSoFar[locComboUse_SubsetToBuild];
			locSourceCombosByUseSoFar.emplace(locComboUseToCreate, locAllBut1Combos);
			return;
		}
		if(locParticlePair.second > 1)
		{
			//create a combo use for X -> N particles of this type
			auto locSourceInfo_NParticles = GetOrMake_SourceComboInfo({locParticlePair}, {});
			DSourceComboUse locSourceComboUse_NParticles(Unknown, locVertexZBin, locSourceInfo_NParticles);
			Combo_Horizontally_AddCombo(locComboUseToCreate, locComboUse_SubsetToBuild, locSourceComboUse_NParticles, locComboingStage, locChargedCombo_WithNow, locExpandAllBut1Flag);
		}
		else
			Combo_Horizontally_AddParticle(locComboUseToCreate, locComboUse_SubsetToBuild, locParticlePair.first, locComboingStage, locChargedCombo_WithNow);
	}
	else //subset was missing a decay PID
	{
		auto locComboUseToAdd = locFurtherDecays.front().first;
		if((locComboingStage == d_ChargedStage) && (Get_ChargeContent(std::get<2>(locComboUseToAdd)) == d_Neutral))
		{
			//this won't be done yet! just copy the all-but-1 as the desired combos
			auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, d_Charged);
			auto& locAllBut1Combos = locSourceCombosByUseSoFar[locComboUse_SubsetToBuild];
			locSourceCombosByUseSoFar.emplace(locComboUseToCreate, locAllBut1Combos);
		}
		else
			Combo_Horizontally_AddCombo(locComboUseToCreate, locComboUse_SubsetToBuild, locComboUseToAdd, locComboingStage, locChargedCombo_WithNow, locExpandAllBut1Flag);
	}
}

void DSourceComboer::Create_Combo_OneParticle(const DSourceComboUse& locComboUseToCreate, ComboingStage_t locComboingStage)
{
	//not much of a combo if there's only 1, is it? //e.g. 1 charged track at a vertex

	//Get combos so far
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, d_Neutral); //if not neutral then is on charged stage: argument doesn't matter

	//get combo use contents
	auto locVertexZBin = std::get<1>(locComboUseToCreate);
	auto locParticlePair = std::get<2>(locComboUseToCreate)->Get_NumParticles().front();

	//if on the mixed stage, must be doing all neutrals: first copy over ALL fcal-only results
	if(locComboingStage == d_MixedStage)
		Copy_ZIndependentMixedResults(locComboUseToCreate, nullptr);
	else //initialize vector for storing results
	{
		locSourceCombosByUseSoFar.emplace(locComboUseToCreate, dResourcePool_SourceComboVector.Get_Resource());
		locSourceCombosByUseSoFar[locComboUseToCreate]->reserve(dInitialComboVectorCapacity);
	}

	auto locPID = locParticlePair.first;

	//Get particles for comboing
	const auto& locParticles = Get_ParticlesForComboing(locPID, locComboingStage, {}, locVertexZBin);
	for(const auto& locParticle : locParticles)
	{
		auto locIsZIndependent = Get_IsComboingZIndependent(locParticle, locPID);
		if((locComboingStage == d_MixedStage) && locIsZIndependent)
			continue; //this combo has already been created (assuming it was valid): during the FCAL-only stage

		auto locCombo = dResourcePool_SourceCombo.Get_Resource();
		locCombo->Set_Members({std::make_pair(locPID, locParticle)}, {}, locIsZIndependent);
		locSourceCombosByUseSoFar[locComboUseToCreate]->push_back(locCombo); //save it //in creation order
		if(locPID == Gamma)
			Register_ValidRFBunches(locComboUseToCreate, locCombo, dSourceComboTimeHandler->Get_ValidRFBunches(locParticle, locVertexZBin), locComboingStage, nullptr);
		else
			Register_ValidRFBunches(locComboUseToCreate, locCombo, {}, locComboingStage, nullptr);
	}
}

void DSourceComboer::Combo_Horizontally_AddCombo(const DSourceComboUse& locComboUseToCreate, const DSourceComboUse& locAllBut1ComboUse, const DSourceComboUse& locSourceComboUseToAdd, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_Presiding, bool locExpandAllBut1Flag)
{
	//e.g. we are grouping N pi0s and M photons (> 1) with L etas (>= 1), etc. to make combos
	//so, let's get the combos for the main grouping

	//Get combos so far
	auto locComboInfo_AllBut1 = std::get<2>(locAllBut1ComboUse);
	auto locChargeContent_AllBut1 = dComboInfoChargeContent[locComboInfo_AllBut1];
	auto locChargedCombo_WithNow = Get_ChargedCombo_WithNow(locChargedCombo_Presiding);
	auto& locSourceCombosByUseToSaveTo = Get_CombosSoFar(locComboingStage, dComboInfoChargeContent[std::get<2>(locComboUseToCreate)], locChargedCombo_WithNow);

	bool locGetFromSoFarFlag = (locComboingStage == d_ChargedStage) || (locChargeContent_AllBut1 != d_Charged);
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, locChargeContent_AllBut1, locChargedCombo_WithNow);

	vector<const DSourceCombo*> locChargedComboVector = {locChargedCombo_WithNow}; //ugh
	auto locCombos_AllBut1 = locGetFromSoFarFlag ? locSourceCombosByUseSoFar[locAllBut1ComboUse] : &locChargedComboVector; //Combos are a vector of (e.g.): -> N pi0s

	auto locChargeContent = dComboInfoChargeContent[std::get<2>(locSourceComboUseToAdd)];
	if((locComboingStage == d_ChargedStage) && (locChargeContent == d_Neutral))
	{
		//can't add neutrals, so we are already done! just copy the results to the new vector
		locSourceCombosByUseToSaveTo.emplace(locComboUseToCreate, locCombos_AllBut1);
		return;
	}

	//if on the all-showers stage, first copy over ALL fcal-only results
	if(locComboingStage == d_MixedStage)
		Copy_ZIndependentMixedResults(locComboUseToCreate, locChargedCombo_WithNow);
	else //initialize vector for storing results
	{
		locSourceCombosByUseToSaveTo.emplace(locComboUseToCreate, dResourcePool_SourceComboVector.Get_Resource());
		locSourceCombosByUseToSaveTo[locComboUseToCreate]->reserve(dInitialComboVectorCapacity);
	}

	auto locDecayPID_UseToAdd = std::get<0>(locSourceComboUseToAdd);
	auto locComboInfo_UseToAdd = std::get<2>(locSourceComboUseToAdd);

	//check if on mixed stage but comboing to charged
	if((locComboingStage != d_ChargedStage) && (locChargeContent == d_Charged))
	{
		//only one valid option: locChargedCombo_WithNow: create all combos immediately
		for(const auto& locCombo_AllBut1 : *locCombos_AllBut1)
		{
			auto locIsZIndependent = locCombo_AllBut1->Get_IsComboingZIndependent();
			if((locComboingStage == d_MixedStage) && locIsZIndependent)
				continue; //this combo has already been created (assuming it was valid): during the FCAL-only stage

			//get the valid RF bunches (those for the all-but-1, because we are comboing with charged which is "all")
			const auto& locValidRFBunches = dValidRFBunches_ByCombo[locCombo_AllBut1];

			//create new combo!
			auto locCombo = dResourcePool_SourceCombo.Get_Resource();

			//get contents of the all-but-1 so that we can add to them
			auto locFurtherDecayCombos_AllBut1 = locCombo_AllBut1->Get_FurtherDecayCombos(); //the all-but-1 combo contents by use
			auto locComboParticles_AllBut1 = locCombo_AllBut1->Get_SourceParticles();

			if(locExpandAllBut1Flag)
			{
				locFurtherDecayCombos_AllBut1.emplace(locSourceComboUseToAdd, vector<const DSourceCombo*>{locChargedCombo_WithNow});
				locCombo->Set_Members(locComboParticles_AllBut1, locFurtherDecayCombos_AllBut1, locIsZIndependent); // create combo with all PIDs
			}
			else //side by side in a new combo
			{
				DSourceCombosByUse_Small locFurtherDecayCombos_Needed;
				locFurtherDecayCombos_Needed.emplace(locAllBut1ComboUse, vector<const DSourceCombo*>{locCombo_AllBut1});
				locFurtherDecayCombos_Needed.emplace(locSourceComboUseToAdd, vector<const DSourceCombo*>{locChargedCombo_WithNow});
				locCombo->Set_Members({}, locFurtherDecayCombos_Needed, locIsZIndependent); // create combo with all PIDs
			}

			//save it! //in creation order!
			locSourceCombosByUseToSaveTo[locComboUseToCreate]->push_back(locCombo);
			Register_ValidRFBunches(locComboUseToCreate, locCombo, locValidRFBunches, locComboingStage, locChargedCombo_WithNow);
		}
	}

	//determine whether we should promote the contents of the combos we are combining up to the new combo (else set combo as decay of new combo)
	auto locComboInfo_UseToCreate = std::get<2>(locComboUseToCreate);
	bool locPromoteToAddFlag = Get_PromoteFlag(locDecayPID_UseToAdd, locComboInfo_UseToCreate, locComboInfo_UseToAdd);
	bool locPromoteAllBut1Flag = Get_PromoteFlag(std::get<0>(locAllBut1ComboUse), locComboInfo_UseToCreate, locComboInfo_AllBut1);

	//get the previous charged combo (if needed)
	const DSourceCombo* locChargedCombo_WithPrevious = Get_ChargedCombo_WithNow(Get_Presiding_ChargedCombo(locChargedCombo_Presiding, locSourceComboUseToAdd, locComboingStage, 1));

	//now, for each combo of all-but-1-PIDs, see which of the to-add combos we can group to it
	//valid grouping: Don't re-use a shower we've already used
	for(const auto& locCombo_AllBut1 : *locCombos_AllBut1)
	{
		//first of all, get the potential combos that satisfy the RF bunches for the all-but-1 combo
		const auto& locValidRFBunches_AllBut1 = dValidRFBunches_ByCombo[locCombo_AllBut1];
		const auto& locDecayCombos_ToAdd = Get_CombosForComboing(locSourceComboUseToAdd, locComboingStage, locValidRFBunches_AllBut1, locChargedCombo_WithPrevious);

		//before we loop, first get all of the showers used to make the all-but-1 grouping, and sort it so that we can quickly search it
		auto locUsedParticles_AllBut1 = DAnalysis::Get_SourceParticles(locCombo_AllBut1->Get_SourceParticles(true)); //true: entire chain
		std::sort(locUsedParticles_AllBut1.begin(), locUsedParticles_AllBut1.end()); //must sort, because when retrieving entire chain is unsorted

		//this function will do our validity test
		auto Search_Duplicates = [&locUsedParticles_AllBut1](const JObject* locParticle) -> bool
			{return std::binary_search(locUsedParticles_AllBut1.begin(), locUsedParticles_AllBut1.end(), locParticle);};

		auto locIsZIndependent_AllBut1 = locCombo_AllBut1->Get_IsComboingZIndependent();

		//loop over potential combos to add to the group, creating a new combo for each valid (non-duplicate) grouping
		for(const auto& locDecayCombo_ToAdd : locDecayCombos_ToAdd)
		{
			auto locIsZIndependent = (locIsZIndependent_AllBut1 && locDecayCombo_ToAdd->Get_IsComboingZIndependent());
			if((locComboingStage == d_MixedStage) && locIsZIndependent)
				continue; //this combo has already been created (assuming it was valid): during the FCAL-only stage

			//search the all-but-1 shower vector to see if any of the showers in this combo are duplicated
			auto locUsedParticles_ToAdd = DAnalysis::Get_SourceParticles(locDecayCombo_ToAdd->Get_SourceParticles(true)); //true: entire chain

			//conduct search
			if(std::any_of(locUsedParticles_ToAdd.begin(), locUsedParticles_ToAdd.end(), Search_Duplicates))
				continue; //at least one photon was a duplicate, this combo won't work

			//no duplicates: this combo is unique.  build a new combo

			//See which RF bunches match up //guaranteed to be at least one, due to selection in Get_CombosForComboing() function
			vector<int> locValidRFBunches = {}; //if charged or massive neutrals, ignore (they don't choose at this stage)
			if(locComboingStage != d_ChargedStage)
				locValidRFBunches = dSourceComboTimeHandler->Get_CommonRFBunches(locValidRFBunches_AllBut1, dValidRFBunches_ByCombo[locDecayCombo_ToAdd]);

			//create new combo!
			auto locCombo = dResourcePool_SourceCombo.Get_Resource();

			//get contents of the all-but-1 so that we can add to them
			auto locFurtherDecayCombos_AllBut1 = locCombo_AllBut1->Get_FurtherDecayCombos(); //the all-but-1 combo contents by use
			auto locComboParticles_AllBut1 = locCombo_AllBut1->Get_SourceParticles(false);

			if(locExpandAllBut1Flag)
			{
				if(locPromoteToAddFlag)
				{
					//promote all contents of to-add to the all-but-1 level
					auto locUsedParticlePairs_ToAdd = locDecayCombo_ToAdd->Get_SourceParticles(false);
					locComboParticles_AllBut1.insert(locComboParticles_AllBut1.end(), locUsedParticlePairs_ToAdd.begin(), locUsedParticlePairs_ToAdd.end());
					auto locFurtherDecayCombos_ToAdd = locDecayCombo_ToAdd->Get_FurtherDecayCombos();
					locFurtherDecayCombos_AllBut1.insert(locFurtherDecayCombos_ToAdd.begin(), locFurtherDecayCombos_ToAdd.end());
				}
				else
					locFurtherDecayCombos_AllBut1.emplace(locSourceComboUseToAdd, vector<const DSourceCombo*>{locDecayCombo_ToAdd});
				locCombo->Set_Members(locComboParticles_AllBut1, locFurtherDecayCombos_AllBut1, locIsZIndependent); // create combo with all PIDs
			}
			else //side by side in a new combo
			{
				auto locUsedParticlePairs_ToAdd = locDecayCombo_ToAdd->Get_SourceParticles(false);
				auto locFurtherDecayCombos_ToAdd = locDecayCombo_ToAdd->Get_FurtherDecayCombos();
				if(locPromoteAllBut1Flag)
				{
					//promote contents of all-but-1 to the to-add level
					locUsedParticlePairs_ToAdd.insert(locUsedParticlePairs_ToAdd.end(), locComboParticles_AllBut1.begin(), locComboParticles_AllBut1.end());
					locFurtherDecayCombos_ToAdd.insert(locFurtherDecayCombos_AllBut1.begin(), locFurtherDecayCombos_AllBut1.end());
					locCombo->Set_Members(locUsedParticlePairs_ToAdd, locFurtherDecayCombos_ToAdd, locIsZIndependent); // create combo with all PIDs
				}
				else if(locPromoteToAddFlag)
				{
					//promote contents of to-add to the all-but-1 level
					locComboParticles_AllBut1.insert(locComboParticles_AllBut1.end(), locUsedParticlePairs_ToAdd.begin(), locUsedParticlePairs_ToAdd.end());
					locFurtherDecayCombos_AllBut1.insert(locFurtherDecayCombos_ToAdd.begin(), locFurtherDecayCombos_ToAdd.end());
					locCombo->Set_Members(locComboParticles_AllBut1, locFurtherDecayCombos_AllBut1, locIsZIndependent); // create combo with all PIDs
				}
				else
				{
					DSourceCombosByUse_Small locFurtherDecayCombos_Needed;
					locFurtherDecayCombos_Needed.emplace(locAllBut1ComboUse, vector<const DSourceCombo*>{locCombo_AllBut1});
					locFurtherDecayCombos_Needed.emplace(locSourceComboUseToAdd, vector<const DSourceCombo*>{locDecayCombo_ToAdd});
					locCombo->Set_Members({}, locFurtherDecayCombos_Needed, locIsZIndependent); // create combo with all PIDs
				}
			}

			//save it! //in creation order!
			locSourceCombosByUseToSaveTo[locComboUseToCreate]->push_back(locCombo);
			Register_ValidRFBunches(locComboUseToCreate, locCombo, locValidRFBunches, locComboingStage, locChargedCombo_WithNow);
		}
	}
}

void DSourceComboer::Combo_Horizontally_AddParticle(const DSourceComboUse& locComboUseToCreate, const DSourceComboUse& locAllBut1ComboUse, Particle_t locPID, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_Presiding)
{
	//Get combos so far
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(locComboingStage, d_Neutral); //if not neutral then is on charged stage: argument doesn't matter

	//e.g. we are grouping a whole bunch of particles and decays with a lone particle to make new combos
	//so, let's get the combos for this initial grouping
	auto locChargedCombo_WithNow = Get_ChargedCombo_WithNow(locChargedCombo_Presiding);
	vector<const DSourceCombo*> locChargedComboVector = {locChargedCombo_WithNow}; //ugh
	auto locChargeContent_AllBut1 = dComboInfoChargeContent[std::get<2>(locAllBut1ComboUse)];
	bool locGetFromSoFarFlag = (locComboingStage == d_ChargedStage) || (locChargeContent_AllBut1 != d_Charged);
	auto locCombos_AllBut1 = locGetFromSoFarFlag ? locSourceCombosByUseSoFar[locAllBut1ComboUse] : &locChargedComboVector; //Combos are a vector of (e.g.): -> N pi0s

	if((locComboingStage == d_ChargedStage) && (ParticleCharge(locPID) == 0))
	{
		//can't add neutrals, so we are already done! just copy the results to the new vector
		locSourceCombosByUseSoFar[locComboUseToCreate] = locCombos_AllBut1;
		return;
	}

	//if on the all-showers stage, first copy over ALL fcal-only results
	if(locComboingStage == d_MixedStage)
		Copy_ZIndependentMixedResults(locComboUseToCreate, nullptr);
	else //initialize vector for storing results
	{
		locSourceCombosByUseSoFar.emplace(locComboUseToCreate, dResourcePool_SourceComboVector.Get_Resource());
		locSourceCombosByUseSoFar[locComboUseToCreate]->reserve(dInitialComboVectorCapacity);
	}

	auto locVertexZBin = std::get<1>(locComboUseToCreate);

	//loop over the combos
	for(const auto& locCombo_AllBut1 : *locCombos_AllBut1)
	{
		//now, for each combo of all-but-1-PIDs, see which of the particles can group to it
		//valid grouping: Don't re-use a particle we've already used

		//before we loop, first get all of the particles of the given PID used to make the all-but-1 grouping, and sort it so that we can quickly search it
		auto locUsedParticlePairs_AllBut1 = locCombo_AllBut1->Get_SourceParticles(true);
		auto locUsedParticles_AllBut1 = DAnalysis::Get_SourceParticles(locUsedParticlePairs_AllBut1, locPID); //true: entire chain
		std::sort(locUsedParticles_AllBut1.begin(), locUsedParticles_AllBut1.end()); //necessary: may be out of order due to comboing of different decays

		//also, pre-get the further decays & FCAL-only flag, as we'll need them to build new combos
		auto locFurtherDecays = locCombo_AllBut1->Get_FurtherDecayCombos(); //the all-but-1 combo contents by use
		auto locIsZIndependent_AllBut1 = locCombo_AllBut1->Get_IsComboingZIndependent();

		//Get potential particles for comboing
		const auto& locValidRFBunches_AllBut1 = dValidRFBunches_ByCombo[locCombo_AllBut1];
		const auto& locParticles = Get_ParticlesForComboing(locPID, locComboingStage, locValidRFBunches_AllBut1, locVertexZBin);

		//loop over potential showers to add to the group, creating a new combo for each valid (non-duplicate) grouping
		for(const auto& locParticle : locParticles)
		{
			auto locIsZIndependent = (locComboingStage == d_MixedStage_ZIndependent) || (locIsZIndependent_AllBut1 && Get_IsComboingZIndependent(locParticle, locPID));
			if((locComboingStage == d_MixedStage) && locIsZIndependent)
				continue; //this combo has already been created (assuming it was valid): during the FCAL-only stage

			//conduct search
			if(std::binary_search(locUsedParticles_AllBut1.begin(), locUsedParticles_AllBut1.end(), locParticle))
				continue; //this shower has already been used, this combo won't work

			//See which RF bunches match up //guaranteed to be at least one, due to selection in Get_ParticlesForComboing() function
			//if charged or massive neutrals, ignore (they don't choose at this stage)
			vector<int> locValidRFBunches = (locPID != Gamma) ? locValidRFBunches_AllBut1 : dSourceComboTimeHandler->Get_CommonRFBunches(locValidRFBunches_AllBut1, locParticle, locVertexZBin);

			//no duplicates: this combo is unique.  build a new combo
			auto locComboParticles = locUsedParticlePairs_AllBut1;
			locComboParticles.emplace_back(locPID, locParticle);
			auto locCombo = dResourcePool_SourceCombo.Get_Resource();
			locCombo->Set_Members(locComboParticles, locFurtherDecays, locIsZIndependent); // create combo with all PIDs

			//save it! //in creation order!
			locSourceCombosByUseSoFar[locComboUseToCreate]->push_back(locCombo);
			Register_ValidRFBunches(locComboUseToCreate, locCombo, locValidRFBunches, locComboingStage, nullptr);
		}
	}
}

/***************************************************************** PARTICLE UTILITY FUNCTIONS *****************************************************************/

const vector<const JObject*>& DSourceComboer::Get_ParticlesForComboing(Particle_t locPID, ComboingStage_t locComboingStage, const vector<int>& locBeamBunches, signed char locVertexZBin)
{
	//find all particles that have an overlapping beam bunch with the input

	//SPECIAL CASES FOR NEUTRALS:
	//massive neutral: all showers
	//unknown RF: All showers
	//unknown vertex, known RF: from each zbin, all showers that were valid for that rf bunch (already setup)

	if(ParticleCharge(locPID) != 0) //charged tracks
		return dTracksByPID[locPID]; //rf bunch & vertex-z are irrelevant
	else if(locPID != Gamma) //massive neutrals
		return dShowersByBeamBunchByZBin[DSourceComboInfo::Get_VertexZIndex_Unknown()][{}]; //all neutrals: cannot do PID at all, and cannot do mass cuts until a specific vertex is chosen, so vertex-z doesn't matter

	if(locComboingStage == d_MixedStage_ZIndependent) //fcal
	{
		locVertexZBin = DSourceComboInfo::Get_VertexZIndex_ZIndependent();
		auto locGroupBunchIterator = dShowersByBeamBunchByZBin[locVertexZBin].find(locBeamBunches);
		if(locGroupBunchIterator != dShowersByBeamBunchByZBin[locVertexZBin].end())
			return locGroupBunchIterator->second;
		return Get_ShowersByBeamBunch(locBeamBunches, dShowersByBeamBunchByZBin[locVertexZBin]);
	}

	if(locBeamBunches.empty())
		return dShowersByBeamBunchByZBin[DSourceComboInfo::Get_VertexZIndex_Unknown()][{}]; //all showers, regardless of vertex-z

	auto locGroupBunchIterator = dShowersByBeamBunchByZBin[locVertexZBin].find(locBeamBunches);
	if(locGroupBunchIterator != dShowersByBeamBunchByZBin[locVertexZBin].end())
		return locGroupBunchIterator->second;
	return Get_ShowersByBeamBunch(locBeamBunches, dShowersByBeamBunchByZBin[locVertexZBin]);
}

const vector<const JObject*>& DSourceComboer::Get_ShowersByBeamBunch(const vector<int>& locBeamBunches, DPhotonShowersByBeamBunch& locShowersByBunch)
{
	//find all particles that have an overlapping beam bunch with the input
	//this won't happen often (max probably tens of times each event), so we can be a little inefficient
	vector<int> locBunchesSoFar = {*locBeamBunches.begin()};
	for(auto locBunchIterator = std::next(locBeamBunches.begin()); locBunchIterator != locBeamBunches.end(); ++locBunchIterator)
	{
		const auto& locComboShowers = locShowersByBunch[locBunchesSoFar];
		const auto& locBunchShowers = locShowersByBunch[vector<int>(*locBunchIterator)];
		locBunchesSoFar.push_back(*locBunchIterator);
		if(locBunchShowers.empty())
		{
			locShowersByBunch.emplace(locBunchesSoFar, locComboShowers);
			continue;
		}

		//merge and move-emplace
		vector<const JObject*> locMergeResult;
		locMergeResult.reserve(locComboShowers.size() + locBunchShowers.size());
		std::set_union(locComboShowers.begin(), locComboShowers.end(), locBunchShowers.begin(), locBunchShowers.end(), std::back_inserter(locMergeResult));
		locShowersByBunch.emplace(locBunchesSoFar, std::move(locMergeResult));
		Build_ParticleIterators(locBeamBunches, locShowersByBunch[locBeamBunches]);
	}
	return locShowersByBunch[locBeamBunches];
}

/******************************************************************* COMBO UTILITY FUNCTIONS ******************************************************************/

void DSourceComboer::Register_ValidRFBunches(const DSourceComboUse& locSourceComboUse, const DSourceCombo* locSourceCombo, const vector<int>& locRFBunches, ComboingStage_t locComboingStage, const DSourceCombo* locChargedCombo_WithNow)
{
	//THE INPUT locChargedCombo MUST BE:
	//Whatever charged combo you just comboed horizontally with to make this new, mixed combo

	//search and register
	auto locComboInfo = std::get<2>(locSourceComboUse);
	dValidRFBunches_ByCombo.emplace(locSourceCombo, locRFBunches);

	//also, register for each individual bunch: so that we can get valid combos for some input rf bunches later
	auto locVertexZBin = std::get<1>(locSourceComboUse);
	if(locComboingStage != d_ChargedStage)
	{
		auto& locSourceCombosByBeamBunchByUse = Get_SourceCombosByBeamBunchByUse(dComboInfoChargeContent[locComboInfo], locChargedCombo_WithNow);
		auto& locCombosByBeamBunch = locSourceCombosByBeamBunchByUse[locSourceComboUse];
		for(const auto& locBeamBunch : locRFBunches)
		{
			auto& locComboVector = locCombosByBeamBunch[{locBeamBunch}];
			locComboVector.push_back(locSourceCombo);
			dResumeSearchAfterIterators_Combos[std::make_pair(locSourceCombo, locVertexZBin)].emplace(vector<int>{locBeamBunch}, std::prev(locComboVector.end()));
		}
	}
	if(locRFBunches.empty()) //all //don't need to save the by-beam-bunch, but still need to save the resume-after iterator
	{
		auto& locComboVector = *(Get_CombosSoFar(locComboingStage, dComboInfoChargeContent[std::get<2>(locSourceComboUse)], locChargedCombo_WithNow)[locSourceComboUse]);
		dResumeSearchAfterIterators_Combos[std::make_pair(locSourceCombo, locVertexZBin)].emplace(locRFBunches, std::prev(locComboVector.end()));
	}
}

const vector<const DSourceCombo*>& DSourceComboer::Get_CombosForComboing(const DSourceComboUse& locComboUse, ComboingStage_t locComboingStage, const vector<int>& locBeamBunches, const DSourceCombo* locChargedCombo_WithPrevious)
{
	//THE INPUT locChargedCombo MUST BE:
	//Whatever charged combo you PREVIOUSLY comboed horizontally with to make the combos you're trying to get

	//find all combos for the given use that have an overlapping beam bunch with the input
	auto locChargeContent = dComboInfoChargeContent[std::get<2>(locComboUse)];
	if(locBeamBunches.empty() || (locChargeContent == d_Charged)) //e.g. fully charged, or a combo of 2 KLongs (RF bunches not saved for massive neutrals)
		return *((Get_CombosSoFar(locComboingStage, locChargeContent, locChargedCombo_WithPrevious))[locComboUse]);

	auto& locSourceCombosByBeamBunchByUse = Get_SourceCombosByBeamBunchByUse(locChargeContent, locChargedCombo_WithPrevious);
	auto locGroupBunchIterator = locSourceCombosByBeamBunchByUse[locComboUse].find(locBeamBunches);
	if(locGroupBunchIterator != locSourceCombosByBeamBunchByUse[locComboUse].end())
		return locGroupBunchIterator->second;
	return Get_CombosByBeamBunch(locSourceCombosByBeamBunchByUse[locComboUse], locBeamBunches, locComboingStage, std::get<1>(locComboUse));
}

const vector<const DSourceCombo*>& DSourceComboer::Get_CombosByBeamBunch(DCombosByBeamBunch& locCombosByBunch, const vector<int>& locBeamBunches, ComboingStage_t locComboingStage, signed char locVertexZBin)
{
	//find all combos for the given use that have an overlapping beam bunch with the input
	//this shouldn't be called very many times per event
	vector<int> locBunchesSoFar = {*locBeamBunches.begin()};
	for(auto locBunchIterator = std::next(locBeamBunches.begin()); locBunchIterator != locBeamBunches.end(); ++locBunchIterator)
	{
		const auto& locComboShowers = locCombosByBunch[locBunchesSoFar];
		const auto& locBunchShowers = locCombosByBunch[vector<int>(*locBunchIterator)];
		locBunchesSoFar.push_back(*locBunchIterator);
		if(locBunchShowers.empty())
		{
			locCombosByBunch.emplace(locBunchesSoFar, locComboShowers);
			continue;
		}

		//merge and move-emplace
		vector<const DSourceCombo*> locMergeResult;
		locMergeResult.reserve(locComboShowers.size() + locBunchShowers.size());
		std::set_union(locComboShowers.begin(), locComboShowers.end(), locBunchShowers.begin(), locBunchShowers.end(), std::back_inserter(locMergeResult));
		locCombosByBunch.emplace(locBunchesSoFar, std::move(locMergeResult));
		Build_ComboIterators(locBeamBunches, locCombosByBunch[locBeamBunches], locComboingStage, locVertexZBin);
	}
	return locCombosByBunch[locBeamBunches];
}

void DSourceComboer::Copy_ZIndependentMixedResults(const DSourceComboUse& locComboUseToCreate, const DSourceCombo* locChargedCombo_WithNow)
{
	//Copy the results from the FCAL-only stage through to the both stage (that way we don't have to repeat them)

	//THE INPUT locChargedCombo MUST BE:
	//Whatever charged combo you are about to combo horizontally with to make this new, mixed combo

	//Get combos so far
	auto locVertexZBin = std::get<1>(locComboUseToCreate);
	auto locComboInfo = std::get<2>(locComboUseToCreate);
	auto locChargeContent = dComboInfoChargeContent[locComboInfo];
	auto& locSourceCombosByUseSoFar = Get_CombosSoFar(d_MixedStage, locChargeContent, locChargedCombo_WithNow);

	//Get the combo vectors
	auto locComboUseFCAL = std::make_tuple(std::get<0>(locComboUseToCreate), DSourceComboInfo::Get_VertexZIndex_ZIndependent(), locComboInfo);
	const auto& locFCALComboVector = *(locSourceCombosByUseSoFar[locComboUseFCAL]);
	auto& locBothComboVector = *(locSourceCombosByUseSoFar[locComboUseToCreate]);

	//Copy over the combos
	locBothComboVector.reserve(locFCALComboVector.size() + dInitialComboVectorCapacity);
	locBothComboVector.assign(locFCALComboVector.begin(), locFCALComboVector.end());

	//Copy over the combos-by-beam-bunch
	auto& locSourceCombosByBeamBunchByUse = Get_SourceCombosByBeamBunchByUse(locChargeContent, locChargedCombo_WithNow);

	const auto& locCombosByBeamBunch = locSourceCombosByBeamBunchByUse[locComboUseFCAL];
	for(const auto& locComboBeamBunchPair : locCombosByBeamBunch)
	{
		if(locComboBeamBunchPair.first.size() == 1) //don't copy the overlap ones: they are not complete & need to be filled on the fly
			locSourceCombosByBeamBunchByUse[locComboUseToCreate].emplace(locComboBeamBunchPair);
	}

	//Copy over the resume-after iterators
	for(vector<const DSourceCombo*>::const_iterator locComboIterator = locBothComboVector.begin(); locComboIterator != locBothComboVector.end(); ++locComboIterator)
	{
		const auto& locRFBunches = dValidRFBunches_ByCombo[*locComboIterator];
		for(const auto& locBeamBunch : locRFBunches)
			dResumeSearchAfterIterators_Combos[std::make_pair(*locComboIterator, locVertexZBin)].emplace(vector<int>{locBeamBunch}, locComboIterator);
		if(locRFBunches.empty()) //all
			dResumeSearchAfterIterators_Combos[std::make_pair(*locComboIterator, locVertexZBin)].emplace(vector<int>{locRFBunches}, locComboIterator);
	}
}

const DSourceCombo* DSourceComboer::Get_StepSourceCombo(const DReaction* locReaction, size_t locDesiredStepIndex, const DSourceCombo* locSourceCombo_Current, size_t locCurrentStepIndex) const
{
	//Get the list of steps we need to traverse //particle pair: step index, particle instance index
	vector<pair<size_t, int>> locParticleIndices = {std::make_pair(locDesiredStepIndex, DReactionStep::Get_ParticleIndex_Initial())};
	while(locParticleIndices.back().first != locCurrentStepIndex)
	{
		auto locParticlePair = DAnalysis::Get_InitialParticleDecayFromIndices(locReaction, locParticleIndices.back().first);
		auto locStep = locReaction->Get_ReactionStep(locParticlePair.first);
		auto locInstanceIndex = DAnalysis::Get_ParticleInstanceIndex(locStep, locParticlePair.second);
		locParticleIndices.emplace_back(locParticlePair.first, locInstanceIndex);
	}

	//start from back of locParticleIndices, searching
	while(true)
	{
		auto locNextStep = locParticleIndices[locParticleIndices.size() - 2].first;
		auto locInstanceToFind = locParticleIndices.back().second;
		const auto& locUseToFind = dSourceComboUseReactionStepMap.find(locReaction)->second.find(locNextStep)->second;
		locSourceCombo_Current = DAnalysis::Find_Combo_AtThisStep(locSourceCombo_Current, locUseToFind, locInstanceToFind);
		if(locSourceCombo_Current == nullptr)
			return nullptr; //e.g. entirely neutral step when input is charged
		if(locNextStep == locDesiredStepIndex)
			return locSourceCombo_Current;
		locParticleIndices.pop_back();
	}

	return nullptr;
}

const DSourceCombo* DSourceComboer::Get_Presiding_ChargedCombo(const DSourceCombo* locChargedCombo_Presiding, const DSourceComboUse& locNextComboUse, ComboingStage_t locComboingStage, size_t locInstance) const
{
	//locInstance starts from ONE!!
	if(locComboingStage == d_ChargedStage)
		return nullptr;
	if(locChargedCombo_Presiding == nullptr)
		return nullptr;
	if(Get_ChargeContent(std::get<2>(locNextComboUse)) != d_AllCharges)
		return nullptr; //not needed

	auto locFurtherDecayCombos = locChargedCombo_Presiding->Get_FurtherDecayCombos();

	auto locUseToFind = (locComboingStage == d_MixedStage_ZIndependent) ? locNextComboUse : dZDependentUseToIndependentMap.find(locNextComboUse)->second;
	auto locUseIterator = locFurtherDecayCombos.find(locUseToFind);

	//check if the use you are looking for is a temporary (e.g. vertical grouping of 2KShorts when comboing horizontally)
	if(locUseIterator == locFurtherDecayCombos.end())
		return locChargedCombo_Presiding; //temporary: the presiding is still the same!

	//get the vector of potential charged combos
	auto locNextChargedComboVector = locUseIterator->second;

	//if on z-independent, don't need to do anything fancy, just return the requested instance
	if(locComboingStage == d_MixedStage_ZIndependent)
		return locNextChargedComboVector[locInstance - 1];

	//there might be multiple combos (e.g. K0 decays), each at a different vertex-z
	//so, we must retrieve the N'th charged combo with the correct vertex-z bin
	size_t locCount = 0;
	auto locDesiredVertexZBin = std::get<1>(locNextComboUse);
	for(const auto& locNextPotentialCombo : locNextChargedComboVector)
	{
		auto locNextVertexZBin = dSourceComboVertexer->Get_VertexZBin(false, locNextPotentialCombo, nullptr);
		if(locNextVertexZBin != locDesiredVertexZBin)
			continue;
		if(++locCount == locInstance)
			return locNextPotentialCombo;
	}

	return nullptr; //uh oh ...
}

const DSourceCombo* DSourceComboer::Get_VertexPrimaryCombo(const DSourceCombo* locReactionCombo, const DReactionStepVertexInfo* locStepVertexInfo)
{
	//if it's the production vertex, just return the input
	if(locStepVertexInfo->Get_ProductionVertexFlag())
		return locReactionCombo;

	//see if it's already been determined before: if so, just return it
	auto locCreationPair = std::make_pair(locReactionCombo, locStepVertexInfo);
	auto locIterator = dVertexPrimaryComboMap.find(locCreationPair);
	if(locIterator != dVertexPrimaryComboMap.end())
		return locIterator->second;

	//find it
	auto locReaction = locStepVertexInfo->Get_Reaction();
	auto locDesiredStepIndex = locStepVertexInfo->Get_StepIndices().front();
	auto locVertexPrimaryCombo = Get_StepSourceCombo(locReaction, locDesiredStepIndex, locReactionCombo, 0);

	//save it and return it
	dVertexPrimaryComboMap.emplace(locCreationPair, locVertexPrimaryCombo);
	return locVertexPrimaryCombo;
}

const DSourceCombo* DSourceComboer::Get_VertexPrimaryCombo(const DSourceCombo* locReactionCombo, const DReactionStepVertexInfo* locStepVertexInfo) const
{
	//if it's the production vertex, just return the input
	if(locStepVertexInfo->Get_ProductionVertexFlag())
		return locReactionCombo;

	//see if it's already been determined before: if so, just return it
	auto locCreationPair = std::make_pair(locReactionCombo, locStepVertexInfo);
	auto locIterator = dVertexPrimaryComboMap.find(locCreationPair);
	if(locIterator != dVertexPrimaryComboMap.end())
		return locIterator->second;

	//find it
	auto locReaction = locStepVertexInfo->Get_Reaction();
	auto locDesiredStepIndex = locStepVertexInfo->Get_StepIndices().front();
	auto locVertexPrimaryCombo = Get_StepSourceCombo(locReaction, locDesiredStepIndex, locReactionCombo, 0);

	//return it
	return locVertexPrimaryCombo;
}

bool DSourceComboer::Get_PromoteFlag(Particle_t locDecayPID_UseToCheck, const DSourceComboInfo* locComboInfo_UseToCreate, const DSourceComboInfo* locComboInfo_UseToCheck) const
{
	if(locDecayPID_UseToCheck != Unknown)
		return false;

	auto locFurtherDecayInfo_UseToAdd = locComboInfo_UseToCheck->Get_FurtherDecays();
	if(!locFurtherDecayInfo_UseToAdd.empty())
	{
		auto locFurtherDecayInfo_UseToCreate = locComboInfo_UseToCreate->Get_FurtherDecays();
		return std::binary_search(locFurtherDecayInfo_UseToCreate.begin(), locFurtherDecayInfo_UseToCreate.end(), locFurtherDecayInfo_UseToAdd.front());
	}
	else
	{
		auto locNumParticles_ToAdd = locComboInfo_UseToCheck->Get_NumParticles();
		auto locNumParticles_UseToCreate = locComboInfo_UseToCreate->Get_NumParticles();
		return std::binary_search(locNumParticles_UseToCreate.begin(), locNumParticles_UseToCreate.end(), locNumParticles_ToAdd.front());
	}
}

} //end DAnalysis namespace
