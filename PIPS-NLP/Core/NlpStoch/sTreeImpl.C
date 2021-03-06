/* PIPS-IPM                                                           *
 * Author:  Cosmin G. Petra                                           *
 * (C) 2012 Argonne National Laboratory. See Copyright Notification.  */

#include "sTreeImpl.h"

#include "StochVector.h"
#include "StochGenMatrix.h"
#include "StochSymMatrix.h"
using namespace std;

sTreeImpl::sTreeImpl( stochasticInput &in_, MPI_Comm comm /*=MPI_COMM_WORLD*/)
  : sTree(), m_id(0), in(in_), parent(NULL)
{
  if(-1==rankMe) MPI_Comm_rank(comm, &rankMe);
  if(-1==numProcs) MPI_Comm_size(comm, &numProcs);

  m_nx = in.nFirstStageVars();
  m_my = compute_nFirstStageEq();
  m_mz = in.nFirstStageCons() - m_my;
  m_mle = in.nLinkECons();
  m_mli = in.nLinkICons();
  for (int scen=0; scen<in.nScenarios(); scen++) {
    sTreeImpl* c = new sTreeImpl(scen+1,in);
    c->parent = this;
    children.push_back( c );
  }
  if(0==rankMe) cout << "[PIPS] - " << in.nScenarios() << " scenarios on " << numProcs << " MPI ranks" << endl;
  
}

sTreeImpl::sTreeImpl(int id, stochasticInput &in_)
  : sTree(), m_id(id), in(in_)
{ 
  m_nx=0; m_my=0; m_mz=0;
  //m_nx = in.nSecondStageVars(id-1);
  //m_my = compute_nSecondStageEq(id-1);
  //m_mz = in.nSecondStageCons(id-1) - m_my;
  m_mle = in.nLinkECons();
  m_mli = in.nLinkICons();
  // add children only if you have multi-stages
}
  
sTreeImpl::~sTreeImpl()
{ 
  // parent deallocates children
  //for(size_t it=0; it<children.size(); it++)
  //  delete children[it];
}

//to be called after assignProcesses
void sTreeImpl::loadLocalSizes()
{
  if(m_id>0) //root sizes alread loaded
    if(commWrkrs!=MPI_COMM_NULL) {
	  // we need to compute number of nSecondStageVars and nSecondStageCons, so firstlly we call m_my =... 
      m_my = compute_nSecondStageEq(m_id-1);		
      m_nx = in.nSecondStageVars(m_id-1);
      m_mz = in.nSecondStageCons(m_id-1) - m_my;
    }
  for(size_t it=0; it<children.size(); it++)
    children[it]->loadLocalSizes();
}
StochSymMatrix* sTreeImpl::createQ() const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochSymDummyMatrix(m_id);

  StochSymMatrix* Q = NULL;

  if (m_id==0) {
    // get the Hessian from stochasticInput and get it in row-major
    // format by transposing it
    CoinPackedMatrix Q0;
    Q0.reverseOrderedCopyOf( in.getFirstStageHessian() );

    Q = new StochSymMatrix(m_id, N, m_nx, Q0.getNumElements(), commWrkrs);

    memcpy( Q->diag->krowM(), 
	    Q0.getVectorStarts(),
	    (Q0.getNumRows()+1)*sizeof(int));
    memcpy( Q->diag->jcolM(),
	    Q0.getIndices(),
	    Q0.getNumElements()*sizeof(int));
    memcpy( Q->diag->M(),
	    Q0.getElements(),
	    Q0.getNumElements()*sizeof(double));
  } else {
    CoinPackedMatrix Qi, Ri;
    Qi.reverseOrderedCopyOf( in.getSecondStageHessian(m_id-1) );
    Ri.reverseOrderedCopyOf( in.getSecondStageCrossHessian(m_id-1) );
			     
    Q = new 
      StochSymMatrix( m_id,N, 
		      m_nx, Qi.getNumElements(), //size and nnz of the diag block
		      parent->m_nx, Ri.getNumElements(), //num of cols and nnz of the border
		      commWrkrs);

    memcpy( Q->diag->krowM(), 
	    Qi.getVectorStarts(),
	    (Qi.getNumRows()+1)*sizeof(int));
    memcpy( Q->diag->jcolM(),
	    Qi.getIndices(),
	    Qi.getNumElements()*sizeof(int));
    memcpy( Q->diag->M(),
	    Qi.getElements(),
	    Qi.getNumElements()*sizeof(double));

    memcpy( Q->border->krowM(), 
	    Ri.getVectorStarts(),
	    (Qi.getNumRows()+1)*sizeof(int));
    memcpy( Q->border->jcolM(),
	    Ri.getIndices(),
	    Ri.getNumElements()*sizeof(int));
    memcpy( Q->border->M(),
	    Ri.getElements(),
	    Ri.getNumElements()*sizeof(double));
  }
    for(size_t it=0; it<children.size(); it++) {
      StochSymMatrix* child = children[it]->createQ();
      Q->AddChild(child);
    }
    return Q;
  return NULL;
}

