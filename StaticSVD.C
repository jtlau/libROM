/******************************************************************************
 *
 * Copyright (c) 2013-2019, Lawrence Livermore National Security, LLC
 * and other libROM project developers. See the top-level COPYRIGHT
 * file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 *
 *****************************************************************************/

// Description: A class implementing interface of SVD for the static SVD
//              algorithm.

#include "StaticSVD.h"
#include "ScalaWRAP/src/LocalMatrix.hpp"

#include "mpi.h"

#include <stdio.h>
#include <string.h>

using namespace ScalaWRAP;

namespace CAROM {

StaticSVD::StaticSVD(
   int dim,
   int samples_per_time_interval,
   int max_basis_dimension,
   double sigma_tolerance,
   bool debug_algorithm) :
   SVD(dim, samples_per_time_interval, debug_algorithm),
   d_samples(nullptr), d_factorization(new SVDInfo()),
   d_this_interval_basis_current(false),
   d_max_basis_dimension(max_basis_dimension),
   d_sigma_tol(sigma_tolerance)
{
   CAROM_ASSERT(dim > 0);
   CAROM_ASSERT(samples_per_time_interval > 0);
   CAROM_ASSERT(max_basis_dimension > 0);
   CAROM_ASSERT(sigma_tolerance >= 0);

   // Get the rank of this process, and the number of processors.
   int mpi_init;
   MPI_Initialized(&mpi_init);
   if (mpi_init == 0) { MPI_Init(nullptr, nullptr); }
   
   MPI_Comm_rank(MPI_COMM_WORLD, &d_rank);
   MPI_Comm_size(MPI_COMM_WORLD, &d_num_procs);
   
   get_global_info();

   /****************************************************************************
    * !!! IMPORTANT !!!
    * The blocksize for the ScaLAPACK matrix set here MUST BE at least
    * d_samples_per_time_interval! This is because we "trick" ScaLAPACK by
    * simply changing the column dimension of the matrix that is stored, but
    * if we do this and the block size is less than the number of columns the
    * data distribution is incorrect.
    ***************************************************************************/
   int blocksize = d_total_dim / d_num_procs + (d_total_dim % d_num_procs != 0);
   blocksize = std::max(blocksize, d_samples_per_time_interval);
   d_samples = std::unique_ptr<ScalaMat>(
           new ScalaMat(d_total_dim, d_samples_per_time_interval, blocksize, blocksize,
                        Context::make_context(d_num_procs, 1))
           );
}

bool
StaticSVD::takeSample(
   double* u_in,
   double time,
   bool add_without_increase)
{
   CAROM_ASSERT(u_in != 0);
   CAROM_ASSERT(time >= 0.0);
   CAROM_NULL_USE(add_without_increase);

   // Check the u_in is not non-zero.
   Vector u_vec(u_in, d_dim, true);
   if (u_vec.norm() == 0.0) {
      return false;
   }

   if (isNewTimeInterval()) {
      // We have a new time interval.
      int num_time_intervals = 
         static_cast<int>(d_time_interval_start_times.size());
      if (num_time_intervals > 0) {
         delete d_basis;
         d_basis = nullptr;
         delete d_basis_right;
         d_basis_right = nullptr;
         delete d_U;
         d_U = nullptr;
         delete d_S;
         d_S = nullptr;
         delete d_W;
         d_W = nullptr;
         delete d_snapshots;
         d_snapshots = nullptr;  
    }
      d_num_samples = 0;
      d_time_interval_start_times.resize(
                static_cast<unsigned>(num_time_intervals) + 1);
      d_time_interval_start_times[static_cast<unsigned>(num_time_intervals)] =
          time;
      d_basis = nullptr;
      d_basis_right = nullptr;
   }
   broadcast_sample(u_in);
   ++d_num_samples;
   
   // Build snapshot matrix before SVD is computed
   d_snapshots = new Matrix(d_dim, d_num_samples, false);
   for (int rank = 0; rank < d_num_procs; ++rank) {
      int nrows = d_dims[static_cast<unsigned>(rank)];
      int firstrow = d_istarts[static_cast<unsigned>(rank)] + 1;
      LocalMatrix(&d_snapshots->item(0,0), nrows, d_num_samples, rank) = 
                  d_samples->submatrix(rowrange(firstrow, firstrow+nrows-1),
                                       colrange(1, d_num_samples));
   }
   d_this_interval_basis_current = false;
   return true;
}

const Matrix*
StaticSVD::getSpatialBasis()
{
   // If this basis is for the last time interval then it may not be up to date
   // so recompute it.
   if (!thisIntervalBasisCurrent()) {
      delete d_basis;
      d_basis = nullptr;
      delete d_basis_right;
      d_basis_right = nullptr;
      delete d_U;
      d_U = nullptr;
      delete d_S;
      d_S = nullptr;
      delete d_W;
      d_W = nullptr;
      computeSVD();
   }
   else {
      CAROM_ASSERT(d_basis != 0);
   }
   CAROM_ASSERT(thisIntervalBasisCurrent());
   return d_basis;
}

const Matrix*
StaticSVD::getTemporalBasis()
{
   // If this basis is for the last time interval then it may not be up to date
   // so recompute it.
   if (!thisIntervalBasisCurrent()) {
      delete d_basis;
      d_basis = nullptr;
      delete d_basis_right;
      d_basis_right = nullptr;
      delete d_U;
      d_U = nullptr;
      delete d_S;
      d_S = nullptr;
      delete d_W;
      d_W = nullptr;
      computeSVD();
   }
   else {
      CAROM_ASSERT(d_basis_right != 0);
   }
   CAROM_ASSERT(thisIntervalBasisCurrent());
   return d_basis_right;
}

const Matrix*
StaticSVD::getSingularValues()
{
   // If these singular values are for the last time interval then they may not
   // be up to date so recompute them.
   if (!thisIntervalBasisCurrent()) {
      delete d_basis;
      d_basis = nullptr;
      delete d_basis_right;
      d_basis = nullptr;
      delete d_U;
      d_U = nullptr;
      delete d_S;
      d_S = nullptr;
      delete d_W;
      d_W = nullptr;
      computeSVD();
   }
   else {
      CAROM_ASSERT(d_S != 0);
   }
   CAROM_ASSERT(thisIntervalBasisCurrent());
   return d_S;
}
   
const Matrix*
StaticSVD::getSnapshotMatrix()
{
   CAROM_ASSERT(d_snapshots != 0);
   return d_snapshots;
}
   
void
StaticSVD::computeSVD()
{
   // This block does the actual ScaLAPACK call to do the factorization.

   // Construct the sample matrix reusing the same data, but marking the number
   // of columns as only the number of samples that we actually collected. This
   // is valid because we set the blocksize to at least the maximum number of
   // samples.
   ScalaMat Samp(d_samples->data(), d_samples->m(), d_num_samples,
                 d_samples->mb(), d_samples->nb(), d_samples->context(),
                 d_samples->rowsrc(), d_samples->colsrc());
   *d_factorization = svd(Samp);

   // Compute how many basis vectors we will actually return.
   int sigma_cutoff = 0, hard_cutoff = d_num_samples;
   if (d_sigma_tol == 0) { 
      sigma_cutoff = std::numeric_limits<int>::max();
   } else {
      for (int i = 0; i < d_num_samples; ++i) {
         if (d_factorization->S[i] / d_factorization->S[0] > d_sigma_tol) {
            sigma_cutoff += 1;
         } else {
            break;
         }
      }
   }
   if (d_max_basis_dimension < hard_cutoff) {
      hard_cutoff = d_max_basis_dimension;
   }
   int ncolumns = hard_cutoff < sigma_cutoff ? hard_cutoff : sigma_cutoff;
   CAROM_ASSERT(ncolumns >= 0);

   // Allocate the appropriate matrices and gather their elements.
   d_basis = new Matrix(d_dim, ncolumns, true);
   d_S = new Matrix(ncolumns, ncolumns, false);
   {
      CAROM_ASSERT(ncolumns >= 0);
      unsigned nc = static_cast<unsigned>(ncolumns);
      memset(&d_S->item(0, 0), 0, nc*nc*sizeof(double));
   }
   
   d_basis_right = new Matrix(d_num_samples, ncolumns, false);
   for (int rank = 0; rank < d_num_procs; ++rank) {
      int nrows = d_dims[static_cast<unsigned>(rank)];
      int firstrow = d_istarts[static_cast<unsigned>(rank)] + 1;

       LocalMatrix(&d_basis->item(0, 0), nrows, ncolumns, rank) =
          d_factorization->U->submatrix(rowrange(firstrow, firstrow+nrows-1),
                                     colrange(1, ncolumns));
       
       // Specifying that the matrix is stored in column major order results in
       // no transposition being applied, which is what we want
       LocalMatrix(&d_basis_right->item(0, 0), ncolumns, d_num_samples, rank,
                   false, COL_MAJOR) =
       d_factorization->Vt->submatrix(rowrange(1, ncolumns),
                                      colrange(1, d_num_samples));
   }
   for (int i = 0; i < ncolumns; ++i)
      d_S->item(i, i) = d_factorization->S[static_cast<unsigned>(i)];
   d_this_interval_basis_current = true;
}

void
StaticSVD::broadcast_sample(const double* u_in)
{
   for (int rank = 0; rank < d_num_procs; ++rank) {
      int firstrow = d_istarts[static_cast<unsigned>(rank)]+1;
      int lastrow = firstrow + d_dims[static_cast<unsigned>(rank)] - 1;
      int col = d_num_samples + 1;

      d_samples->submatrix(rowrange(firstrow, lastrow), colrange(col, col)) =
          LocalMatrix(const_cast<double*>(u_in), lastrow - firstrow + 1, 1, rank, 
                      false, COL_MAJOR);
   }
}

void
StaticSVD::get_global_info()
{
   d_dims.resize(static_cast<unsigned>(d_num_procs));
   MPI_Allgather(&d_dim, 1, MPI_INT, d_dims.data(), 1, MPI_INT, MPI_COMM_WORLD);
   d_total_dim = 0;
   d_istarts = std::vector<int>(static_cast<unsigned>(d_num_procs), 0);
   
   for (unsigned i = 0; i < static_cast<unsigned>(d_num_procs); ++i) {
      d_total_dim += d_dims[i];
      if (i > 0) {
         d_istarts[i] = d_istarts[i-1] + d_dims[i-1];
      }
   }
}

}
