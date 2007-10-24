#ifndef _DKINEMATICDATA_
#define _DKINEMATICDATA_

/*
 *  DKinematicData.h
 *  Hall D
 *
 *  Created by Matthew Shepherd on 5/28/07.
 
 Class largely borrowed from the KTKinematicData class used in CLEO.
 Where applicable CLHEP objects have been converted int Hall D
 ROOT-based typdefs.
 
 A portion of the original CLEO documenation appears below:
 
 Usage:
 
 Describes kinematic properties of charged tracks, photons, and
 virtual particles such as pi0, Ks,  so that a user
 can carry out standard operations such as calculating masses, adding
 4-momenta together, etc. The basic information consists of the
 3-momentum, 3-position, mass and charge. A 7x7 error matrix is stored
 for the quantities \c (Px,Py,Pz,E,x,y,z).
  
 \par Building a simple DKinematicData object
 In addition to the usual copy constructor, DKinematicData objects can
 be built from basic momentum and position information, e.g.,
 \code
 DVector3 momentum(1.2, -0.5, 0.6);
 DVector3  position(0.002, 0.003, 0.);
 double mass = 0.1396;
 double charge = -1.0;
 DMatrixDSym errMatrix(7,1);  // Create a 7x7 unit matrix for now
 DKinematicData pion(momentum, position, mass, charge, errorMatrix);
 \endcode
 The error matrix argument is optional. If absent, a null error matrix
 is stored.
 <br><br>
 Most of the time you build a kinematic object from a helix read
 from the data access system. In that case you supply the helix
 parameters, magnetic field (to compute the momentum) and mass (needed
                                                                for the energy). The kinematic parameters are evaluated at the point
 of closest approach to the reference point.
 <br><br>
 Note that the magnetic field is always specified in \e kilogauss.
 \code
 KTHelix helix;
 double mass = 0.1396;
 DVector3 bField(0., 0., -15.);
 DKinematicData pion(helix, mass, bField);
 \endcode
 By default, the helix error matrix is converted to the appropriate
 7x7 form and added to the object. You can prevent the error matrix
 from being formed by replacing the above declaration by
 \code
 bool noErrorMatrix = false;
 KTKinematic pion(helix, mass, bField, noErrorMatrix);
 \endcode
 
 \par Setting and retrieving information from DKinematicData objects.
 All kinds of information can be set or retrieved.
 \code
 DKinematicData pion(helix, mass, bField);            // Make a pion
 \endcode
 Modify some of the pion components
 \code
 pion.setMomentum(DVector3(0.2,0.5,-1.2));          // 3-momentum
 pion.setPosition(DVector3(0.002,0.005,0.02));       // Position
 pion.setMass(0.4937);                                 // Mass
 pion.clearErrorMatrix();                              // Clear err matrix
 \endcode
 Retrieve pion information
 \code
 DVector3 momentum = pion.momentum();               // 3-momentum
 DLorentzVector fourMomentum = pion.lorenzMomentum(); // 4-momentum
 DVector3  position = pion.position();               // Position
 double mass = pion.mass();                            // Mass
 double charge = pion.charge();                        // Charge
 DMatrixDSym errorMatrix = pion.errorMatrix();        // Error matrix
 double ptot = pion.pmag();                            // p
 double ptotsq = pion.pmag2();                         // p^2
 double pt = pion.pperp();                             // pt
 double ptsq = pion.pperp2();                          // pt^2
 double px = pion.px();                                // px
 double py = pion.py();                                // py
 double pz = pion.pz();                                // pz
 double E  = pion.energy();                            // E
 double x = pion.x();                                  // x
 double y = pion.y();                                  // y
 double z = pion.z();                                  // z
 \endcode
 
 \par Fixed or floating mass
 Particles like pions, kaons and gammas have predetermined, fixed masses
 while those calculated from invariant masses or mass fits, such as
 D0's and B's, have masses which "float" because the energy is
 independent of the momentum. DKinematicData objects have a flag
 that specifies whether or not the mass is floating or not. The flag
 can be accessed as follows
 \code
 DKinematicData pion(...);
 bool massFixed = pion.hasFixedMass();  // Get the fixed mass flag
 pion.setMassFixed();                        // Set fixed mass
 pion.setMassFloat();                        // Set floating mass
 \endcode
 One should rarely need to set the mass flag since the defaults used by
 the tracking system are expected to be adequate. For example, when
 building a DKinematicData object from a helix or from the basic
 3-momentum, 3-position, etc., the flag is set to "true". As kinematic
 fitting becomes available, particles built by combining the 4-momentum
 of daughter particles will have the flag set to "false" because the
 energy will be truly independent of the 3-momentum.
 
 */