//#define RESCALE 200

static double RESCALE=1.0;
StochVector* sTreeImpl::createc() const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* svec = new StochVector(m_nx, commWrkrs);
  double* vec = ((SimpleVector*)svec->vec)->elements();  

  if(m_id==0) {
    //RESCALE=0.1*children.size();
#ifdef TIMING
      //RESCALE=1;//0.25*children.size();
#endif
    vector<double> c = in.getFirstStageObj();
    copy(c.begin(), c.end(), vec);

    for(size_t i=0; i<m_nx; i++)
      vec[i] = vec[i]*RESCALE;
  }  else {
    vector<double> c = in.getSecondStageObj(m_id-1);
    copy(c.begin(), c.end(), vec);

    for(size_t i=0; i<m_nx; i++)
      vec[i] = vec[i]*RESCALE;
  }

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createc();
    svec->AddChild(child);
  }
  return svec;
}

StochVector* sTreeImpl::createb() const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* svec = new StochVector(m_my, commWrkrs);
  double* vec = ((SimpleVector*)svec->vec)->elements();  

  vector<double> lb, ub;

  if(m_id==0) {
    lb = in.getFirstStageRowLB();
    ub = in.getFirstStageRowUB();
  }  else {
    lb = in.getSecondStageRowLB(m_id-1);
    ub = in.getSecondStageRowUB(m_id-1);
  }

  int eq_cnt=0;
  for(size_t i=0; i<lb.size(); i++)
    if(lb[i]==ub[i])
      vec[eq_cnt++]=lb[i];  

  assert(eq_cnt-m_my==0);

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createb();
    svec->AddChild(child);
  }
  return svec;
}

