// $Id$
//
//    File: DFactory_DTOFHit.cc
// Created: Thu Jun  9 10:05:21 EDT 2005
// Creator: davidl (on Darwin wire129.jlab.org 7.8.0 powerpc)
//

#include "DFactory_DTOFHit.h"

//------------------
// evnt
//------------------
derror_t DFactory_DTOFHit::evnt(DEventLoop *eventLoop, int eventnumber)
{
	/// Place holder for now. 

	return NOERROR;
}

//------------------
// Extract_HDDM
//------------------
derror_t DFactory_DTOFHit::Extract_HDDM(s_HDDM_t *hddm_s, vector<void*> &v)
{
	/// Copies the data from the given hddm_s structure. This is called
	/// from DEventSourceHDDM::GetObjects.
	
	v.clear();

	// Loop over Physics Events
	s_PhysicsEvents_t* PE = hddm_s->physicsEvents;
	if(!PE) return NOERROR;
	
	for(unsigned int i=0; i<PE->mult; i++){
		s_Vcounters_t *vcounters = NULL;
		s_Hcounters_t *hcounters = NULL;
		if(PE->in[i].hitView){
			if(PE->in[i].hitView->forwardTOF){
				vcounters = PE->in[i].hitView->forwardTOF->vcounters;
				hcounters = PE->in[i].hitView->forwardTOF->hcounters;
			}
		}
		
		if(vcounters){
			for(unsigned int j=0;j<vcounters->mult;j++){
				float x = vcounters->in[j].x;
				float y = 0.0;
				s_Top_t *top = vcounters->in[j].top;
				if(top){
					s_Hits_t *hits = top->hits;
					for(unsigned int k=0;k<hits->mult;k++){
						float dE = hits->in[k].dE;
						float t = hits->in[k].t;
					
						DTOFHit *tofhit = new DTOFHit;
						tofhit->x = x;
						tofhit->y = y;
						tofhit->orientation = 0;
						tofhit->end = 0;
						tofhit->dE = dE;
						tofhit->t = t;
						v.push_back(tofhit);
					}
				}
				s_Bottom_t *bottom = vcounters->in[j].bottom;
				if(bottom){
					s_Hits_t *hits = bottom->hits;
					for(unsigned int k=0;k<hits->mult;k++){
						float dE = hits->in[k].dE;
						float t = hits->in[k].t;
					
						DTOFHit *tofhit = new DTOFHit;
						tofhit->x = x;
						tofhit->y = y;
						tofhit->orientation = 0;
						tofhit->end = 1;
						tofhit->dE = dE;
						tofhit->t = t;
						v.push_back(tofhit);
					}
				}
			}
		}

		if(hcounters){
			for(unsigned int j=0;j<hcounters->mult;j++){
				float y = hcounters->in[j].y;
				float x = 0.0;
				s_Left_t *left = hcounters->in[j].left;
				if(left){
					s_Hits_t *hits = left->hits;
					for(unsigned int k=0;k<hits->mult;k++){
						float dE = hits->in[k].dE;
						float t = hits->in[k].t;
					
						DTOFHit *tofhit = new DTOFHit;
						tofhit->x = x;
						tofhit->y = y;
						tofhit->orientation = 1;
						tofhit->end = 0;
						tofhit->dE = dE;
						tofhit->t = t;
						v.push_back(tofhit);
					}
				}
				s_Right_t *right = hcounters->in[j].right;
				if(right){
					s_Hits_t *hits = right->hits;
					for(unsigned int k=0;k<hits->mult;k++){
						float dE = hits->in[k].dE;
						float t = hits->in[k].t;
					
						DTOFHit *tofhit = new DTOFHit;
						tofhit->x = x;
						tofhit->y = y;
						tofhit->orientation = 1;
						tofhit->end = 1;
						tofhit->dE = dE;
						tofhit->t = t;
						v.push_back(tofhit);
					}
				}
			}
		}
	}

	return NOERROR;
}

//------------------
// toString
//------------------
const string DFactory_DTOFHit::toString(void)
{
	// Ensure our Get method has been called so _data is up to date
	Get();
	if(_data.size()<=0)return string(); // don't print anything if we have no data!

	printheader("row:   x(cm):   y(cm):  orientation:     end:     dE(MeV):   t(ns):");
	
	for(unsigned int i=0; i<_data.size(); i++){
		DTOFHit *tofhit = _data[i];

		printnewrow();
		
		printcol("%d", i);
		printcol("%3.1f", tofhit->x);
		printcol("%3.1f", tofhit->y);
		printcol(tofhit->orientation ? "horizontal":"vertical");
		printcol(tofhit->end ? "right":"left");
		printcol("%2.3fs", tofhit->dE*1000.0);
		printcol("%4.3fs", tofhit->t);
		
		printrow();
	}
	
	return _table;
}
