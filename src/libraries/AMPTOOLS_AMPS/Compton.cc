
#include <cassert>
#include <iostream>
#include <string>
#include <sstream>
#include <cstdlib>

#include "TLorentzVector.h"
#include "TLorentzRotation.h"
#include "TFile.h"

#include "IUAmpTools/Kinematics.h"
#include "AMPTOOLS_AMPS/Compton.h"

Compton::Compton( const vector< string >& args ) :
UserAmplitude< Compton >( args )
{
	assert( args.size() == 1 ||  args.size() == 3 || args.size() == 5 );

	// three ways to pass on polarization information
	// (adapted from Zlm.cc)
	
	if( args.size() == 1 ) {
		// 1. polarization information must be included in beam photon four vector
		//    Usage: amplitude <reaction>::<sum>::<ampName>

		polInTree = true;
	} else if( args.size() == 3 ) {
		// 2. polarization fixed per amplitude and passed as flag
		//    Usage: amplitude <reaction>::<sum>::<ampName> <polAngle> <polFraction>
		polInTree = false;
		polAngle = atof( args[1].c_str() );
		polFraction = atof( args[2].c_str() );
	} else {
		// 2. polarization fixed per amplitude and passed as flag
		//    Usage: amplitude <reaction>::<sum>::<ampName> <polAngle> <polFraction=0.> <rootFile> <hist>
		polInTree = false;
		polAngle = atof( args[1].c_str() );	
		polFraction = 0.;
		TFile* f = new TFile( args[3].c_str() );
        polFrac_vs_E = (TH1D*)f->Get( args[4].c_str() );
        assert( polFrac_vs_E != NULL );
	}
	
	// do some sanity checks?
}


complex< GDouble >
Compton::calcAmplitude( GDouble** pKin ) const {
  
	TLorentzVector target  ( 0., 0., 0., 0.938);

	TLorentzVector beam;
    TVector3 eps;
   	if(polInTree) {
    	beam.SetPxPyPzE( 0., 0., pKin[0][0], pKin[0][0]);
    	eps.SetXYZ(pKin[0][1], pKin[0][2], 0.); // makes default output gen_amp trees readable as well (without transforming)
   	} else {
    	beam.SetPxPyPzE( pKin[0][1], pKin[0][2], pKin[0][3], pKin[0][0] ); 
    	eps.SetXYZ(cos(polAngle*TMath::DegToRad()), sin(polAngle*TMath::DegToRad()), 0.0); // beam polarization vector
   	}

	TLorentzVector recoil ( pKin[1][1], pKin[1][2], pKin[1][3], pKin[1][0] ); 
	TLorentzVector p1     ( pKin[2][1], pKin[2][2], pKin[2][3], pKin[2][0] ); 
	
	TLorentzVector cm = recoil + p1;
	TLorentzRotation cmBoost( -cm.BoostVector() );
	
	TLorentzVector beam_cm = cmBoost * beam;
	TLorentzVector target_cm = cmBoost * target;
	TLorentzVector recoil_cm = cmBoost * recoil;
	
	// phi dependence needed for polarized distribution
	TLorentzVector p1_cm = cmBoost * p1;
	GDouble phi = p1_cm.Phi() + polAngle*TMath::Pi()/180.;
	GDouble cos2Phi = cos(2.*phi);
	
	// get beam polarization
	GDouble Pgamma;
	if(polInTree) {
		Pgamma = eps.Mag();
	} else {
		if(polFraction > 0.) { // for fitting with constant polarization 
			Pgamma = polFraction;
		} else{
			int bin = polFrac_vs_E->GetXaxis()->FindBin(pKin[0][0]);
			if (bin == 0 || bin > polFrac_vs_E->GetXaxis()->GetNbins()){
				Pgamma = 0.;
			} else 
				Pgamma = polFrac_vs_E->GetBinContent(bin);
		}
	}

	// factors needed to calculate cross section from model
	GDouble s = cm.M2();
	GDouble t = (recoil - target).M2();
	if(fabs(t) < 0.4) return 0.; // interested in high -t behavior, so temporarily exclude low -t 

	// model parameters from PAC42 proposal PR12-14-003
	GDouble s_0 = 10.92;
	GDouble t_0 = 2.61;
	GDouble W = 0.0702 * pow(s_0/s, 2) * pow(t_0/t, 4);

	// include polarization effects later
	GDouble BeamSigma = 0.1;	
	W *= (1 - Pgamma * BeamSigma * cos2Phi);

	return complex< GDouble > ( sqrt( fabs(W) ) );
}