namespace {
// ------------------------------------------------------------
// Internal code used to extract eq. and ineq. matrices from
// stochasticInput (which stores all constraints in one matrix)
// ------------------------------------------------------------

// Functors used to templatize the code for extracting
// equality and inequality matrices
class eq_comp
{
public:
  inline bool operator()(const double& lb, const double& ub) const { return (lb==ub); }
};

class ineq_comp
{
public:
  inline bool operator()(const double& lb, const double& ub) const { return (lb!=ub); }
};

/** Counts the nnz in Mcol's rows corresponding to entries in lb and ub 
 * satisfying 'compFun' condition. */

template<typename Compare>
int countNNZ(const CoinPackedMatrix& M, 
	     const vector<double>& lb, const vector<double>& ub, 
	     const Compare& compFun)
{
 
  int nnz=0;
  size_t R=lb.size();
  
  for(size_t i=0; i<R; i++) {
    
    if (compFun(lb[i],ub[i])) {
      nnz += M.getVectorSize(i);
      //cout << "i=" << i << "   nnz=" << nnz << endl;
    }
  }
  
  return nnz;
}		 

/** Extracts the Mcol's rows corresponding to entries in lb and ub 
 * satisfying 'compFun' condition. Mcol is in row-major format, the
 * output krowM,jcolM,dM rexepresent a row-major submatrix of Mcol. */
template<typename Compare>
void extractRows(const CoinPackedMatrix& M, 
		 const vector<double>& lb, const vector<double>& ub, 
		 const Compare& compFun,
		 int* krowM, int* jcolM, double* dM)
{
  size_t R=lb.size();

  int nRow=0;
  int* indRow=new int[R];//overallocated, but that's fine
  
  for(size_t i=0; i<R; i++) {
    if (compFun(lb[i],ub[i])) {
      indRow[nRow++]=i;
    }
  }

  if (nRow==0) {
    krowM[0]=0;
    delete[] indRow;
    return;
  }

  CoinPackedMatrix Msub;

  Msub.submatrixOf(M, nRow, indRow); //this seems to crash if nRow==0

  delete[] indRow;
  assert(false==Msub.hasGaps());

  memcpy(krowM,Msub.getVectorStarts(),(Msub.getNumRows()+1)*sizeof(int));
  memcpy(jcolM,Msub.getIndices(),Msub.getNumElements()*sizeof(int));
  memcpy(dM,Msub.getElements(),Msub.getNumElements()*sizeof(double));

  /*printf("Rows=%d Cols=%d nnz=%d\n", Msub.getNumRows(), Msub.getNumCols(), Msub.getNumElements());
  for(int r=0;r<=Msub.getNumRows(); r++) printf("%d ", krowM[r]); printf("\n");
  for(int e=0; e<Msub.getNumElements(); e++) printf("%d ", jcolM[e]); printf("\n");
  for(int e=0; e<Msub.getNumElements(); e++) printf("%12.5e ", dM[e]); printf("\n");
  printf("---------------\n");
  */

  //r.getElements() returns a vector containing:
  //    3 1 -2 -1 -1 2 1.1 1 1 2.8 -1.2 5.6 1 1.9
  //  r.getIndices() returns a vector containing:
  //    0 1  3  4  7 1 2   2 5 3    6   0   4 7
  //  r.getVectorStarts() returns a vector containing:
  //    0 5 7 9 11 14
}		 
} // end of the unnamed namespace

