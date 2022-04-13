#if !defined(ASCIIDATAWRITER)
#define ASCIIDATAWRITER

#include <vector>
#include "IUAmpTools/Kinematics.h"


/**                                                                                                                                 
 * This class writes events passed in the Kinematics data type (see AmpTools)
 * to disk in the genr8 ASCII format. This is a quick-and-dirty solution
 * for preparing events generated by AmpTools-based event generators
 * for simulation with HDGeant. This should be replaced at some 
 * point by an "HDDMDataWriter"
 */

class ASCIIDataWriter
{

public:
  
  ASCIIDataWriter( const string& outFile );
  ~ASCIIDataWriter();
  
  void writeEvent( const Kinematics& kin, vector<int> &types);
  
  int eventCounter() const { return m_eventCounter; }
  
private:
  
  FILE* fid;
  int m_eventCounter;
  
  int m_nPart;

};

#endif
