/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Paul Crozier (SNL)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pair_lj_cut_coul_long_omp.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "kspace.h"
#include "update.h"
#include "integrate.h"
#include "respa.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define EWALD_F   1.12837917
#define EWALD_P   0.3275911
#define A1        0.254829592
#define A2       -0.284496736
#define A3        1.421413741
#define A4       -1.453152027
#define A5        1.061405429

/* ---------------------------------------------------------------------- */

PairLJCutCoulLongOMP::PairLJCutCoulLongOMP(LAMMPS *lmp) : PairOMP(lmp)
{
  respa_enable = 1;
  ftable = NULL;
}

/* ---------------------------------------------------------------------- */

PairLJCutCoulLongOMP::~PairLJCutCoulLongOMP()
{
  if (allocated) {
    memory->destroy_2d_int_array(setflag);
    memory->destroy_2d_double_array(cutsq);

    memory->destroy_2d_double_array(cut_lj);
    memory->destroy_2d_double_array(cut_ljsq);
    memory->destroy_2d_double_array(epsilon);
    memory->destroy_2d_double_array(sigma);
    memory->destroy_2d_double_array(lj1);
    memory->destroy_2d_double_array(lj2);
    memory->destroy_2d_double_array(lj3);
    memory->destroy_2d_double_array(lj4);
    memory->destroy_2d_double_array(offset);
  }
  if (ftable) free_tables();
}

/* ---------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::compute(int eflag, int vflag)
{
  if (eflag || vflag) {
    ev_setup(eflag,vflag);
    ev_setup_thr(eflag,vflag);
  } else evflag = vflag_fdotr = 0;

  if (evflag) {
    if (eflag) {
      if (force->newton_pair) return eval<1,1,1>();
      else return eval<1,1,0>();
    } else {
      if (force->newton_pair) return eval<1,0,1>();
      else return eval<1,0,0>();
    }
  } else {
    if (force->newton_pair) return eval<0,0,1>();
    else return eval<0,0,0>();
  }
}

template <int EVFLAG, int EFLAG, int NEWTON_PAIR>
void PairLJCutCoulLongOMP::eval()
{

#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {

    int i,j,ii,jj,inum,jnum,itype,jtype,itable,tid;
    double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,evdwl,ecoul,fpair;
    double fraction,table;
    double r,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
    double grij,expm2,prefactor,t,erfc;
    int *ilist,*jlist,*numneigh,**firstneigh;
    double rsq;

    evdwl = ecoul = 0.0;

    const int nlocal = atom->nlocal;
    const int nall = nlocal + atom->nghost;
    const int nthreads = comm->nthreads;

    double **x = atom->x;
    double *q = atom->q;
    int *type = atom->type;
    double *special_coul = force->special_coul;
    double *special_lj = force->special_lj;
    double qqrd2e = force->qqrd2e;
    double fxtmp,fytmp,fztmp;

    inum = list->inum;
    ilist = list->ilist;
    numneigh = list->numneigh;
    firstneigh = list->firstneigh;

    // loop over neighbors of my atoms

    int iifrom, iito;
    double **f = loop_setup_thr(f, iifrom, iito, tid, inum, nall, nthreads);
    for (ii = iifrom; ii < iito; ++ii) {

      i = ilist[ii];
      qtmp = q[i];
      xtmp = x[i][0];
      ytmp = x[i][1];
      ztmp = x[i][2];
      itype = type[i];
      jlist = firstneigh[i];
      jnum = numneigh[i];
      fxtmp=fytmp=fztmp=0.0;

      for (jj = 0; jj < jnum; jj++) {
	j = jlist[jj];

	if (j < nall) factor_coul = factor_lj = 1.0;
	else {
	  factor_coul = special_coul[j/nall];
	  factor_lj = special_lj[j/nall];
	  j %= nall;
	}
	
	delx = xtmp - x[j][0];
	dely = ytmp - x[j][1];
	delz = ztmp - x[j][2];
	rsq = delx*delx + dely*dely + delz*delz;
        jtype = type[j];

	if (rsq < cutsq[itype][jtype]) {
	  r2inv = 1.0/rsq;

	  if (rsq < cut_coulsq) {
	    if (!ncoultablebits || rsq <= tabinnersq) {
	      r = sqrt(rsq);
	      grij = g_ewald * r;
	      expm2 = exp(-grij*grij);
	      t = 1.0 / (1.0 + EWALD_P*grij);
	      erfc = t * (A1+t*(A2+t*(A3+t*(A4+t*A5)))) * expm2;
	      prefactor = qqrd2e * qtmp*q[j]/r;
	      forcecoul = prefactor * (erfc + EWALD_F*grij*expm2);
	      if (factor_coul < 1.0) {
		forcecoul -= (1.0-factor_coul)*prefactor;
	      }
	    } else {
	      union_int_float_t rsq_lookup;
	      rsq_lookup.f = rsq;
	      itable = rsq_lookup.i & ncoulmask;
	      itable >>= ncoulshiftbits;
	      fraction = (rsq_lookup.f - rtable[itable]) * drtable[itable];
	      table = ftable[itable] + fraction*dftable[itable];
	      forcecoul = qtmp*q[j] * table;
	      if (factor_coul < 1.0) {
		table = ctable[itable] + fraction*dctable[itable];
		prefactor = qtmp*q[j] * table;
		forcecoul -= (1.0-factor_coul)*prefactor;
	      }
	    }
	  } else forcecoul = 0.0;

	  if (rsq < cut_ljsq[itype][jtype]) {
	    r6inv = r2inv*r2inv*r2inv;
	    forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
	  } else forcelj = 0.0;

	  fpair = (forcecoul + factor_lj*forcelj) * r2inv;

	  fxtmp += delx*fpair;
	  fytmp += dely*fpair;
	  fztmp += delz*fpair;
	  if (NEWTON_PAIR || j < nlocal) {
	    f[j][0] -= delx*fpair;
	    f[j][1] -= dely*fpair;
	    f[j][2] -= delz*fpair;
	  }

	  if (EFLAG) {
	    if (rsq < cut_coulsq) {
	      if (!ncoultablebits || rsq <= tabinnersq)
		ecoul = prefactor*erfc;
	      else {
		table = etable[itable] + fraction*detable[itable];
		ecoul = qtmp*q[j] * table;
	      }
	      if (factor_coul < 1.0) ecoul -= (1.0-factor_coul)*prefactor;
	    } else ecoul = 0.0;

	    if (rsq < cut_ljsq[itype][jtype]) {
	      evdwl = r6inv*(lj3[itype][jtype]*r6inv-lj4[itype][jtype]) -
		offset[itype][jtype];
	      evdwl *= factor_lj;
	    } else evdwl = 0.0;
	  }
	  if (EVFLAG) ev_tally_thr(i,j,nlocal,NEWTON_PAIR,
				   evdwl,ecoul,fpair,delx,dely,delz,tid);
	}
      }
      f[i][0] += fxtmp;
      f[i][1] += fytmp;
      f[i][2] += fztmp;
    }

    // reduce per thread forces into global force array.
    force_reduce_thr(atom->f, nall, nthreads, tid);
  }
  if (EVFLAG) ev_reduce_thr();
  if (vflag_fdotr) virial_compute();
}

/* ---------------------------------------------------------------------- */

 void PairLJCutCoulLongOMP::compute_inner()
{
  if (force->newton_pair) return eval_inner<1>();
  else return eval_inner<0>();
}

