// $Id$
//
//    File: DEventTag.h
// Created: Fri Dec  4 10:14:22 EST 2015
// Creator: davidl (on Darwin harriet.jlab.org 13.4.0 i386)
//

#ifndef _DEventTag_
#define _DEventTag_

#include <JANA/jerror.h>

#include <TRIGGER/DL3Trigger.h>

class DEventTag:public jana::JObject{
	public:
		JOBJECT_PUBLIC(DL3Trigger);

		DEventTag(uint64_t es=0L, DL3Trigger::L3_decision_t d=DL3Trigger::kNO_DECISION, uint64_t l3s=0, uint32_t l3a=0)
			:event_status(es),L3_decision(d),L3_status(l3s),L3_algorithm(l3a){}
		
		uint64_t event_status;                   ///< JANA event status word when event was written
		DL3Trigger::L3_decision_t L3_decision;   ///< L3 decision when event was written
		uint64_t L3_status;                      ///< L3 status word when event was written
		uint32_t L3_algorithm;                   ///< L3 algorithm identifier when event was written
		
		// This method is used primarily for pretty printing
		// the second argument to AddString is printf style format
		void toStrings(vector<pair<string,string> > &items)const{
			AddString(items, "event_status", "%d"     , event_status);
			AddString(items, "L3_decision" , "%d"     , L3_decision);
			AddString(items, "L3_status"   , "0x%16x" , L3_status);
			AddString(items, "L3_algorithm", "0x%8x"  , L3_algorithm);
		}

};

#endif // _DEventTag_