StochGenMatrix* sTreeImpl::createA() 
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochGenDummyMatrix(m_id);

  StochGenMatrix* A = NULL;
  if (m_id==0) {
    CoinPackedMatrix Arow; 
    Arow.reverseOrderedCopyOf( in.getFirstStageConstraints() );
    assert(false==Arow.hasGaps());  
    // number of nz in the rows corresponding to eq constraints
    int nnzB=countNNZ( Arow, 
		       in.getFirstStageRowLB(), 
		       in.getFirstStageRowUB(), 
		       eq_comp());
    if (!m_mle)
    {
        A = new StochGenMatrix( m_id, MY, N, 
			    m_my, 0,   0,    // A does not exist for the root
			    m_my, m_nx, nnzB, // B is 1st stage eq matrix
			    commWrkrs );
        extractRows( Arow,
	    	 in.getFirstStageRowLB(), in.getFirstStageRowUB(), eq_comp(),
		 A->Bmat->krowM(), A->Bmat->jcolM(), A->Bmat->M() );
    }
    else
      { 
	CoinPackedMatrix Erow;
	Erow.reverseOrderedCopyOf( in.getLinkMatrix(m_id));
	int nnzE=countNNZ( Erow,
			   in.getLinkRowLB(),
                           in.getLinkRowUB(),
			   eq_comp());
        A = new StochGenMatrix( m_id, MY, N,
				m_my, 0,   0,    // A does not exist for the root
				m_my, m_nx, nnzB, // B is 1st stage eq matrix
				commWrkrs, m_mle, m_nx, nnzE );
        extractRows( Arow,
		     in.getFirstStageRowLB(), in.getFirstStageRowUB(), eq_comp(),
		     A->Bmat->krowM(), A->Bmat->jcolM(), A->Bmat->M() );
        extractRows( Erow,
		     in.getLinkRowLB(), in.getLinkRowUB(), eq_comp(),
		     A->Cmat->krowM(), A->Cmat->jcolM(), A->Cmat->M() );
	//A->Bmat->atPutSubmatrix(m_my-m_mle, 0, *(A->Cmat), 0, 0, m_mle, m_nx); 
      }
  } else {
    int scen=m_id-1;
    CoinPackedMatrix Arow, Brow; 
    Arow.reverseOrderedCopyOf( in.getLinkingConstraints(scen) );
    Brow.reverseOrderedCopyOf( in.getSecondStageConstraints(scen) );

    int nnzA=countNNZ( Arow, in.getSecondStageRowLB(scen), 
		       in.getSecondStageRowUB(scen), eq_comp() );
    int nnzB=countNNZ( Brow, in.getSecondStageRowLB(scen), 
		       in.getSecondStageRowUB(scen), eq_comp() );

    if (!m_mle)
    {
        A = new StochGenMatrix( m_id, MY, N, 
			    m_my, parent->m_nx, nnzA, 
			    m_my, m_nx,         nnzB,
			    commWrkrs );
	extractRows( Arow,
		 in.getSecondStageRowLB(scen), 
		 in.getSecondStageRowUB(scen), 
		 eq_comp(),
		 A->Amat->krowM(), A->Amat->jcolM(), A->Amat->M() );
	extractRows( Brow,
		 in.getSecondStageRowLB(scen), 
		 in.getSecondStageRowUB(scen), 
		 eq_comp(),
		 A->Bmat->krowM(), A->Bmat->jcolM(), A->Bmat->M() );
    }
    else
      {
	CoinPackedMatrix Erow;
        Erow.reverseOrderedCopyOf( in.getLinkMatrix(m_id));
	int nnzE=countNNZ( Erow,
			   in.getLinkRowLB(),
                           in.getLinkRowUB(),
                           eq_comp());

        A = new StochGenMatrix( m_id, MY, N,
				m_my, parent->m_nx, nnzA,
				m_my, m_nx,         nnzB,
				commWrkrs, m_mle, m_nx, nnzE);
	extractRows( Arow,
		     in.getSecondStageRowLB(scen),
		     in.getSecondStageRowUB(scen),
		     eq_comp(),
		     A->Amat->krowM(), A->Amat->jcolM(), A->Amat->M() );
        extractRows( Brow,
		     in.getSecondStageRowLB(scen),
		     in.getSecondStageRowUB(scen),
		     eq_comp(),
		     A->Bmat->krowM(), A->Bmat->jcolM(), A->Bmat->M() );

        extractRows( Erow,
                     in.getLinkRowLB(), in.getLinkRowUB(), eq_comp(),
                     A->Cmat->krowM(), A->Cmat->jcolM(), A->Cmat->M() );
      }
  }
  
  for(size_t it=0; it<children.size(); it++) {
    StochGenMatrix* child = ((sTreeImpl*)children[it])->createA();
    A->AddChild(child);
  }
  return A;
}
StochGenMatrix* sTreeImpl::createC() 
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochGenDummyMatrix(m_id);

  StochGenMatrix* C = NULL;
  if (m_id==0) {
    CoinPackedMatrix Crow; 
    Crow.reverseOrderedCopyOf( in.getFirstStageConstraints() );

    // number of nz in the rows corresponding to ineq constraints
    int nnzD=countNNZ( Crow, 
		       in.getFirstStageRowLB(), 
		       in.getFirstStageRowUB(), 
		       ineq_comp());

    if (!m_mli)
    {
        C = new StochGenMatrix( m_id, MZ, N, 
			    m_mz, 0,   0,    // C does not exist for the root
			    m_mz, m_nx, nnzD, // D is 1st stage ineq matrix
			    commWrkrs );
	extractRows( Crow,
		 in.getFirstStageRowLB(), in.getFirstStageRowUB(), ineq_comp(),
		 C->Bmat->krowM(), C->Bmat->jcolM(), C->Bmat->M() );
	//printf("  -- 1st stage mz=%lu nx=%lu nnzD=%d\n", m_mz, m_nx, nnzD);
    }
    else
      {
        CoinPackedMatrix Frow;
        Frow.reverseOrderedCopyOf( in.getLinkMatrix(m_id));
        int nnzF=countNNZ( Frow,
                           in.getLinkRowLB(),
                           in.getLinkRowUB(),
                           ineq_comp());
        C = new StochGenMatrix( m_id, MZ, N,
				m_mz, 0,   0,    // C does not exist for the root
				m_mz, m_nx, nnzD, // D is 1st stage ineq matrix
				commWrkrs, m_mli, m_nx, nnzF );
        extractRows( Crow,
		     in.getFirstStageRowLB(), in.getFirstStageRowUB(), ineq_comp(),
		     C->Bmat->krowM(), C->Bmat->jcolM(), C->Bmat->M() );
	extractRows( Frow,
                     in.getLinkRowLB(), in.getLinkRowUB(), ineq_comp(),
                     C->Cmat->krowM(), C->Cmat->jcolM(), C->Cmat->M() );
      }
  } else {
    int scen=m_id-1;
    CoinPackedMatrix Crow, Drow; 
    Crow.reverseOrderedCopyOf( in.getLinkingConstraints(scen) );
    Drow.reverseOrderedCopyOf( in.getSecondStageConstraints(scen) );

    int nnzC=countNNZ( Crow, in.getSecondStageRowLB(scen), 
		       in.getSecondStageRowUB(scen), ineq_comp() );
    int nnzD=countNNZ( Drow, in.getSecondStageRowLB(scen), 
		       in.getSecondStageRowUB(scen), ineq_comp() );

    if (!m_mli)
    {
        C = new StochGenMatrix( m_id, MZ, N, 
			    m_mz, parent->m_nx, nnzC, 
			    m_mz, m_nx,         nnzD,
			    commWrkrs );
	extractRows( Crow,
		 in.getSecondStageRowLB(scen), 
		 in.getSecondStageRowUB(scen), 
		 ineq_comp(),
		 C->Amat->krowM(), C->Amat->jcolM(), C->Amat->M() );
	extractRows( Drow,
		 in.getSecondStageRowLB(scen), 
		 in.getSecondStageRowUB(scen), 
		 ineq_comp(),
		 C->Bmat->krowM(), C->Bmat->jcolM(), C->Bmat->M() );
    }
    else
    {
        CoinPackedMatrix Frow;
	Frow.reverseOrderedCopyOf( in.getLinkMatrix(m_id));
	int nnzF=countNNZ( Frow,
			   in.getLinkRowLB(),
                           in.getLinkRowUB(),
			 ineq_comp()); 
	C = new StochGenMatrix( m_id, MZ, N,
 				m_mz, parent->m_nx, nnzC,
				m_mz, m_nx,         nnzD,
				commWrkrs , m_mli, m_nx, nnzF);
        extractRows( Crow,
		     in.getSecondStageRowLB(scen),
		     in.getSecondStageRowUB(scen),
		     ineq_comp(),
		     C->Amat->krowM(), C->Amat->jcolM(), C->Amat->M() );
        extractRows( Drow,
		     in.getSecondStageRowLB(scen),
		     in.getSecondStageRowUB(scen),
		     ineq_comp(),
		     C->Bmat->krowM(), C->Bmat->jcolM(), C->Bmat->M() );
	extractRows( Frow,
                     in.getLinkRowLB(), in.getLinkRowUB(), ineq_comp(),
                     C->Cmat->krowM(), C->Cmat->jcolM(), C->Cmat->M() );

    }
  }
  
  for(size_t it=0; it<children.size(); it++) {
    StochGenMatrix* child = ((sTreeImpl*)children[it])->createC();
    C->AddChild(child);
  }
  return C;
}