template <int NEWTON_PAIR>
void PairLJCutCoulLongOMP::eval_inner()
{
#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {
    int i,j,ii,jj,inum,jnum,itype,jtype,tid;
  double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,fpair;
  double rsq,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
  double rsw;
  int *ilist,*jlist,*numneigh,**firstneigh;

  const int nlocal = atom->nlocal;
  const int nall = nlocal + atom->nghost;
  const int nthreads = comm->nthreads;

  double **x = atom->x;
  double **f = atom->f;
  double *q = atom->q;
  int *type = atom->type;
  double *special_coul = force->special_coul;
  double *special_lj = force->special_lj;
  double qqrd2e = force->qqrd2e;

  inum = listinner->inum;
  ilist = listinner->ilist;
  numneigh = listinner->numneigh;
  firstneigh = listinner->firstneigh;

  double cut_out_on = cut_respa[0];
  double cut_out_off = cut_respa[1];

  double cut_out_diff = cut_out_off - cut_out_on;
  double cut_out_on_sq = cut_out_on*cut_out_on;
  double cut_out_off_sq = cut_out_off*cut_out_off;

  // loop over neighbors of my atoms
  int iifrom, iito;
  f = loop_setup_thr(f, iifrom, iito, tid, inum, nall, nthreads);
  for (ii = iifrom; ii < iito; ++ii) {
    i = ilist[ii];
    qtmp = q[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    itype = type[i];
    jlist = firstneigh[i];
    jnum = numneigh[i];

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];

      if (j < nall) factor_coul = factor_lj = 1.0;
      else {
	factor_coul = special_coul[j/nall];
	factor_lj = special_lj[j/nall];
	j %= nall;
      }

      delx = xtmp - x[j][0];
      dely = ytmp - x[j][1];
      delz = ztmp - x[j][2];
      rsq = delx*delx + dely*dely + delz*delz;

      if (rsq < cut_out_off_sq) {
        r2inv = 1.0/rsq;
        forcecoul = qqrd2e * qtmp*q[j]*sqrt(r2inv);
        if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*forcecoul;

        jtype = type[j];
	if (rsq < cut_ljsq[itype][jtype]) {
	  r6inv = r2inv*r2inv*r2inv;
	  forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
	} else forcelj = 0.0;

	fpair = (forcecoul + factor_lj*forcelj) * r2inv;

        if (rsq > cut_out_on_sq) {
          rsw = (sqrt(rsq) - cut_out_on)/cut_out_diff;
	  fpair *= 1.0 + rsw*rsw*(2.0*rsw-3.0);
        }

        f[i][0] += delx*fpair;
        f[i][1] += dely*fpair;
        f[i][2] += delz*fpair;
        if (NEWTON_PAIR || j < nlocal) {
          f[j][0] -= delx*fpair;
          f[j][1] -= dely*fpair;
          f[j][2] -= delz*fpair;
        }
      }
    }
  }

    // reduce per thread forces into global force array.
    force_reduce_thr(atom->f, nall, nthreads, tid);
  }
}

