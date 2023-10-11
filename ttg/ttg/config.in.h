//
// Created by Eduard Valeyev on 10/31/22.
//

#ifndef TTG_CONFIG_IN_H
#define TTG_CONFIG_IN_H

/** the C++ header containing the coroutine API */
#define TTG_CXX_COROUTINE_HEADER <@CXX_COROUTINE_HEADER@>

/** the C++ namespace containing the coroutine API */
#define TTG_CXX_COROUTINE_NAMESPACE @CXX_COROUTINE_NAMESPACE@

#cmakedefine TTG_HAVE_CUDA

#cmakedefine TTG_HAVE_CUDART

#cmakedefine TTG_HAVE_HIP

#cmakedefine TTG_HAVE_HIPBLAS

#cmakedefine TTG_HAVE_DEVICE

#cmakedefine TTG_HAVE_MPI
#cmakedefine TTG_HAVE_MPIEXT

#endif  // TTG_CONFIG_IN_H