StochVector* sTreeImpl::createxlow()  const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();
 
  StochVector* xlow = new StochVector(m_nx, commWrkrs);
  double* vec = ((SimpleVector*)xlow->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> x;
  if(m_id==0)
    x=in.getFirstStageColLB();
  else 
    x=in.getSecondStageColLB(m_id-1);

  for(size_t i=0; i<m_nx; i++)
    if(x[i]>-1.0e20) 
	  vec[i]=x[i];
    else         
	  vec[i]=0.0;

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createxlow();
    xlow->AddChild(child);
  }
  return xlow;
}

StochVector* sTreeImpl::createixlow() const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();
 
  StochVector* ixlow = new StochVector(m_nx, commWrkrs);
  double* vec = ((SimpleVector*)ixlow->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> x;
  if(m_id==0)
    x=in.getFirstStageColLB();
  else 
    x=in.getSecondStageColLB(m_id-1);

  for(size_t i=0; i<m_nx; i++)
    if(x[i]>-1.0e20) 
	  vec[i]=1.0;
    else             
	  vec[i]=0.0;

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createixlow();
    ixlow->AddChild(child);
  }
  return ixlow;
}

// ----------------------------------------------------------
// The rest of the public methods implemented below.
// ----------------------------------------------------------

StochVector* sTreeImpl::createxupp()  const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* xupp = new StochVector(m_nx, commWrkrs);
  double* vec = ((SimpleVector*)xupp->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> x;
  if(m_id==0)
    x=in.getFirstStageColUB();
  else 
    x=in.getSecondStageColUB(m_id-1);
    
  for(size_t i=0; i<m_nx; i++)
    if(x[i]<1.0e+20) 
      vec[i]=x[i];
    else 
      vec[i]=0.0;
  
  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createxupp();
    xupp->AddChild(child);
  }
  return xupp;
}

