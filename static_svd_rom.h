#ifndef included_static_svd_rom_h
#define included_static_svd_rom_h

#include "static_svd_time_stepper.h"

namespace CAROM {

// A class which implements a Reduced Order Model base on the static svd
// algorithm.  At the present time it pretty much just recasts the API of
// the static_svd_time_stepper in terms of snapshots.  In the future this
// class will likely need to do much more.
class static_svd_rom
{
   public:
      // Constructor.
      static_svd_rom(
         int dim);

      // Destructor.
      ~static_svd_rom();

      // Returns true if it is time for the next svd snapshot.
      bool
      isNextSnapshot(
         double time)
      {
         return d_svdts->isNextStateCollection(time);
      }

      // Add a snapshot to the static svd.
      void
      takeSnapshot(
         double* u_in,
         double time)
      {
         NULL_USE(time);
         d_svdts->collectState(u_in);
      }

      // Computes next time an svd snapshot is needed.
      double
      computeNextSnapshotTime(
         double* u_in,
         double* rhs_in,
         double time)
      {
         return d_svdts->computeNextStateCollectionTime(u_in, rhs_in, time);
      }

      // Returns the model parameters.
      Matrix*
      getModel(
         double time)
      {
         NULL_USE(time);
         return d_svdts->getModel();
      }

   private:
      // Unimplemented default constructor.
      static_svd_rom();

      // Unimplemented copy constructor.
      static_svd_rom(const static_svd_rom& other);

      // Unimplemented assignment operator.
      static_svd_rom&
      operator = (const static_svd_rom& rhs);

      boost::shared_ptr<static_svd_time_stepper> d_svdts;
};

}

#endif