/* ---------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::compute_middle()
{
  if (force->newton_pair) return eval_middle<1>();
  else return eval_middle<0>();
}

template <int NEWTON_PAIR>
void PairLJCutCoulLongOMP::eval_middle()
{
#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {

    int i,j,ii,jj,inum,jnum,itype,jtype,tid;
    double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,fpair;
    double rsq,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
    double rsw;
    int *ilist,*jlist,*numneigh,**firstneigh;

    const int nlocal = atom->nlocal;
    const int nall = nlocal + atom->nghost;
    const int nthreads = comm->nthreads;

    double **x = atom->x;
    double **f = atom->f;
    double *q = atom->q;
    int *type = atom->type;
    double *special_coul = force->special_coul;
    double *special_lj = force->special_lj;
    double qqrd2e = force->qqrd2e;

    inum = listmiddle->inum;
    ilist = listmiddle->ilist;
    numneigh = listmiddle->numneigh;
    firstneigh = listmiddle->firstneigh;

    double cut_in_off = cut_respa[0];
    double cut_in_on = cut_respa[1];
    double cut_out_on = cut_respa[2];
    double cut_out_off = cut_respa[3];

    double cut_in_diff = cut_in_on - cut_in_off;
    double cut_out_diff = cut_out_off - cut_out_on;
    double cut_in_off_sq = cut_in_off*cut_in_off;
    double cut_in_on_sq = cut_in_on*cut_in_on;
    double cut_out_on_sq = cut_out_on*cut_out_on;
    double cut_out_off_sq = cut_out_off*cut_out_off;

    // loop over neighbors of my atoms
    int iifrom, iito;
    f = loop_setup_thr(f, iifrom, iito, tid, inum, nall, nthreads);
    for (ii = iifrom; ii < iito; ++ii) {

      i = ilist[ii];
      qtmp = q[i];
      xtmp = x[i][0];
      ytmp = x[i][1];
      ztmp = x[i][2];
      itype = type[i];

      jlist = firstneigh[i];
      jnum = numneigh[i];

      for (jj = 0; jj < jnum; jj++) {
	j = jlist[jj];

	if (j < nall) factor_coul = factor_lj = 1.0;
	else {
	  factor_coul = special_coul[j/nall];
	  factor_lj = special_lj[j/nall];
	  j %= nall;
	}

	delx = xtmp - x[j][0];
	dely = ytmp - x[j][1];
	delz = ztmp - x[j][2];
	rsq = delx*delx + dely*dely + delz*delz;

	if (rsq < cut_out_off_sq && rsq > cut_in_off_sq) {
	  r2inv = 1.0/rsq;
	  forcecoul = qqrd2e * qtmp*q[j]*sqrt(r2inv);
	  if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*forcecoul;

	  jtype = type[j];
	if (rsq < cut_ljsq[itype][jtype]) {
	  r6inv = r2inv*r2inv*r2inv;
	  forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
	} else forcelj = 0.0;

	  fpair = (forcecoul + factor_lj*forcelj) * r2inv;
	  if (rsq < cut_in_on_sq) {
	    rsw = (sqrt(rsq) - cut_in_off)/cut_in_diff;
	    fpair *= rsw*rsw*(3.0 - 2.0*rsw);
	  }
	  if (rsq > cut_out_on_sq) {
	    rsw = (sqrt(rsq) - cut_out_on)/cut_out_diff;
	    fpair *= 1.0 + rsw*rsw*(2.0*rsw - 3.0);
	  }

	  f[i][0] += delx*fpair;
	  f[i][1] += dely*fpair;
	  f[i][2] += delz*fpair;
	  if (NEWTON_PAIR || j < nlocal) {
	    f[j][0] -= delx*fpair;
	    f[j][1] -= dely*fpair;
	    f[j][2] -= delz*fpair;
	  }
	}
      }
    }

    // reduce per thread forces into global force array.
    force_reduce_thr(atom->f, nall, nthreads, tid);
  }
}

/* ---------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::compute_outer(int eflag, int vflag)
{
  if (eflag || vflag) {
    ev_setup(eflag,vflag);
    ev_setup_thr(eflag,vflag);
  } else evflag = vflag_fdotr = 0;

  if (evflag) {
    if (eflag) {
      if (vflag) {
	if (force->newton_pair) return eval_outer<1,1,1,1>();
	else return eval_outer<1,1,1,0>();
      } else {
	if (force->newton_pair) return eval_outer<1,1,0,1>();
	else return eval_outer<1,1,0,0>();
      }
    } else {
      if (vflag) {
	if (force->newton_pair) return eval_outer<1,0,1,1>();
	else return eval_outer<1,0,1,0>();
      } else {
	if (force->newton_pair) return eval_outer<1,0,0,1>();
	else return eval_outer<1,0,0,0>();
      }
    }
  } else {
    if (force->newton_pair) return eval_outer<0,0,0,1>();
    else return eval_outer<0,0,0,0>();
  }
}

template <int EVFLAG, int EFLAG, int VFLAG, int NEWTON_PAIR>
void PairLJCutCoulLongOMP::eval_outer()
{
#if defined(_OPENMP)
#pragma omp parallel default(shared)
#endif
  {

    int i,j,ii,jj,inum,jnum,itype,jtype,itable,tid;
    double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,evdwl,ecoul,fpair;
    double fraction,table;
    double r,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
    double grij,expm2,prefactor,t,erfc;
    double rsw;
    int *ilist,*jlist,*numneigh,**firstneigh;
    double rsq;

    evdwl = ecoul = 0.0;

    const int nlocal = atom->nlocal;
    const int nall = nlocal + atom->nghost;
    const int nthreads = comm->nthreads;

    double **x = atom->x;
    double **f = atom->f;
    double *q = atom->q;
    int *type = atom->type;
    double *special_coul = force->special_coul;
    double *special_lj = force->special_lj;
    double qqrd2e = force->qqrd2e;

    inum = listouter->inum;
    ilist = listouter->ilist;
    numneigh = listouter->numneigh;
    firstneigh = listouter->firstneigh;

    double cut_in_off = cut_respa[2];
    double cut_in_on = cut_respa[3];

    double cut_in_diff = cut_in_on - cut_in_off;
    double cut_in_off_sq = cut_in_off*cut_in_off;
    double cut_in_on_sq = cut_in_on*cut_in_on;

    // loop over neighbors of my atoms

    int iifrom, iito;
    f = loop_setup_thr(f, iifrom, iito, tid, inum, nall, nthreads);
    for (ii = iifrom; ii < iito; ++ii) {

      i = ilist[ii];
      qtmp = q[i];
      xtmp = x[i][0];
      ytmp = x[i][1];
      ztmp = x[i][2];
      itype = type[i];
      jlist = firstneigh[i];
      jnum = numneigh[i];

      for (jj = 0; jj < jnum; jj++) {
	j = jlist[jj];

	if (j < nall) factor_coul = factor_lj = 1.0;
	else {
	  factor_coul = special_coul[j/nall];
	  factor_lj = special_lj[j/nall];
	  j %= nall;
	}

	delx = xtmp - x[j][0];
	dely = ytmp - x[j][1];
	delz = ztmp - x[j][2];
	rsq = delx*delx + dely*dely + delz*delz;
	jtype = type[j];

	if (rsq < cutsq[itype][jtype]) {
            r2inv = 1.0/rsq;

	  if (rsq < cut_coulsq) {
	  if (!ncoultablebits || rsq <= tabinnersq) {
	    r = sqrt(rsq);
	    grij = g_ewald * r;
	    expm2 = exp(-grij*grij);
	    t = 1.0 / (1.0 + EWALD_P*grij);
	    erfc = t * (A1+t*(A2+t*(A3+t*(A4+t*A5)))) * expm2;
	    prefactor = qqrd2e * qtmp*q[j]/r;
	    forcecoul = prefactor * (erfc + EWALD_F*grij*expm2 - 1.0);
	    if (rsq > cut_in_off_sq) {
	      if (rsq < cut_in_on_sq) {
	        rsw = (r - cut_in_off)/cut_in_diff;
	        forcecoul += prefactor*rsw*rsw*(3.0 - 2.0*rsw);
	        if (factor_coul < 1.0)
		  forcecoul -= (1.0-factor_coul)*prefactor*rsw*rsw*(3.0 - 2.0*rsw);
	      } else {
	        forcecoul += prefactor;
	        if (factor_coul < 1.0)
		  forcecoul -= (1.0-factor_coul)*prefactor;
	      }
	    }
	  } else {
	    union_int_float_t rsq_lookup;
	    rsq_lookup.f = rsq;
	    itable = rsq_lookup.i & ncoulmask;
	    itable >>= ncoulshiftbits;
	    fraction = (rsq_lookup.f - rtable[itable]) * drtable[itable];
	    table = ftable[itable] + fraction*dftable[itable];
	    forcecoul = qtmp*q[j] * table;
	    if (factor_coul < 1.0) {
	      table = ctable[itable] + fraction*dctable[itable];
	      prefactor = qtmp*q[j] * table;
	      forcecoul -= (1.0-factor_coul)*prefactor;
	    }
	  }
	} else forcecoul = 0.0;

	if (rsq < cut_ljsq[itype][jtype] && rsq > cut_in_off_sq) {
	  r6inv = r2inv*r2inv*r2inv;
	  forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
	  if (rsq < cut_in_on_sq) {
	    rsw = (sqrt(rsq) - cut_in_off)/cut_in_diff;
	    forcelj *= rsw*rsw*(3.0 - 2.0*rsw);
	  }
	} else forcelj = 0.0;

	  fpair = (forcecoul + forcelj) * r2inv;

	  f[i][0] += delx*fpair;
	  f[i][1] += dely*fpair;
	  f[i][2] += delz*fpair;
	  if (NEWTON_PAIR || j < nlocal) {
	    f[j][0] -= delx*fpair;
	    f[j][1] -= dely*fpair;
	    f[j][2] -= delz*fpair;
	  }

	  if (EFLAG) {
	    if (rsq < cut_coulsq) {
	      if (!ncoultablebits || rsq <= tabinnersq) {
		ecoul = prefactor*erfc;
		if (factor_coul < 1.0) ecoul -= (1.0-factor_coul)*prefactor;
	      } else {
		table = etable[itable] + fraction*detable[itable];
		ecoul = qtmp*q[j] * table;
		if (factor_coul < 1.0) {
		  table = ptable[itable] + fraction*dptable[itable];
		  prefactor = qtmp*q[j] * table;
		  ecoul -= (1.0-factor_coul)*prefactor;
		}
	      }
	    } else ecoul = 0.0;

                if (rsq < cut_ljsq[itype][jtype]) {
                    r6inv = r2inv*r2inv*r2inv;
                    evdwl = r6inv*(lj3[itype][jtype]*r6inv-lj4[itype][jtype]) -
                      offset[itype][jtype];
                    evdwl *= factor_lj;
              } else evdwl = 0.0;
            }

	  if (VFLAG) {
	    if (rsq < cut_coulsq) {
	      if (!ncoultablebits || rsq <= tabinnersq) {
		forcecoul = prefactor * (erfc + EWALD_F*grij*expm2);
		if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*prefactor;
	      } else {
		table = vtable[itable] + fraction*dvtable[itable];
		forcecoul = qtmp*q[j] * table;
		if (factor_coul < 1.0) {
		  table = ptable[itable] + fraction*dptable[itable];
		  prefactor = qtmp*q[j] * table;
		  forcecoul -= (1.0-factor_coul)*prefactor;
		}
	      }
	    } else forcecoul = 0.0;

	    if (rsq <= cut_in_off_sq) {
                r6inv = r2inv*r2inv*r2inv;
                forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
	    } else if (rsq <= cut_in_on_sq)
                forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);

            fpair = (forcecoul + factor_lj*forcelj) * r2inv;
	    

	    fpair = (forcecoul + factor_lj*forcelj) * r2inv;
	  }

	  if (EVFLAG) ev_tally_thr(i,j,nlocal,NEWTON_PAIR,
				   evdwl,ecoul,fpair,delx,dely,delz,tid);
	}
      }
    }
    // reduce per thread forces into global force array.
    force_reduce_thr(atom->f, nall, nthreads, tid);
  }
  // reduce per thread accumulators
  if (EVFLAG) ev_reduce_thr();
  if (vflag_fdotr) virial_compute();
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::allocate()
{
  allocated = 1;
  int n = atom->ntypes;

  setflag = memory->create_2d_int_array(n+1,n+1,"pair:setflag");
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;

  cutsq = memory->create_2d_double_array(n+1,n+1,"pair:cutsq");

  cut_lj = memory->create_2d_double_array(n+1,n+1,"pair:cut_lj");
  cut_ljsq = memory->create_2d_double_array(n+1,n+1,"pair:cut_ljsq");
  epsilon = memory->create_2d_double_array(n+1,n+1,"pair:epsilon");
  sigma = memory->create_2d_double_array(n+1,n+1,"pair:sigma");
  lj1 = memory->create_2d_double_array(n+1,n+1,"pair:lj1");
  lj2 = memory->create_2d_double_array(n+1,n+1,"pair:lj2");
  lj3 = memory->create_2d_double_array(n+1,n+1,"pair:lj3");
  lj4 = memory->create_2d_double_array(n+1,n+1,"pair:lj4");
  offset = memory->create_2d_double_array(n+1,n+1,"pair:offset");
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::settings(int narg, char **arg)
{
 if (narg < 1 || narg > 2) error->all("Illegal pair_style command");

  cut_lj_global = force->numeric(arg[0]);
  if (narg == 1) cut_coul = cut_lj_global;
  else cut_coul = force->numeric(arg[1]);

  // reset cutoffs that have been explicitly set

  if (allocated) {
    int i,j;
    for (i = 1; i <= atom->ntypes; i++)
      for (j = i+1; j <= atom->ntypes; j++)
	if (setflag[i][j]) cut_lj[i][j] = cut_lj_global;
  }
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::coeff(int narg, char **arg)
{
  if (narg < 4 || narg > 5) error->all("Incorrect args for pair coefficients");
  if (!allocated) allocate();

  int ilo,ihi,jlo,jhi;
  force->bounds(arg[0],atom->ntypes,ilo,ihi);
  force->bounds(arg[1],atom->ntypes,jlo,jhi);

  double epsilon_one = force->numeric(arg[2]);
  double sigma_one = force->numeric(arg[3]);

  double cut_lj_one = cut_lj_global;
  if (narg == 5) cut_lj_one = force->numeric(arg[4]);

  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      epsilon[i][j] = epsilon_one;
      sigma[i][j] = sigma_one;
      cut_lj[i][j] = cut_lj_one;
      setflag[i][j] = 1;
      count++;
    }
  }

  if (count == 0) error->all("Incorrect args for pair coefficients");
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::init_style()
{
  if (!atom->q_flag)
    error->all("Pair style lj/cut/coul/long requires atom attribute q");

  // request regular or rRESPA neighbor lists

  int irequest;

  if (update->whichflag == 1 && strcmp(update->integrate_style,"respa") == 0) {
    int respa = 0;
    if (((Respa *) update->integrate)->level_inner >= 0) respa = 1;
    if (((Respa *) update->integrate)->level_middle >= 0) respa = 2;

    if (respa == 0) irequest = neighbor->request(this);
    else if (respa == 1) {
      irequest = neighbor->request(this);
      neighbor->requests[irequest]->id = 1;
      neighbor->requests[irequest]->half = 0;
      neighbor->requests[irequest]->respainner = 1;
      irequest = neighbor->request(this);
      neighbor->requests[irequest]->id = 3;
      neighbor->requests[irequest]->half = 0;
      neighbor->requests[irequest]->respaouter = 1;
    } else {
      irequest = neighbor->request(this);
      neighbor->requests[irequest]->id = 1;
      neighbor->requests[irequest]->half = 0;
      neighbor->requests[irequest]->respainner = 1;
      irequest = neighbor->request(this);
      neighbor->requests[irequest]->id = 2;
      neighbor->requests[irequest]->half = 0;
      neighbor->requests[irequest]->respamiddle = 1;
      irequest = neighbor->request(this);
      neighbor->requests[irequest]->id = 3;
      neighbor->requests[irequest]->half = 0;
      neighbor->requests[irequest]->respaouter = 1;
    }

  } else irequest = neighbor->request(this);

  cut_coulsq = cut_coul * cut_coul;

  // set rRESPA cutoffs

  if (strcmp(update->integrate_style,"respa") == 0 &&
      ((Respa *) update->integrate)->level_inner >= 0)
    cut_respa = ((Respa *) update->integrate)->cutoff;
  else cut_respa = NULL;

  // insure use of KSpace long-range solver, set g_ewald

  if (force->kspace == NULL)
    error->all("Pair style is incompatible with KSpace style");
  g_ewald = force->kspace->g_ewald;

  // setup force tables

  if (ncoultablebits) init_tables();
}

/* ----------------------------------------------------------------------
   neighbor callback to inform pair style of neighbor list to use
   regular or rRESPA
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::init_list(int id, NeighList *ptr)
{
  if (id == 0) list = ptr;
  else if (id == 1) listinner = ptr;
  else if (id == 2) listmiddle = ptr;
  else if (id == 3) listouter = ptr;
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairLJCutCoulLongOMP::init_one(int i, int j)
{
  if (setflag[i][j] == 0) {
    epsilon[i][j] = mix_energy(epsilon[i][i],epsilon[j][j],
			       sigma[i][i],sigma[j][j]);
    sigma[i][j] = mix_distance(sigma[i][i],sigma[j][j]);
    cut_lj[i][j] = mix_distance(cut_lj[i][i],cut_lj[j][j]);
  }

  double cut = MAX(cut_lj[i][j],cut_coul);
  cut_ljsq[i][j] = cut_lj[i][j] * cut_lj[i][j];

  lj1[i][j] = 48.0 * epsilon[i][j] * pow(sigma[i][j],12.0);
  lj2[i][j] = 24.0 * epsilon[i][j] * pow(sigma[i][j],6.0);
  lj3[i][j] = 4.0 * epsilon[i][j] * pow(sigma[i][j],12.0);
  lj4[i][j] = 4.0 * epsilon[i][j] * pow(sigma[i][j],6.0);

  if (offset_flag) {
    double ratio = sigma[i][j] / cut_lj[i][j];
    offset[i][j] = 4.0 * epsilon[i][j] * (pow(ratio,12.0) - pow(ratio,6.0));
  } else offset[i][j] = 0.0;

  cut_ljsq[j][i] = cut_ljsq[i][j];
  lj1[j][i] = lj1[i][j];
  lj2[j][i] = lj2[i][j];
  lj3[j][i] = lj3[i][j];
  lj4[j][i] = lj4[i][j];
  offset[j][i] = offset[i][j];

  // check interior rRESPA cutoff

  if (cut_respa && MIN(cut_lj[i][j],cut_coul) < cut_respa[3])
    error->all("Pair cutoff < Respa interior cutoff");

  // compute I,J contribution to long-range tail correction
  // count total # of atoms of type I and J via Allreduce

  if (tail_flag) {
    int *type = atom->type;
    int nlocal = atom->nlocal;

    double count[2],all[2];
    count[0] = count[1] = 0.0;
    for (int k = 0; k < nlocal; k++) {
      if (type[k] == i) count[0] += 1.0;
      if (type[k] == j) count[1] += 1.0;
    }
    MPI_Allreduce(count,all,2,MPI_DOUBLE,MPI_SUM,world);

    double PI = 4.0*atan(1.0);
    double sig2 = sigma[i][j]*sigma[i][j];
    double sig6 = sig2*sig2*sig2;
    double rc3 = cut_lj[i][j]*cut_lj[i][j]*cut_lj[i][j];
    double rc6 = rc3*rc3;
    double rc9 = rc3*rc6;
    etail_ij = 8.0*PI*all[0]*all[1]*epsilon[i][j] *
      sig6 * (sig6 - 3.0*rc6) / (9.0*rc9);
    ptail_ij = 16.0*PI*all[0]*all[1]*epsilon[i][j] *
      sig6 * (2.0*sig6 - 3.0*rc6) / (9.0*rc9);
  }

  return cut;
}

/* ----------------------------------------------------------------------
   setup force tables used in compute routines
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::init_tables()
{
  int masklo,maskhi;
  double r,grij,expm2,derfc,rsw;
  double qqrd2e = force->qqrd2e;

  tabinnersq = tabinner*tabinner;
  init_bitmap(tabinner,cut_coul,ncoultablebits,
	      masklo,maskhi,ncoulmask,ncoulshiftbits);

  int ntable = 1;
  for (int i = 0; i < ncoultablebits; i++) ntable *= 2;

  // linear lookup tables of length N = 2^ncoultablebits
  // stored value = value at lower edge of bin
  // d values = delta from lower edge to upper edge of bin

  if (ftable) free_tables();

  rtable = (double *) memory->smalloc(ntable*sizeof(double),"pair:rtable");
  ftable = (double *) memory->smalloc(ntable*sizeof(double),"pair:ftable");
  ctable = (double *) memory->smalloc(ntable*sizeof(double),"pair:ctable");
  etable = (double *) memory->smalloc(ntable*sizeof(double),"pair:etable");
  drtable = (double *) memory->smalloc(ntable*sizeof(double),"pair:drtable");
  dftable = (double *) memory->smalloc(ntable*sizeof(double),"pair:dftable");
  dctable = (double *) memory->smalloc(ntable*sizeof(double),"pair:dctable");
  detable = (double *) memory->smalloc(ntable*sizeof(double),"pair:detable");

  if (cut_respa == NULL) {
    vtable = ptable = dvtable = dptable = NULL;
  } else {
    vtable = (double *) memory->smalloc(ntable*sizeof(double),"pair:vtable");
    ptable = (double *) memory->smalloc(ntable*sizeof(double),"pair:ptable");
    dvtable = (double *) memory->smalloc(ntable*sizeof(double),"pair:dvtable");
    dptable = (double *) memory->smalloc(ntable*sizeof(double),"pair:dptable");
  }

  union_int_float_t rsq_lookup;
  union_int_float_t minrsq_lookup;
  int itablemin;
  minrsq_lookup.i = 0 << ncoulshiftbits;
  minrsq_lookup.i |= maskhi;

  for (int i = 0; i < ntable; i++) {
    rsq_lookup.i = i << ncoulshiftbits;
    rsq_lookup.i |= masklo;
    if (rsq_lookup.f < tabinnersq) {
      rsq_lookup.i = i << ncoulshiftbits;
      rsq_lookup.i |= maskhi;
    }
    r = sqrtf(rsq_lookup.f);
    grij = g_ewald * r;
    expm2 = exp(-grij*grij);
    derfc = erfc(grij);
    if (cut_respa == NULL) {
      rtable[i] = rsq_lookup.f;
      ftable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      ctable[i] = qqrd2e/r;
      etable[i] = qqrd2e/r * derfc;
    } else {
      rtable[i] = rsq_lookup.f;
      ftable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2 - 1.0);
      ctable[i] = 0.0;
      etable[i] = qqrd2e/r * derfc;
      ptable[i] = qqrd2e/r;
      vtable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      if (rsq_lookup.f > cut_respa[2]*cut_respa[2]) {
	if (rsq_lookup.f < cut_respa[3]*cut_respa[3]) {
	  rsw = (r - cut_respa[2])/(cut_respa[3] - cut_respa[2]);
	  ftable[i] += qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
	  ctable[i] = qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
	} else {
	  ftable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
	  ctable[i] = qqrd2e/r;
	}
      }
    }
    minrsq_lookup.f = MIN(minrsq_lookup.f,rsq_lookup.f);
  }

  tabinnersq = minrsq_lookup.f;

  int ntablem1 = ntable - 1;

  for (int i = 0; i < ntablem1; i++) {
    drtable[i] = 1.0/(rtable[i+1] - rtable[i]);
    dftable[i] = ftable[i+1] - ftable[i];
    dctable[i] = ctable[i+1] - ctable[i];
    detable[i] = etable[i+1] - etable[i];
  }

  if (cut_respa) {
    for (int i = 0; i < ntablem1; i++) {
      dvtable[i] = vtable[i+1] - vtable[i];
      dptable[i] = ptable[i+1] - ptable[i];
    }
  }

  // get the delta values for the last table entries
  // tables are connected periodically between 0 and ntablem1

  drtable[ntablem1] = 1.0/(rtable[0] - rtable[ntablem1]);
  dftable[ntablem1] = ftable[0] - ftable[ntablem1];
  dctable[ntablem1] = ctable[0] - ctable[ntablem1];
  detable[ntablem1] = etable[0] - etable[ntablem1];
  if (cut_respa) {
    dvtable[ntablem1] = vtable[0] - vtable[ntablem1];
    dptable[ntablem1] = ptable[0] - ptable[ntablem1];
  }

  // get the correct delta values at itablemax
  // smallest r is in bin itablemin
  // largest r is in bin itablemax, which is itablemin-1,
  //   or ntablem1 if itablemin=0
  // deltas at itablemax only needed if corresponding rsq < cut*cut
  // if so, compute deltas between rsq and cut*cut

  double f_tmp,c_tmp,e_tmp,p_tmp,v_tmp;
  itablemin = minrsq_lookup.i & ncoulmask;
  itablemin >>= ncoulshiftbits;
  int itablemax = itablemin - 1;
  if (itablemin == 0) itablemax = ntablem1;
  rsq_lookup.i = itablemax << ncoulshiftbits;
  rsq_lookup.i |= maskhi;

  if (rsq_lookup.f < cut_coulsq) {
    rsq_lookup.f = cut_coulsq;
    r = sqrtf(rsq_lookup.f);
    grij = g_ewald * r;
    expm2 = exp(-grij*grij);
    derfc = erfc(grij);

    if (cut_respa == NULL) {
      f_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      c_tmp = qqrd2e/r;
      e_tmp = qqrd2e/r * derfc;
    } else {
      f_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2 - 1.0);
      c_tmp = 0.0;
      e_tmp = qqrd2e/r * derfc;
      p_tmp = qqrd2e/r;
      v_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      if (rsq_lookup.f > cut_respa[2]*cut_respa[2]) {
        if (rsq_lookup.f < cut_respa[3]*cut_respa[3]) {
          rsw = (r - cut_respa[2])/(cut_respa[3] - cut_respa[2]);
          f_tmp += qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
          c_tmp = qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
        } else {
          f_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
          c_tmp = qqrd2e/r;
        }
      }
    }

    drtable[itablemax] = 1.0/(rsq_lookup.f - rtable[itablemax]);
    dftable[itablemax] = f_tmp - ftable[itablemax];
    dctable[itablemax] = c_tmp - ctable[itablemax];
    detable[itablemax] = e_tmp - etable[itablemax];
    if (cut_respa) {
      dvtable[itablemax] = v_tmp - vtable[itablemax];
      dptable[itablemax] = p_tmp - ptable[itablemax];
    }
  }
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::write_restart(FILE *fp)
{
  write_restart_settings(fp);

  int i,j;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      fwrite(&setflag[i][j],sizeof(int),1,fp);
      if (setflag[i][j]) {
	fwrite(&epsilon[i][j],sizeof(double),1,fp);
	fwrite(&sigma[i][j],sizeof(double),1,fp);
	fwrite(&cut_lj[i][j],sizeof(double),1,fp);
      }
    }
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::read_restart(FILE *fp)
{
  read_restart_settings(fp);

  allocate();

  int i,j;
  int me = comm->me;
  for (i = 1; i <= atom->ntypes; i++)
    for (j = i; j <= atom->ntypes; j++) {
      if (me == 0) fread(&setflag[i][j],sizeof(int),1,fp);
      MPI_Bcast(&setflag[i][j],1,MPI_INT,0,world);
      if (setflag[i][j]) {
	if (me == 0) {
	  fread(&epsilon[i][j],sizeof(double),1,fp);
	  fread(&sigma[i][j],sizeof(double),1,fp);
	  fread(&cut_lj[i][j],sizeof(double),1,fp);
	}
	MPI_Bcast(&epsilon[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&sigma[i][j],1,MPI_DOUBLE,0,world);
	MPI_Bcast(&cut_lj[i][j],1,MPI_DOUBLE,0,world);
      }
    }
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::write_restart_settings(FILE *fp)
{
  fwrite(&cut_lj_global,sizeof(double),1,fp);
  fwrite(&cut_coul,sizeof(double),1,fp);
  fwrite(&offset_flag,sizeof(int),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_lj_global,sizeof(double),1,fp);
    fread(&cut_coul,sizeof(double),1,fp);
    fread(&offset_flag,sizeof(int),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_lj_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&cut_coul,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&offset_flag,1,MPI_INT,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world);
}

/* ----------------------------------------------------------------------
   free memory for tables used in pair computations
------------------------------------------------------------------------- */