StochVector* sTreeImpl::createixupp() const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* ixupp = new StochVector(m_nx, commWrkrs);
  double* vec = ((SimpleVector*)ixupp->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> x;
  if(m_id==0)
    x=in.getFirstStageColUB();
  else 
    x=in.getSecondStageColUB(m_id-1);
    
  for(size_t i=0; i<m_nx; i++)
    if(x[i]<1.0e+20) 
      vec[i]=1.0;
    else 
      vec[i]=0.0;
  
  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createixupp();
    ixupp->AddChild(child);
  }
  return ixupp;
}

StochVector* sTreeImpl::createclow()  const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* svec = new StochVector(m_mz, commWrkrs);
  double* vec = ((SimpleVector*)svec->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> lb, ub;
  if(m_id==0) {
    lb = in.getFirstStageRowLB();
    ub = in.getFirstStageRowUB();
  } else {
    lb = in.getSecondStageRowLB(m_id-1);
    ub = in.getSecondStageRowUB(m_id-1);
  }
  int ineq_cnt=0;
  for(size_t i=0; i<lb.size(); i++)
    if(lb[i]!=ub[i]) {
      if(lb[i]>-1.0e20)
		vec[ineq_cnt]=lb[i];  
      else
		vec[ineq_cnt]=0.0;
      	ineq_cnt++;
    }

  assert(ineq_cnt-m_mz==0);

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createclow();
    svec->AddChild(child);
  }
  return svec;
}

StochVector* sTreeImpl::createiclow() const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* svec = new StochVector(m_mz, commWrkrs);
  double* vec = ((SimpleVector*)svec->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> lb, ub;
  if(m_id==0) {
    lb = in.getFirstStageRowLB();
    ub = in.getFirstStageRowUB();
  } else {
    lb = in.getSecondStageRowLB(m_id-1);
    ub = in.getSecondStageRowUB(m_id-1);
  }
  int ineq_cnt=0;
  for(size_t i=0; i<lb.size(); i++)
    if(lb[i]!=ub[i]) {
      if(lb[i]>-1.0e20)
		vec[ineq_cnt]=1.0;  
      else
		vec[ineq_cnt]=0.0;
      	ineq_cnt++;
    }

  assert(ineq_cnt-m_mz==0);

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createiclow();
    svec->AddChild(child);
  }
  return svec;
}

