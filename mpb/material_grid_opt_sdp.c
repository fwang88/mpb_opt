/* Copyright (C) 1999, 2000, 2001, 2002, Massachusetts Institute of Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* optimization using SDP */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "config.h"
#include <mpiglue.h>
#include <mpi_utils.h>
#include <check.h>

#include <scalar.h>

#include "mpb.h"

#ifdef HAVE_SDP_DECLARATIONS_H
# include <sdp/declarations.h>
#endif

#ifdef HAVE_MOSEK_H
#  include <mosek.h>    /* Include the MOSEK definition file.  */
#endif

#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
typedef int bool;
#define true 1
#define false 0
#define NUM_THREADS 1
#define optRank 0


/**************************************************************************/
#ifdef HAVE_MOSEK_H
static void MSKAPI printstr(void *handle,
                            MSKCONST char str[])
{
  mpi_one_printf("%s",str);
} /* printstr */
#endif

/**************************************************************************/
/* res = v1'*v2 */
static scalar_complex cvector_inproduct(const scalar_complex *cv1, const double *v2, int len)
{
  int i;
  scalar_complex res;
  CASSIGN_SCALAR(res,0.0,0.0);
  
  for (i = 0; i < len; ++i) {
    res.re += cv1[i].re*v2[i];
    res.im += cv1[i].im*v2[i];
  }
  return res;
  
}

static scalar_complex cvector_inproductT(const scalar_complex *cv1, const double *v2, int len, int stride)
{
  int i;
  scalar_complex res;
  CASSIGN_SCALAR(res,0.0,0.0);
  
  for (i = 0; i < len; ++i) {
    res.re += cv1[i*stride].re*v2[i];
    res.im += cv1[i*stride].im*v2[i];
  }
  return res;
  
}


static void get_Dfield(int which_band,scalar_complex *cfield,int cur_num_bands,real scale)
{
  int i, N;
  maxwell_compute_d_from_H(mdata, H, cfield, which_band-1, cur_num_bands);
  N = mdata->fft_output_size;
  /*Dfield/Efield in position space, hence dimension = fft_output_size*3 = (nx*ny*nz)*3*/
  for (i = 0; i < 3*N; ++i) {
    cfield[i].re *= scale;
    cfield[i].im *= scale;
  }
}

static void get_Efield_from_Dfield1(const scalar_complex *dfield, scalar_complex *efield, int cur_num_bands)
{
  
  int i,b;
  
  /*Dfield/Efield in position space, hence dimension = fft_output_size*3*/
  int N = mdata->fft_output_size;
  
  for (i = 0; i < N; ++i) {
    symmetric_matrix eps_inv = mdata->eps_inv[i];
    for (b = 0; b < cur_num_bands; ++b) {
      int ib = 3 * (i * cur_num_bands + b);
      assign_symmatrix_vector(&efield[ib], eps_inv, &dfield[ib]);
    }
  }
  
}

static void get_Efield_from_Dfield(scalar_complex *cfield, int cur_num_bands)
{
  maxwell_compute_e_from_d(mdata, cfield, cur_num_bands);
}

static void get_Efield(int which_band,scalar_complex *cfield,int cur_num_bands, real scale)
{
  get_Dfield(which_band,cfield,cur_num_bands,scale);
  get_Efield_from_Dfield(cfield,cur_num_bands);
}



