// $Id: DTOFMCHit_factory.cc 2710 2007-06-22 15:09:48Z zihlmann $
//
//    File: DTOFMCHit_factory.cc
// Created: Mon Jul  9 16:34:24 EDT 2007
// Creator: B. Zihlmann
//
#include "DTOFMCHit_factory.h"
#include "DTOFMCResponse.h"

#include <math.h>

//------------------
// evnt
//------------------
jerror_t DTOFMCHit_factory::evnt(JEventLoop *eventLoop, int eventnumber)
{

  vector<const DTOFMCResponse*> mcresponses;
  eventLoop->Get(mcresponses);

  for (unsigned int i = 0; i < mcresponses.size(); i++){

    const DTOFMCResponse *mcresponse = mcresponses[i];
    DTOFMCHit *hit = new DTOFMCHit;

    // calculate meantime and time difference of MC data
    if (mcresponse->TDC_north>0 && mcresponse->TDC_south>0){
      hit->id          = mcresponse->id;
      hit->orientation = mcresponse->orientation;

      float tn =  float(mcresponse->TDC_north)*TDC_RES_MC;
      float ts =  float(mcresponse->TDC_south)*TDC_RES_MC;
      float en =  float(mcresponse->ADC_north);
      float es =  float(mcresponse->ADC_south);
      // mean time
      float tm = (tn+ts)/2.;
      // time difference south-north so positive values are hits closer to north
      float td = (ts-tn);

      // position 
      float pos = C_EFFECTIVE*td/2.;
      
      hit->meantime  = tm;
      hit->timediff = td;
      hit->pos      = pos;
      hit->dpos     = TOF_POS_RES; // see above only true if hit seen o both sides

      // mean energy deposition weight by arrival time at PMT (favor closer PMT)
      // devide by two to be comparable with single PMT hits
      en *= exp((HALFPADDLE-pos)/ATTEN_LENGTH) ;
      es *= exp((HALFPADDLE+pos)/ATTEN_LENGTH) ;

      float emean = (en+es)/2.; 
                                                    
      if (emean>2048) emean = 2048; // overflow
      emean = emean*TOF_ADC_TO_E;
      hit->dE = emean;

    } else {
      hit->id          = mcresponse->id;
      hit->orientation = mcresponse->orientation;
      hit->meantime = -999.;
      hit->timediff = -999.;
      hit->pos      = -999.;
      hit->dpos     = -999.;
      hit->dE       = -999.;	
    }


    _data.push_back(hit);

  }

  return NOERROR;
}


//------------------
// toString
//------------------
const string DTOFMCHit_factory::toString(void)
{
  // Ensure our Get method has been called so _data is up to date
  Get();
  if(_data.size()<=0)return string(); // don't print anything if we have no data!

  printheader( "id: orientation: pos[cm]:  epos[cm]:  dE [MeV]: meantime [ns]: timediff [ns]:" );

	
  for(unsigned int i=0; i<_data.size(); i++){
    DTOFMCHit *tofhit = _data[i];

    printnewrow();
    printcol("%d",	tofhit->id );
    printcol("%d",	tofhit->orientation );
    printcol("%2.3f",	tofhit->pos );
    printcol("%2.3f",	tofhit->dpos );
    printcol("%1.3f",	tofhit->dE );
    printcol("%1.3f",	tofhit->meantime );    
    printcol("%1.3f",	tofhit->timediff );

    printrow();
  }
  
	
  return _table;
}

//------------------
// brun
//------------------
jerror_t DTOFMCHit_factory::brun(JEventLoop *loop, int eventnumber)
{
  map<string, double> tofparms;
  
  if ( !loop->GetCalib("TOF/tof_parms", tofparms)){
    cout<<"DTOFMCHit_factory: loading values from TOF data base"<<endl;
  } else {
    cout << "DTOFMCHit_factory: Error loading values from TOF data base" <<endl;
  }
  
  ATTEN_LENGTH   =    tofparms["TOF_ATTEN_LENGTH"]; 
  C_EFFECTIVE    =    tofparms["TOF_C_EFFECTIVE"];
  TDC_RES_MC     =    tofparms["TOF_TDC_RES_MC"];
  HALFPADDLE     =    tofparms["TOF_HALFPADDLE"];
  TOF_POS_RES    =    tofparms["TOF_POS_RES"];
  TOF_ADC_TO_E   =    tofparms["TOF_ADC_TO_E"];
  
  return NOERROR;
  
}