#include "JANA/JObject.h"

#include "DRandom.h"    

#include "DVector3.h"    
#include "DLorentzVector.h" 
#include "DMatrixDSym.h" 

class DKinematicData : public JObject
{
    
public:
    
	//virtual const char* className(void){return "DKinematicData"; }
//	virtual const char* className(void){return static_className();}
//	static const char* static_className(void){return "DKinematicData";}

    // constants, enums and typedefs
    typedef double ValueType;
    
    enum { kDefaultMass = 0 ,
           kDefaultCharge = 0
    } ;
    
    ///This is needed to associate the elements correctly in the error matrix
    enum ParameterOrder {kPx = 1,
        kPy,
        kPz,
        kEnergy,
        kX,
        kY,
        kZ};
    
    // constructors and destructor

    DKinematicData( void ) ;
    
    DKinematicData( const oid_t id );

    DKinematicData( const DKinematicData& aKinematicData );
    
    DKinematicData( const DKinematicData& aKinematicData,
                    const bool aCopyErrorMatrix ) ;
 
    DKinematicData( const DVector3& aMomentum ,
                    const DVector3& aPosition,
                    const ValueType aMass,
                    const ValueType aCharge) ;
    
    DKinematicData( const DVector3& aMomentum ,
                    const DVector3& aPosition,
                    const ValueType aMass,
                    const ValueType aCharge,
                    const DMatrixDSym& aErrorMatrix ) ;
    
    // this class needs additional constructors to construct
    // from kinematic data from tracking output
    
    virtual ~DKinematicData( void ) ;
    
    // assignment operator(s)
    const DKinematicData& operator=( const DKinematicData& aOtherKinematicData ) ;
    
    bool operator==( const DKinematicData& rhs ) const ;
    bool operator!=( const DKinematicData& rhs ) const ;
    
    // member functions
    void setMass( const ValueType aMass ) ;
    void setMomentum( const DVector3& aMomentum ) ;
    void setPosition( const DVector3& aPosition ) ;
    void setCharge( const ValueType aCharge);
    void setMassFixed( void ) ;
    void setMassFloat( void ) ;
    void clearErrorMatrix( void ) ;
    void setErrorMatrix( const DMatrixDSym& aMatrix ) ;
    
    // For debugging with MCThrown
    void smearMCThrownMomentum( double smearPct );

    // const member functions
    ValueType mass( void ) const ;
    ValueType charge( void ) const ;
    ValueType px( void ) const ;
    ValueType py( void ) const ;
    ValueType pz( void ) const ;
    ValueType energy( void ) const ;
    ValueType x( void ) const ;
    ValueType y( void ) const ;
    ValueType z( void ) const ;
    ValueType pperp( void ) const ;
    ValueType pperp2( void ) const ;
    ValueType pmag( void ) const ;
    ValueType pmag2( void ) const ;
    const DVector3& momentum( void ) const ;
    const DVector3& position( void ) const ;
    const DLorentzVector lorentzMomentum( void ) const ;
    bool hasFixedMass( void ) const ;
    virtual const DMatrixDSym& errorMatrix( void ) const ;
    
    /// \return TRUE if errors are all zero
    bool hasNullErrorMatrix() const {
        return (&errorMatrix() == nullMatrix());};
    
protected:
        
    // protected member functions
        
    // These routines are used to optimize the performance of some
    // of the move routines
    DMatrixDSym* takeOwnershipOfPointer( void );
    void         restoreOwnershipOfPointer( DMatrixDSym* const aPointer);
    
    // left for reference -- KTHelix class not ported to GlueX
    // void calculate7x7ErrorMatrixFrom5x5ErrorMatrix( const KTHelix& aHelix);
    
private:
        
    // data members
    bool m_hasFixedMass ;
    ValueType m_mass ;
    ValueType m_charge ;
    DVector3 m_momentum ;
    DVector3 m_position ;
    DMatrixDSym* m_errorMatrix ;   // Order is (px, py, pz, E, x, y, z)
    
    //All matricies without a set error matrix can share the same nullMatrix
    static DMatrixDSym* nullMatrix();
};

// inline function definitions

#endif /* _DKINEMATICDATA_ */