void PairLJCutCoulLongOMP::free_tables()
{
  memory->sfree(rtable);
  memory->sfree(drtable);
  memory->sfree(ftable);
  memory->sfree(dftable);
  memory->sfree(ctable);
  memory->sfree(dctable);
  memory->sfree(etable);
  memory->sfree(detable);
  memory->sfree(vtable);
  memory->sfree(dvtable);
  memory->sfree(ptable);
  memory->sfree(dptable);
}

/* ---------------------------------------------------------------------- */

double PairLJCutCoulLongOMP::single(int i, int j, int itype, int jtype,
				 double rsq,
				 double factor_coul, double factor_lj,
				 double &fforce)
{
  double r2inv,r6inv,r,grij,expm2,t,erfc,prefactor;
  double fraction,table,forcecoul,forcelj,phicoul,philj;
  int itable;

  r2inv = 1.0/rsq;
  if (rsq < cut_coulsq) {
    if (!ncoultablebits || rsq <= tabinnersq) {
      r = sqrt(rsq);
      grij = g_ewald * r;
      expm2 = exp(-grij*grij);
      t = 1.0 / (1.0 + EWALD_P*grij);
      erfc = t * (A1+t*(A2+t*(A3+t*(A4+t*A5)))) * expm2;
      prefactor = force->qqrd2e * atom->q[i]*atom->q[j]/r;
      forcecoul = prefactor * (erfc + EWALD_F*grij*expm2);
      if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*prefactor;
    } else {
      union_int_float_t rsq_lookup_single;
      rsq_lookup_single.f = rsq;
      itable = rsq_lookup_single.i & ncoulmask;
      itable >>= ncoulshiftbits;
      fraction = (rsq_lookup_single.f - rtable[itable]) * drtable[itable];
      table = ftable[itable] + fraction*dftable[itable];
      forcecoul = atom->q[i]*atom->q[j] * table;
      if (factor_coul < 1.0) {
	table = ctable[itable] + fraction*dctable[itable];
	prefactor = atom->q[i]*atom->q[j] * table;
	forcecoul -= (1.0-factor_coul)*prefactor;
      }
    }
  } else forcecoul = 0.0;

  if (rsq < cut_ljsq[itype][jtype]) {
    r6inv = r2inv*r2inv*r2inv;
    forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
  } else forcelj = 0.0;

  fforce = (forcecoul + factor_lj*forcelj) * r2inv;

  double eng = 0.0;
  if (rsq < cut_coulsq) {
    if (!ncoultablebits || rsq <= tabinnersq)
      phicoul = prefactor*erfc;
    else {
      table = etable[itable] + fraction*detable[itable];
      phicoul = atom->q[i]*atom->q[j] * table;
    }
    if (factor_coul < 1.0) phicoul -= (1.0-factor_coul)*prefactor;
    eng += phicoul;
  }

  if (rsq < cut_ljsq[itype][jtype]) {
    philj = r6inv*(lj3[itype][jtype]*r6inv-lj4[itype][jtype]) -
      offset[itype][jtype];
    eng += factor_lj*philj;
  }

  return eng;
}

/* ---------------------------------------------------------------------- */

void *PairLJCutCoulLongOMP::extract(char *str)
{
  if (strcmp(str,"cut_coul") == 0) return (void *) &cut_coul;
  return NULL;
}

/* ---------------------------------------------------------------------- */

double PairLJCutCoulLongOMP::memory_usage()
{
  const int n=atom->ntypes;

  double bytes = PairOMP::memory_usage();

  bytes += 9*((n+1)*(n+1) * sizeof(double) + (n+1)*sizeof(double *));
  bytes += 1*((n+1)*(n+1) * sizeof(int) + (n+1)*sizeof(int *));

  return bytes;
}