StochVector* sTreeImpl::createcupp()  const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* svec = new StochVector(m_mz, commWrkrs);
  double* vec = ((SimpleVector*)svec->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> lb, ub;
  if(m_id==0) {
    lb = in.getFirstStageRowLB();
    ub = in.getFirstStageRowUB();
  } else {
    lb = in.getSecondStageRowLB(m_id-1);
    ub = in.getSecondStageRowUB(m_id-1);
  }
  int ineq_cnt=0;
  for(size_t i=0; i<lb.size(); i++)
    if(lb[i]!=ub[i]) {
      if(ub[i]<1.0e20)
		vec[ineq_cnt]=ub[i];  
      else
		vec[ineq_cnt]=0.0;
      	ineq_cnt++;
    }

  assert(ineq_cnt-m_mz==0);

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createcupp();
    svec->AddChild(child);
  }
  return svec;
}

StochVector* sTreeImpl::createicupp() const
{
  //is this node a dead-end for this process?
  if(commWrkrs==MPI_COMM_NULL)
    return new StochDummyVector();

  StochVector* svec = new StochVector(m_mz, commWrkrs);
  double* vec = ((SimpleVector*)svec->vec)->elements();  

  //get the data from the stochasticInput
  vector<double> lb, ub;
  if(m_id==0) {
    lb = in.getFirstStageRowLB();
    ub = in.getFirstStageRowUB();
  } else {
    lb = in.getSecondStageRowLB(m_id-1);
    ub = in.getSecondStageRowUB(m_id-1);
  }
  int ineq_cnt=0;
  for(size_t i=0; i<lb.size(); i++)
    if(lb[i]!=ub[i]) {
      if(ub[i]<1.0e20)
		vec[ineq_cnt]=1.0;  
      else
		vec[ineq_cnt]=0.0;
      	ineq_cnt++;
    }

  assert(ineq_cnt-m_mz==0);

  for(size_t it=0; it<children.size(); it++) {
    StochVector* child = children[it]->createicupp();
    svec->AddChild(child);
  }
  return svec;
}


StochVector*	  sTreeImpl::createCeqBody() const
{
  return newDualYVector();
}
StochVector*	  sTreeImpl::createCineqBody()  const
{
  return newDualZVector();
}
StochVector*	  sTreeImpl::createBarrGrad() const 
{
  return newPrimalVector();
}



int sTreeImpl::nx() const
{
  return m_nx;
}

int sTreeImpl::my() const
{
  return m_my;
}
 
int sTreeImpl::mz() const
{
  return m_mz;
}
 
int sTreeImpl::id() const
{
  return m_id;
}
 

void sTreeImpl::computeGlobalSizes()
{
  long long  tempN=0,tempMY=0,tempMZ=0;

  for(size_t it=0; it<children.size(); it++) {
    sTreeImpl* c = dynamic_cast<sTreeImpl*>(children[it]);
    tempN += c->m_nx; tempMY += c->m_my; tempMZ += c->m_mz;
  }
  
  MPI_Allreduce(&tempN, &N, 1, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&tempMY, &MY, 1, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
  MPI_Allreduce(&tempMZ, &MZ, 1, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);

  N+=m_nx; MY+=m_my; MZ+=m_mz;  
}

// -----------------------------------------------------
// Helper methods
// -----------------------------------------------------
int sTreeImpl::compute_nFirstStageEq()
{
  int num=0;
  vector<double> lb=in.getFirstStageRowLB();
  vector<double> ub=in.getFirstStageRowUB();

  for (size_t i=0;i<lb.size(); i++)
    if (lb[i]==ub[i]) num++;
  return num;
}

int sTreeImpl::compute_nSecondStageEq(int scen)
{
  int num=0;
  vector<double> lb=in.getSecondStageRowLB(scen);
  vector<double> ub=in.getSecondStageRowUB(scen);

  for (size_t i=0;i<lb.size(); i++)
    if (lb[i]==ub[i]) num++;

  return num;
}

void sTreeImpl::get_FistStageSize(int& nx, int& my, int& mz)
{
  if (parent == NULL)
  {
    nx = m_nx;
    my = m_my;
    mz = m_mz;
  }
  else
  {
     nx = parent->m_nx;
    my = parent->m_my;
    mz = parent->m_mz;   
  }
}