static void deps_du(double *v, double scalegrad, const material_grid *grids, int ngrids)
{
  int i, j, k, n1, n2, n3, n_other, n_last, rank, last_dim;
#ifdef HAVE_MPI
  int local_n2, local_y_start, local_n3;
#endif
  real s1, s2, s3, c1, c2, c3;
  
  int ntot = material_grids_ntot(grids, ngrids);
  
  n1 = mdata->nx; n2 = mdata->ny; n3 = mdata->nz;
  n_other = mdata->other_dims;
  n_last = mdata->last_dim_size / (sizeof(scalar_complex)/sizeof(scalar));
  last_dim = mdata->last_dim;
  rank = (n3 == 1) ? (n2 == 1 ? 1 : 2) : 3;
  
  s1 = geometry_lattice.size.x / n1;
  s2 = geometry_lattice.size.y / n2;
  s3 = geometry_lattice.size.z / n3;
  c1 = n1 <= 1 ? 0 : geometry_lattice.size.x * 0.5;
  c2 = n2 <= 1 ? 0 : geometry_lattice.size.y * 0.5;
  c3 = n3 <= 1 ? 0 : geometry_lattice.size.z * 0.5;
  
  /* Here we have different loops over the coordinates, depending
     upon whether we are using complex or real and serial or
     parallel transforms.  Each loop must define, in its body,
     variables (i2,j2,k2) describing the coordinate of the current
     point, and "index" describing the corresponding index in
     the curfield array.
     
     This was all stolen from fields.c...it would be better
     if we didn't have to cut and paste, sigh. */
  
#ifdef SCALAR_COMPLEX
  
#  ifndef HAVE_MPI
  
  for (i = 0; i < n1; ++i)
    for (j = 0; j < n2; ++j)
      for (k = 0; k < n3; ++k)
        {
          int i2 = i, j2 = j, k2 = k;
          int index = ((i * n2 + j) * n3 + k);
        
#  else /* HAVE_MPI */
          
  local_n2 = mdata->local_ny;
  local_y_start = mdata->local_y_start;
  
  /* first two dimensions are transposed in MPI output: */
  for (j = 0; j < local_n2; ++j)
    for (i = 0; i < n1; ++i)
      for (k = 0; k < n3; ++k)
        {
          int i2 = i, j2 = j + local_y_start, k2 = k;
          int index = ((j * n1 + i) * n3 + k);
        
#  endif /* HAVE_MPI */
  
#else /* not SCALAR_COMPLEX */
  
#  ifndef HAVE_MPI
  
  for (i = 0; i < n_other; ++i)
    for (j = 0; j < n_last; ++j)
      {
        int index = i * n_last + j;
        int i2, j2, k2;
        switch (rank) {
        case 2: i2 = i; j2 = j; k2 = 0; break;
        case 3: i2 = i / n2; j2 = i % n2; k2 = j; break;
        default: i2 = j; j2 = k2 = 0;  break;
        }
        
#  else /* HAVE_MPI */
        
        local_n2 = mdata->local_ny;
        local_y_start = mdata->local_y_start;
        
        /* For a real->complex transform, the last dimension is cut in
           half.  For a 2d transform, this is taken into account in local_ny
           already, but for a 3d transform we must compute the new n3: */
        if (n3 > 1)
          local_n3 = mdata->last_dim_size / 2;
        else
          local_n3 = 1;
        
        /* first two dimensions are transposed in MPI output: */
        for (j = 0; j < local_n2; ++j)
          for (i = 0; i < n1; ++i)
            for (k = 0; k < local_n3; ++k)
              {
#         define i2 i
                int j2 = j + local_y_start;
#         define k2 k
                int index = ((j * n1 + i) * local_n3 + k);
              
#  endif /* HAVE_MPI */
        
#endif /* not SCALAR_COMPLEX */
        
        {
          vector3 p;
          
          p.x = i2 * s1 - c1; p.y = j2 * s2 - c2; p.z = k2 * s3 - c3;
          
          material_grids_addgradient_point(
                                           v + index*ntot, p, scalegrad, grids,ngrids);
          
#ifndef SCALAR_COMPLEX
          {
            int last_index;
#  ifdef HAVE_MPI
            if (n3 == 1)
              last_index = j + local_y_start;
            else
              last_index = k;
#  else
            last_index = j;
#  endif
            
            if (last_index != 0 && 2*last_index != last_dim) {
              int i2c, j2c, k2c;
              i2c = i2 ? (n1 - i2) : 0;
              j2c = j2 ? (n2 - j2) : 0;
              k2c = k2 ? (n3 - k2) : 0;
              p.x = i2c * s1 - c1;
              p.y = j2c * s2 - c2;
              p.z = k2c * s3 - c3;
              
              material_grids_addgradient_point(
                                               v+index*ntot, p, scalegrad, grids,ngrids);
            }
          }
#endif /* !SCALAR_COMPLEX */
          
        }
        
      }
}
/*************/
static scalar_complex compute_fields_energy(scalar_complex *field1, scalar_complex *field2, bool update)
{
  int i, N, last_dim, last_dim_stored, nx, nz, local_y_start;
  scalar_complex energy_sum;
  scalar_complex *product = (scalar_complex *) field1;
  
  last_dim = mdata->last_dim;
  last_dim_stored =
    mdata->last_dim_size / (sizeof(scalar_complex)/sizeof(scalar));
  nx = mdata->nx; nz = mdata->nz; local_y_start = mdata->local_y_start;
  
  energy_sum.re = 0;
  energy_sum.im = 0;
  for (i = 0; i < mdata->fft_output_size; ++i) {
    scalar_complex field[3];
    real comp_sqr = 0;
    real comp_sqri = 0;
    
    field[0] =   field1[3*i];
    field[1] = field1[3*i+1];
    field[2] = field1[3*i+2];
    
    comp_sqr += field[0].re *   field2[3*i].re + field[0].im *   field2[3*i].im;
    comp_sqr += field[1].re * field2[3*i+1].re + field[1].im * field2[3*i+1].im;
    comp_sqr += field[2].re * field2[3*i+2].re + field[2].im * field2[3*i+2].im;
    
    comp_sqri += field[0].re *   field2[3*i].im - field[0].im *   field2[3*i].re;
    comp_sqri += field[1].re * field2[3*i+1].im - field[1].im * field2[3*i+1].re;
    comp_sqri += field[2].re * field2[3*i+2].im - field[2].im * field2[3*i+2].re;
    
    /* Note: here, we write to product[i]; this is
       safe, even though product is aliased to field1,
       since product[i] is guaranteed to come at or before
       field1[i] (which we are now done with). */
    
    energy_sum.re += comp_sqr;
    energy_sum.im += comp_sqri;
    if (update == true)
      CASSIGN_SCALAR(product[i],comp_sqr,comp_sqri);
    
    
    
#ifndef SCALAR_COMPLEX
        /* most points need to be counted twice, by rfftw output symmetry: */
        {
          int last_index;
#  ifdef HAVE_MPI
          if (nz == 1) /* 2d calculation: 1st dim. is truncated one */
            last_index = i / nx + local_y_start;
          else
            last_index = i % last_dim_stored;
#  else
          last_index = i % last_dim_stored;
#  endif /*HAVE_MPI*/
          if (last_index != 0 && 2*last_index != last_dim) {
            energy_sum.re += comp_sqr;
            energy_sum.im -= comp_sqri;
          }
        }
#endif
  }
  
  mpi_allreduce_1(&energy_sum.re, real, SCALAR_MPI_TYPE,
                  MPI_SUM, mpb_comm);
  mpi_allreduce_1(&energy_sum.im, real, SCALAR_MPI_TYPE,
                  MPI_SUM, mpb_comm);
  return energy_sum;
}

/********************/

/* returns transposed Asp. Key is in the way Asp is updated (stride = final count, Asp starts from the offset (=current count)) */
static void material_grids_SPt(scalar_complex *Asp, const double *depsdu, const double *u, real scalegrad, int ntot, int band1, int band2, int stride)
{
  
  int i,ui;
  scalar_complex *field1,*field2,A0, Aisum, Aitmpt, fieldsum1;
  int cur_num_bands = 1;
  
  CHECK(band1 <= num_bands && band2 <= num_bands, "reducedA0 called for uncomputed band\n");
  field1 = (scalar_complex *) malloc(sizeof(scalar_complex) * mdata->fft_output_size*3);
  field2 = (scalar_complex *) malloc(sizeof(scalar_complex) * mdata->fft_output_size*3);
  /* Ai = (scalar_complex *) malloc(sizeof(scalar_complex) * ntot); */
  
  /* compute A0: Dfield_1'*Efield_2 */
  if (band1)
    get_Dfield(band1, field1, cur_num_bands, 1.0);
  
  if (band2!=band1)
    get_Efield(band2,field2, cur_num_bands, 1.0);
  else
    get_Efield_from_Dfield1(field1, field2, cur_num_bands);
  
  A0 = compute_fields_energy(field1,field2,false);
  /* A0 has been reduced (mpi_allreduce_1) in the compute_field_energy routine */
  
  scalegrad *= 1.0/H.N;
  CASSIGN_SCALAR(A0,A0.re*scalegrad,A0.im*scalegrad);
  
  /* compute Ai: - Efield_1'*depsdu*Efield_2 */
  if (band1!=band2){
    get_Efield_from_Dfield(field1, cur_num_bands);
  }
  else
    {
      free(field1);
      field1 = field2;
    }
  
  fieldsum1 = compute_fields_energy(field1,field2,true);
  scalegrad *= -1.0;
  
  for (ui = 0; ui < ntot; ++ui)
    {
      CASSIGN_SCALAR(Aitmpt, 0.0,0.0);
      
      for (i = 0; i < mdata->fft_output_size; ++i)
        {
          Aitmpt.re += depsdu[i*ntot+ui]*field1[i].re;
          Aitmpt.im += depsdu[i*ntot+ui]*field1[i].im;
        }
      CASSIGN_SCALAR(Asp[ui*stride],Aitmpt.re*scalegrad,Aitmpt.im*scalegrad);
      
      /* field1, field2, and despdu (of size fft_output_size = nx*local_y*nz) only have the local block of data, and their products 
         should be summed up (mpi_reduce) to account for global info. local_ny ~= ny/mpi_comm_size */
      
      mpi_allreduce_1(&Asp[ui*stride].re, real, SCALAR_MPI_TYPE, MPI_SUM, mpb_comm);
      mpi_allreduce_1(&Asp[ui*stride].im, real, SCALAR_MPI_TYPE, MPI_SUM, mpb_comm);
      
    }
  
  Aisum = cvector_inproductT(Asp, u, ntot, stride);
  Asp[ntot*stride].re =  A0.re - Aisum.re;
  Asp[ntot*stride].im =  A0.im - Aisum.im;

  if(band1 != band2) free(field1);
  free(field2);
}

/**************************************************************************/

/* a quick implementation of the matlab "find" function; 
   "find" returns the indices of v that are strictly positive;
   v is not expected to be long; n is the length of v;
   pos = 1, first occurrence; pos > 1, first pos occurrences;
   pos = -1, last occurrence;
 */
static int find(const double *v, double scalev, double shiftval, int pos, const int n)
{
  int i,idx = -1;
  double val;
  for (i = 0; i < n; ++i)
    {
      val = v[i]*scalev+shiftval;
      if(val>0) {
        idx = i;
        if (pos ==1)
          break;
      }
    }
  if(idx>=0 || (idx < 0 && pos ==-1)){ /* idx = -1; */
    return idx;
  }
  else{ /* can't find anything positive, return the last index */
    return n;
  }
}





static double  detectFluctuation(double *obj,int irun,int maxflucfreq,double usum,double utol)
{ 
  int i,j;
  double *errs,errsum; 
  int backshift = MIN2((irun+1)/2,maxflucfreq);

  CHK_MALLOC(errs, double, maxflucfreq);
  
  if (backshift>0)
    {
      for (j=1; j<=backshift; j++)
        {
          errsum = 0.0;
          for (i=0; i<j; i++)
            {
              errs[i] = fabs((obj[irun-i] - obj[irun-i-j])/obj[irun-i]);
              errsum += errs[i];
            }
	  mpi_one_printf("errsum = %+1.6e, usum = %+1.6e\n",errsum,usum);
          if(errsum/j <= utol)
            {  mpi_one_printf("fluctuation in gap detected, with flucfreq = %d\n",j);
              /* to break the while loop */
              usum = utol/10; 
              break;
            }
        }
    }
  free(errs);
  return usum;
}


/*******************************************************************************/
/* the entire row of Asp has to be transferred to bara_v */
void buildSymmatTriplet(scalar_complex *Asp, double *bara_v, int spdim)
{
  int ridx,cidx,count,count1;
  int stride = spdim*(spdim+1)/2;
  count = 0; count1 = 0;
  for(ridx=0; ridx<spdim; ridx++)
    {
      for(cidx=0; cidx<=ridx; cidx++)
	{
	  bara_v[count] = -Asp[count].re;
	  bara_v[count+stride] = -Asp[count].re;
	  bara_v[count+stride*2] = -Asp[count].im;
	  if (ridx!=cidx){
	    bara_v[count1+stride*3] = Asp[count].im;
	    ++count1;
	  }
	  ++count;
	}
    }
}



void split_k(int nk, int *kidx_start, int *kidx_end) {
  if (mpb_mygroup < (nk % mpb_numgroups)){
    *kidx_start = (nk/mpb_numgroups + 1) * mpb_mygroup;
    *kidx_end = (nk/mpb_numgroups + 1) * (mpb_mygroup+1);
  }
  else {
    *kidx_start = (nk/mpb_numgroups) * mpb_mygroup + nk%mpb_numgroups;
    *kidx_end = (nk/mpb_numgroups) * (mpb_mygroup+1) + nk%mpb_numgroups;
  }
  if (mpb_mygroup == (mpb_numgroups -1))
    *kidx_end = nk;
   
}
 
/*******************************************************************************/
number run_matgrid_optgap_mosek(vector3_list kpoints,
                                integer band1, integer band2,
                                integer maxrun, number utol,
                                number low_tol, number upp_tol,char *title)
{
  int icount, k, ntot, n, ngrids,*nl, *nu,nk,ridx,cidx,rband,cband,count,nltmpt,nutmpt,N,nblocks,stride,maxspdim;
  double *u,*eigenvalues,*Ctemp,*depsdu,lambda_l,lambda_u,recv_lambda,vec[2],usum;
  double freq_gap, omega_l, omega_u;
  material_grid *grids;
  double lowtol = (double) low_tol; /* determines the size of lower subspace */
  double upptol = (double) upp_tol; /* determines the size of upper subspace */
  double scale;
  int irun; /* ,maxrun; */
  scalar_complex A0,Aisum,*Altemp,*Autemp;
  int count1;
  double *gap, *obj;
  int maxflucfreq = 5;
  
#ifndef HAVE_MOSEK
  CHECK(0, "mosek is required for material_grids_optgap\n");
#else
  /**************/
  MSKint32t i, j;
  MSKrescodee  r;
  MSKenv_t     env = NULL;
  MSKtask_t    task = NULL;
  
  MSKint32t aptrb[]  = {0}, aptre[]  = {2}, asub[]   = {0, 1}; /* column subscripts of A */
  MSKint32t *asub2;
  double  a1val[]   = {-1.0, 1.0}, *a2val, a3val[] = {1.0, 1.0};
  
  MSKint32t *bara_i,*bara_j;
  double *bara_v;
  MSKint64t    idx;
  double       falpha = 1.0;
  
  double       *xx, *y;
  double *xc, *slc, *suc, *slx, *sux, *snx;
  
  
  
  MSKint32t       c = 1;
  MSKint32t       qo = 1; 
  MSKint32t       a = 1; 
  MSKint32t       qc = 1; 
  MSKint32t       bc = 1; 
  MSKint32t       bx = 1; 
  MSKint32t       vartype = 1; 
  MSKint32t       cones = 1;
  MSKint32t *barvarList;
  
  /**************/
  
  int rank, sz;
  MPI_Comm_rank(mpb_comm, &rank);
  MPI_Comm_size(mpb_comm, &sz);

  int global_rank, global_size, local_rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &global_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &global_size);
  MPI_Comm_rank(mpb_comm, &local_rank);

  nk = kpoints.num_items;
 
  //  printf("my group: %d, numgroups: %d\n", mpb_mygroup, mpb_numgroups);

  grids = get_material_grids(geometry, &ngrids);

  ntot = material_grids_ntot(grids, ngrids);
  
  /* u = [ubar_1, ubar_2, ... , ubar_ntot, theta, lambda_l, lambda_u] */
  n = ntot + 3;
  mpi_one_printf("number of decision variable n = %d \n",n);
  u = (double *) malloc(sizeof(double) * n);
  
  /* storing the dimensions of the lower and upper subspaces for each k */
  nl = (int *) malloc(sizeof(int) * nk * 2);
  nu = nl + nk;
  
  gap = (double *) malloc(sizeof(double) * maxrun);
  obj = (double *) malloc(sizeof(double) * maxrun);
    
  eigenvalues = (double *) malloc(sizeof(double) * num_bands);
  
  Altemp = (scalar_complex *) malloc(sizeof(scalar_complex) * band1*(band1+1)/2*n);
  Autemp = (scalar_complex *) malloc(sizeof(scalar_complex) * (num_bands-band1)*(num_bands-band1+1)/2*n);
  Ctemp = (double *) calloc((MAX2(band1,num_bands-band2+1)*2)*(MAX2(band1,num_bands-band2+1)*2)+1, sizeof(double));
  
  /* n1 = mdata->nx; n2 = mdata->ny; n3 = mdata->nz; */
  N = mdata->fft_output_size;
  depsdu = (double *) calloc(ntot*N, sizeof(double));
  deps_du(depsdu, 1.0, grids, ngrids);

  mpi_one_printf("nx = %d, ny = %d, nz = %d\n", mdata->nx, mdata->ny, mdata->nz);

  maxspdim = MAX2(band1,num_bands-band2+1)*2;

  int tmp;

  int bara_dim; 
  bara_dim = maxspdim*(maxspdim+1)/2;  

  bara_i = (MSKint32t *) malloc(sizeof(MSKint32t)*bara_dim);
  bara_j = (MSKint32t *) malloc(sizeof(MSKint32t)*bara_dim);
  bara_v = (double *) calloc(bara_dim, sizeof(double));
  
  MSKint32t *baral_i,*baral_j, *barau_i, *barau_j;
  double *baral_v, *barau_v;
  baral_i = (MSKint32t *) malloc(sizeof(MSKint32t)*bara_dim);
  baral_j = (MSKint32t *) malloc(sizeof(MSKint32t)*bara_dim);
  baral_v = (double *) calloc(bara_dim, sizeof(double));
  
  barau_i = (MSKint32t *) malloc(sizeof(MSKint32t)*bara_dim);
  barau_j = (MSKint32t *) malloc(sizeof(MSKint32t)*bara_dim);
  barau_v = (double *) calloc(bara_dim, sizeof(double));
    
  int *recv_nl, *recv_nu, *recv_k;
  int *recv_rank;
  recv_nl = (int *) malloc(sizeof(int) * mpb_numgroups);
  recv_nu = (int *) malloc(sizeof(int) * mpb_numgroups);
  recv_k = (int *) malloc(sizeof(int) * mpb_numgroups);
  recv_rank = (int *) malloc(sizeof(int) * mpb_numgroups);
  
  MPI_Status status;
  
  a2val = (double *) malloc(sizeof(double)*(ntot+1));
  
  asub2 = (MSKint32t *) malloc(sizeof(MSKint32t)*(ntot+1));
  
  const MSKint32t NUMCON = n;
  const MSKint32t NUMVAR = 2*ntot+4;
  const MSKint32t NUMBARVAR = 2*nk;
  barvarList = (MSKint32t *) malloc(sizeof(MSKint32t)*NUMBARVAR);
  
  usum = ntot; irun = 0;

  r = MSK_makeenv(&env,NULL);      

  material_grids_get(u, grids, ngrids);
  MPI_Bcast(u, ntot, MPI_DOUBLE, optRank, MPI_COMM_WORLD);
  material_grids_set(u, grids, ngrids);
  reset_epsilon();

  MPI_Barrier(MPI_COMM_WORLD);
  get_epsilon();
 

  while (irun < maxrun & usum >= utol)
    {
      mpi_one_printf("\n opt step irun = %d \n \n", irun);
      
      /*************************************************/
      if ( r==MSK_RES_OK )
        {
          r = MSK_maketask(env,NUMCON,0,&task);      
      
          MSK_linkfunctotaskstream(task,MSK_STREAM_LOG,NULL,printstr);
          r = MSK_putintparam(task,MSK_IPAR_NUM_THREADS,NUM_THREADS);
	  
          if ( r == MSK_RES_OK )
            r = MSK_appendcons(task,NUMCON);
          if ( r == MSK_RES_OK )
            r = MSK_appendvars(task,NUMVAR);
          if ( r ==MSK_RES_OK )
            r = MSK_putcj(task,NUMVAR-1,1.0);
          
          for (j=0; j<NUMVAR-1 && r==MSK_RES_OK; ++j)
            r = MSK_putvarbound( task,j,MSK_BK_UP,-MSK_INFINITY,0.0);
          r = MSK_putvarbound( task,NUMVAR-1,MSK_BK_FR,-MSK_INFINITY,MSK_INFINITY); 
          /* l^c = u^c = b = [0;0;...0;-2,2]; max: b^t*y = b^t*(s_l^c - s_u^c) */
          for(i=0; i<NUMCON-2 && r==MSK_RES_OK; ++i)
            r = MSK_putconbound(task, i, MSK_BK_FX, 0.0, 0.0);
          r = MSK_putconbound(task, NUMCON-2, MSK_BK_FX, -2.0, -2.0);
          r = MSK_putconbound(task, NUMCON-1, MSK_BK_FX, 2.0, 2.0);

          for (i=0; i<ntot && r==MSK_RES_OK; ++i)
            { 
              asub[0]  = 2*i;
              asub[1] = 2*i+1;
              r = MSK_putarow(task, i, 2, asub, a1val);
            }
          
          for (j=0; j<ntot+1; ++j)
            {
              asub2[j] = 2*j;
              a2val[j] = 1.0;
            }
          if (r== MSK_RES_OK)
            r = MSK_putarow(task, ntot, ntot+1, asub2, a2val);
          
          asub[0] = ntot+1;
          asub[1] = ntot+2;
          if ( r ==MSK_RES_OK )
            r = MSK_putacol(task, 2*ntot+1, 1, asub, a3val);
          
          if ( r ==MSK_RES_OK )
            r = MSK_putacol(task, 2*ntot+2, 1, asub+1, a3val);
          
          if ( r ==MSK_RES_OK )
            r = MSK_putacol(task, 2*ntot+3, 2, asub, a3val);
        }
      /********************************************************/
      
      lambda_l = 0.0;
      lambda_u = 100.0;
      omega_l = 0.0;
      omega_u = 100;

      material_grids_get(u, grids, ngrids);
      
      mpi_one_printf("number of k points is nk = %d \n", nk);

      double t1;      
      
      int kidx_start, kidx_end;

      //      split_k(nk, &kidx_start, &kidx_end);

      for (k = mpb_mygroup; k < nk; k+=mpb_numgroups) {

        randomize_fields();
        solve_kpoint(kpoints.items[k]);

        MPI_Barrier(MPI_COMM_WORLD);
        
        for (j = 0; j < num_bands; ++j)
          eigenvalues[j]  = freqs.items[j]*freqs.items[j];
        
        /* lambda_l and lambda_u updated at each k, only used outside the "for" loop for k */
        lambda_l = MAX2(eigenvalues[band1-1], lambda_l);
        lambda_u = MIN2(eigenvalues[band2-1], lambda_u);
        
        /* find(const double *v, double scalev, double shiftval, int pos, const int n) */
        nltmpt = (band1-1) - find(eigenvalues,-1,eigenvalues[band1-1]*(1-lowtol),-1,num_bands);
        nutmpt = find(eigenvalues,1,-eigenvalues[band2-1]*(1+upptol),1,num_bands) - (band2-1);
        
        nl[k] = nltmpt>=1 ? nltmpt : nltmpt+1;
        nu[k] = nutmpt>=1 ? nutmpt : nutmpt+1;
        
        mpi_one_printf("nl[%d] = %d, nu[%d] = %d\n",k,nl[k],k,nu[k]);
        /* then send nl[k] to global rank = 0 */

        //        MPI_Barrier(MPI_COMM_WORLD);
        mpi_one_printf("\n start of comm ..\n");

        if(global_rank == 0) {
          asub[0] = 2*nl[k];
          r = MSK_appendbarvars(task, 1, asub);
          asub[0] = 2*nu[k];
          r = MSK_appendbarvars(task, 1, asub);
        }
        
        for(i = 1; i < mpb_numgroups; i++) {

          if (mpb_mygroup == i && local_rank == 0) {
            mpi_one_printf("start send from rank %d\n", i);
            MPI_Send(&nl[k], 1, MPI_INT, 0, 1, MPI_COMM_WORLD);
            MPI_Send(&nu[k], 1, MPI_INT, 0, 2, MPI_COMM_WORLD);
            MPI_Send(&k, 1, MPI_INT, 0, 3, MPI_COMM_WORLD);
            MPI_Send(&lambda_l, 1, MPI_DOUBLE, 0, 4, MPI_COMM_WORLD);
            MPI_Send(&lambda_u, 1, MPI_DOUBLE, 0, 5, MPI_COMM_WORLD);
            mpi_one_printf("end send from rank %d\n", i);
          }
          
          if (global_rank == 0){
            if( i < global_size % mpb_numgroups)
              recv_rank[i] = global_size/mpb_numgroups * i + i;
            else
              recv_rank[i] = global_size/mpb_numgroups * i + global_size % mpb_numgroups;
            mpi_one_printf("start recv from rank %d\n", recv_rank[i]);
            MPI_Recv(&recv_nl[i], 1, MPI_INT, recv_rank[i], 1, MPI_COMM_WORLD, &status);
            MPI_Recv(&recv_nu[i], 1, MPI_INT, recv_rank[i], 2, MPI_COMM_WORLD, &status);
            MPI_Recv(&recv_k[i], 1, MPI_INT, recv_rank[i], 3, MPI_COMM_WORLD, &status);
            MPI_Recv(&recv_lambda, 1, MPI_DOUBLE, recv_rank[i], 4, MPI_COMM_WORLD, &status);
            lambda_l = MAX2(lambda_l, recv_lambda);
            MPI_Recv(&recv_lambda, 1, MPI_DOUBLE, recv_rank[i], 5, MPI_COMM_WORLD, &status);
            lambda_u = MIN2(lambda_u, recv_lambda);

            tmp = recv_nl[i] * 2;
            r = MSK_appendbarvars(task, 1, &tmp);
            tmp = recv_nu[i] * 2;
            r = MSK_appendbarvars(task, 1, &tmp);
            mpi_one_printf("end recv from rank %d\n", i);
          }
          //          MPI_Barrier(MPI_COMM_WORLD);
        }
        
        mpi_one_printf("\n end of comm ..\n");

        /* fill the reduced gradient matrices into a temporary block diagonal matrix, */
        /* except the vectorized blocks are arranged side by side: [vec[block1] vec[block2] ... vec[blockn], vec[block1]*u[1] + ...+ vec[blockn]*u[n]],  */
        /* instead of diagonally. */
        
        scale = 1;
        count = 0; count1 = 0;
        stride = (nl[k]+1)*nl[k]/2;

        /* this for loop takes much time*/
        for (ridx = 0; ridx < nl[k]; ++ridx)
          {
            rband = band1-nl[k]+1+ridx;
            for (cidx = 0; cidx <= ridx; ++cidx)
              {

                cband = band1-nl[k]+1+cidx;
                material_grids_SPt(Altemp+count, depsdu, u, -scale, ntot, rband, cband, stride);
                CASSIGN_SCALAR(Altemp[count+(n-2)*stride],(rband==cband)? 1:0.0, 0.0);
                CASSIGN_SCALAR(Altemp[count+(n-1)*stride], 0.0, 0.0);

                bara_i[count] = ridx; 
                bara_j[count] = cidx;
                bara_i[count+stride] = ridx+nl[k];
                bara_j[count+stride] = cidx+nl[k];
                bara_i[count+stride*2] = ridx+nl[k]; 
                bara_j[count+stride*2] = cidx;

                if (ridx!=cidx){
                  bara_i[count1+stride*3] = cidx+nl[k]; 
                  bara_j[count1+stride*3] = ridx;
                  ++count1;
                }
                ++count;
              }
          }

        for(j=0; j<n; ++j) {
            buildSymmatTriplet(Altemp+stride*j, bara_v, nl[k]);
            /* first append baraij in rank 0*/
            if(global_rank==0) {
              r = MSK_appendsparsesymmat(task, 2*nl[k], nl[k]*(2*nl[k]+1), bara_i, bara_j, bara_v, &idx);
              r = MSK_putbaraij(task, j, 2*k, 1, &idx, &falpha);
            }

            /* send bara_i, bara_j, bara_v, and fill up nl part */
            
            for(i = 1; i < mpb_numgroups; i++) {
              if (mpb_mygroup == i && local_rank == 0) {
                MPI_Send(bara_i, bara_dim, MPI_INT, 0, 1, MPI_COMM_WORLD);
                MPI_Send(bara_j, bara_dim, MPI_INT, 0, 2, MPI_COMM_WORLD);
                MPI_Send(bara_v, bara_dim, MPI_DOUBLE, 0, 3, MPI_COMM_WORLD);
              }
              
              if (global_rank == 0){                
                MPI_Recv(baral_i, bara_dim, MPI_INT, recv_rank[i], 1, MPI_COMM_WORLD, &status);           
                MPI_Recv(baral_j, bara_dim, MPI_INT, recv_rank[i], 2, MPI_COMM_WORLD, &status);       
                MPI_Recv(baral_v, bara_dim, MPI_DOUBLE, recv_rank[i], 3, MPI_COMM_WORLD, &status);

                r = MSK_appendsparsesymmat(task, 2*recv_nl[i], recv_nl[i]*(2*recv_nl[i]+1), baral_i, baral_j, baral_v, &idx);
                r = MSK_putbaraij(task, j, 2*recv_k[i], 1, &idx, &falpha);
              }
              MPI_Barrier(MPI_COMM_WORLD);
            }
            
        }

        /*pass nl[k] value to proc 0; appendbarvars in proc 0; pass bara_i, bara_j, bara_v to proc 0; append spase matrix and put bar aij in proc 0*/
      
        count = 0; count1 = 0;
        stride = (nu[k]+1)*nu[k]/2;
        for (ridx = 0; ridx < nu[k]; ++ridx)
          {
            rband = band2+ridx;
            for (cidx = 0; cidx <= ridx; ++cidx)
              {
                cband = band2+cidx;
                
                material_grids_SPt(Autemp+count, depsdu, u, scale, ntot, rband, cband, stride);
                
                CASSIGN_SCALAR(Autemp[count+stride*(n-2)], 0.0, 0.0);
                CASSIGN_SCALAR(Autemp[count+stride*(n-1)],(rband==cband)? -1:0.0, 0.0);
                
                bara_i[count] = ridx; bara_j[count] = cidx;
                bara_i[count+stride] = ridx+nu[k]; bara_j[count+stride] = cidx+nu[k];
                bara_i[count+stride*2] = ridx+nu[k]; bara_j[count+stride*2] = cidx;
                if (ridx!=cidx)
                  {
                    bara_i[count1+stride*3] = cidx+nu[k]; bara_j[count1+stride*3] = ridx;
                    ++count1;
                  }
                ++count;
              }
          }
        mpi_one_printf("finishing put bar aij\n");
        for(j=0; j<n; ++j) {
          buildSymmatTriplet(Autemp+stride*j, bara_v, nu[k]);
          
          if(global_rank==0) {
            r = MSK_appendsparsesymmat(task, 2*nu[k], nu[k]*(2*nu[k]+1), bara_i, bara_j, bara_v, &idx);
            r = MSK_putbaraij(task, j, 2*k+1, 1, &idx, &falpha);
          }

          for(i = 1; i < mpb_numgroups; i++) {
            if (mpb_mygroup == i && local_rank == 0) {
              MPI_Send(bara_i, bara_dim, MPI_INT, 0, 1, MPI_COMM_WORLD);
              MPI_Send(bara_j, bara_dim, MPI_INT, 0, 2, MPI_COMM_WORLD);
              MPI_Send(bara_v, bara_dim, MPI_DOUBLE, 0, 3, MPI_COMM_WORLD);
            }

            if (global_rank == 0){
              MPI_Recv(barau_i, bara_dim, MPI_INT, recv_rank[i], 1, MPI_COMM_WORLD, &status);           
              MPI_Recv(barau_j, bara_dim, MPI_INT, recv_rank[i], 2, MPI_COMM_WORLD, &status);       
              MPI_Recv(barau_v, bara_dim, MPI_DOUBLE, recv_rank[i], 3, MPI_COMM_WORLD, &status);

              r = MSK_appendsparsesymmat(task, 2*recv_nu[i], recv_nu[i]*(2*recv_nu[i]+1), barau_i, barau_j, barau_v, &idx);
              r = MSK_putbaraij(task, j, 2*recv_k[i] + 1, 1, &idx, &falpha);
            }

            MPI_Barrier(MPI_COMM_WORLD);
          }
        }
      }

      gap[irun] = 2*(lambda_u-lambda_l)/(lambda_l+lambda_u);
      omega_l = sqrt(lambda_l);
      omega_u = sqrt(lambda_u);

      if(mpb_mygroup == 0)
        // mpi_one_printf("Before maximization, %d, gap is %0.15g \n",irun+1, gap[irun]);

      freq_gap = 2*(omega_u-omega_l)/(omega_l+omega_u);
      if(mpb_mygroup == 0)
        mpi_one_printf("Before maximization, %d, freq_gap is %0.15g \n",irun+1, freq_gap);

      if ( r==MSK_RES_OK && global_rank == 0 )
        {
          MSKrescodee trmcode;
          mpi_one_printf("start msk optimitization\n");

          /* Run optimizer */
          r = MSK_optimizetrm(task,&trmcode);

          mpi_one_printf("msk optimitization done\n");
          MSK_solutionsummary (task,MSK_STREAM_MSG);

          if ( r==MSK_RES_OK )
            {
	      MSKsolstae solsta;
              MSK_getsolsta (task,MSK_SOL_ITR,&solsta);
              
              switch(solsta)
                {
                case MSK_SOL_STA_OPTIMAL:
                case MSK_SOL_STA_NEAR_OPTIMAL:
                  y   = (double*) MSK_calloctask(task,n,sizeof(MSKrealt));

                  MSK_gety(task, MSK_SOL_ITR, y);
                  MSK_getdualobj (task, MSK_SOL_ITR, obj+irun);

                  usum = 0.0;
                  for(j=0; j<ntot; j++){
                    usum += fabs(u[j]-y[j]/y[ntot]);
                    u[j] = (double) y[j]/y[ntot];
                  }
                  
                  usum /= ntot;
                  mpi_one_printf("After maximization, %d, objective is %0.15g, change_in_u = %g\n",irun+1, obj[irun], usum);
                  usum = detectFluctuation(obj,irun,maxflucfreq,usum,utol);
		  
                  /* detect flunctuation */	
                  MSK_freetask(task,y);
                  
                  break;
                case MSK_SOL_STA_DUAL_INFEAS_CER:
                case MSK_SOL_STA_PRIM_INFEAS_CER:
                case MSK_SOL_STA_NEAR_DUAL_INFEAS_CER:
                case MSK_SOL_STA_NEAR_PRIM_INFEAS_CER:
                  mpi_one_printf("Primal or dual infeasibility certificate found.\n");
                  break;
                  
                case MSK_SOL_STA_UNKNOWN:
                  mpi_one_printf("The status of the solution could not be determined.\n");
                  break;
                default:
                  mpi_one_printf("Other solution status.");
                  break;
                }
            }
          else
            mpi_one_printf("Error while optimizing.\n");     

          mpi_one_printf("about to delete task\n");
          MSK_deletetask(&task);

        }

      mpi_one_printf("broadcasting u\n");
      MPI_Bcast(u, ntot, MPI_DOUBLE, optRank, MPI_COMM_WORLD);
      mpi_one_printf("broadcasting usum\n");
      MPI_Bcast(&usum, 1, MPI_DOUBLE, optRank, MPI_COMM_WORLD);

      /* update u and epsilon */
      material_grids_set(u, grids, ngrids);
      reset_epsilon();

      /* output epsilon file at each iteration from group 0*/ 
      char prefix[256];       
      snprintf(prefix, 256, "%s%04d-", title, irun+1);
      get_epsilon();
      if(mpb_mygroup == 0) {
        //printf("i am rank %d \n", global_rank);
        output_field_to_file(-1, prefix);
      }
      /* output grid file at each iteration */ 
      strcat(prefix,"grid");
      if(mpb_mygroup == 0)
        save_material_grid(*grids, prefix);
    
      irun++;
      MPI_Barrier(MPI_COMM_WORLD);
    }

  free(nl);
  free(u);
  free(Altemp);
  free(Autemp);
  free(Ctemp);
  free(bara_i);
  free(bara_j);
  free(bara_v);

  free(baral_i);
  free(baral_j);
  free(baral_v);
  free(barau_i);
  free(barau_j);
  free(barau_v);

  free(recv_nl);
  free(recv_nu);
  free(recv_k);

  free(eigenvalues);
  free(depsdu);

  free(gap);
  free(obj);

  free(asub2);
  free(a2val);
  free(barvarList);
  MSK_deleteenv(&env);

#endif
  return 1;
  
}




