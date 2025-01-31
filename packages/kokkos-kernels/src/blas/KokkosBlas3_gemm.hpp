/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 3.0
//       Copyright (2020) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY NTESS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NTESS OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Siva Rajamanickam (srajama@sandia.gov)
//
// ************************************************************************
//@HEADER
*/
#ifndef KOKKOSBLAS3_GEMV_HPP_
#define KOKKOSBLAS3_GEMV_HPP_

/// \file KokkosBlas3_gemm.hpp

#include <KokkosKernels_Macros.hpp>
#include <KokkosBlas3_gemm_spec.hpp>
#include <KokkosBlas2_gemv.hpp>
#include <KokkosBlas1_scal.hpp>
#include <KokkosKernels_helpers.hpp>
#include <sstream>
#include <type_traits>

namespace KokkosBlas {

namespace Impl {
  // Special codepath for when B/C have 1 column: use GEMV (matrix-vector) instead.
  // GEMV performs better than tiled GEMM in this case.
  //
  // Returns true if the criteria are met and GEMV was run, false otherwise.
  //
  // This case must be intercepted here rather than impl in order to call TPL
  // GEMV instead of TPL GEMM. This codepath was measured to be profitable with cuBLAS.
  template<class AViewType,
           class BViewType,
           class CViewType>
  bool
  gemv_based_gemm
       (const char transA[],
        const char transB[],
        typename AViewType::const_value_type& alpha,
        const AViewType& A,
        const BViewType& B,
        typename CViewType::const_value_type& beta,
        const CViewType& C,
        typename std::enable_if<
          !std::is_same<typename BViewType::array_layout, Kokkos::LayoutStride>::value &&
          !std::is_same<typename CViewType::array_layout, Kokkos::LayoutStride>::value>::type* = nullptr)
  {
    if(toupper(transA[0]) == 'N' && toupper(transB[0]) == 'N' && B.extent(1) == size_t(1))
    {
      // since B/C both have a single column and are not LayoutStride,
      // can create a raw contiguous rank-1 vector from them rather than using subview.
      Kokkos::View<typename BViewType::value_type*, typename BViewType::array_layout,
        typename BViewType::device_type, Kokkos::MemoryTraits<Kokkos::Unmanaged>> Bvec(B.data(), B.extent(0));
      Kokkos::View<typename CViewType::value_type*, typename CViewType::array_layout,
        typename CViewType::device_type, Kokkos::MemoryTraits<Kokkos::Unmanaged>> Cvec(C.data(), C.extent(0));
      KokkosBlas::gemv("N", alpha, A, Bvec, beta, Cvec);
      return true;
    }
    return false;
  }

  // Don't attempt to call GEMV with LayoutStride vectors.
  // GEMV is not ETI'd for this case, so there would be undefined symbol errors in tests.
  template<class AViewType,
           class BViewType,
           class CViewType>
  bool
  gemv_based_gemm
       (const char /*transA*/[],
        const char /*transB*/[],
        typename AViewType::const_value_type& /*alpha*/,
        const AViewType& /*A*/,
        const BViewType& /*B*/,
        typename CViewType::const_value_type& /*beta*/,
        const CViewType& /*C*/,
        typename std::enable_if<
          std::is_same<typename BViewType::array_layout, Kokkos::LayoutStride>::value ||
          std::is_same<typename CViewType::array_layout, Kokkos::LayoutStride>::value>::type* = nullptr)
  {
    return false;
  }
}

/// \brief Dense matrix-matrix multiply: C = beta*C + alpha*op(A)*op(B).
///
/// \tparam AViewType Input matrix, as a 2-D Kokkos::View
/// \tparam BViewType Input matrix, as a 2-D Kokkos::View
/// \tparam CViewType Output matrix, as a nonconst 2-D Kokkos::View
///
/// \param transA [in] "N" for non-transpose, "T" for transpose, "C"
///   for conjugate transpose.  All characters after the first are
///   ignored.  This works just like the BLAS routines.
/// \param transB [in] "N" for non-transpose, "T" for transpose, "C"
///   for conjugate transpose.  All characters after the first are
///   ignored.  This works just like the BLAS routines.
/// \param alpha [in] Input coefficient of A*x
/// \param A [in] Input matrix, as a 2-D Kokkos::View
/// \param B [in] Input matrix, as a 2-D Kokkos::View
/// \param beta [in] Input coefficient of C
/// \param C [in/out] Output vector, as a nonconst 2-D Kokkos::View
template<class AViewType,
         class BViewType,
         class CViewType>
void
gemm (const char transA[],
      const char transB[],
      typename AViewType::const_value_type& alpha,
      const AViewType& A,
      const BViewType& B,
      typename CViewType::const_value_type& beta,
      const CViewType& C)
{

  #if (KOKKOSKERNELS_DEBUG_LEVEL > 0)
  static_assert (Kokkos::Impl::is_view<AViewType>::value,
                 "AViewType must be a Kokkos::View.");
  static_assert (Kokkos::Impl::is_view<BViewType>::value,
                 "BViewType must be a Kokkos::View.");
  static_assert (Kokkos::Impl::is_view<CViewType>::value,
                 "CViewType must be a Kokkos::View.");
  static_assert (static_cast<int> (AViewType::rank) == 2,
                 "AViewType must have rank 2.");
  static_assert (static_cast<int> (BViewType::rank) == 2,
                 "BViewType must have rank 2.");
  static_assert (static_cast<int> (CViewType::rank) == 2,
                 "CViewType must have rank 2.");

  // Check validity of transpose argument
  bool valid_transA = (transA[0] == 'N') || (transA[0] == 'n') ||
                      (transA[0] == 'T') || (transA[0] == 't') ||
                      (transA[0] == 'C') || (transA[0] == 'c');
  bool valid_transB = (transB[0] == 'N') || (transB[0] == 'n') ||
                      (transB[0] == 'T') || (transB[0] == 't') ||
                      (transB[0] == 'C') || (transB[0] == 'c');
  if(!(valid_transA && valid_transB)) {
    std::ostringstream os;
    os << "KokkosBlas::gemm: transA[0] = '" << transA[0] << " transB[0] = '" << transB[0] << "'. " <<
      "Valid values include 'N' or 'n' (No transpose), 'T' or 't' (Transpose), "
      "and 'C' or 'c' (Conjugate transpose).";
    Kokkos::Impl::throw_runtime_exception (os.str ());
  }

  // Check compatibility of dimensions at run time.
  bool A_t = !(transA[0] == 'N' || transA[0] == 'n');
  bool B_t = !(transB[0] == 'N' || transB[0] == 'n');
  int64_t A0 = A.extent(0);
  int64_t A1 = A.extent(1);
  // B0 is a `#define`'d constant in 
  // certain MacOSX SDKs in termios.h:291
  int64_t B_0 = B.extent(0);
  int64_t B1 = B.extent(1);
  int64_t C0 = C.extent(0);
  int64_t C1 = C.extent(1);

  if ( ((A_t?A1:A0) != C0) ||
       ((B_t?B_0:B1) != C1) ||
       ((A_t?A0:A1) != (B_t?B1:B_0)) ) {
      std::ostringstream os;
      os << "KokkosBlas::gemm: Dimensions of A, B, and C do not match: "
         << "transA: " << transA[0] << " transB: " << transB[0]
         << " A: " << A.extent(0) << " x " << A.extent(1)
         << " B: " << B.extent(0) << " x " << B.extent(1)
         << " C: " << C.extent(0) << " x " << C.extent(1);
      Kokkos::Impl::throw_runtime_exception (os.str ());
    }
  #endif // KOKKOSKERNELS_DEBUG_LEVEL > 0

  // Return if C matrix is degenerated
  if((C.extent(0) == 0) || (C.extent(1) == 0)) {
    return;
  }

  // Simply scale C if A matrix is degenerated
  if(A.extent(1) == 0) {
    scal(C, beta, C);
    return;
  }

  // Check if gemv code path is allowed and profitable, and if so run it.
  if(Impl::gemv_based_gemm(transA, transB, alpha, A, B, beta, C))
    return;

  // Minimize the number of Impl::GEMM instantiations, by
  // standardizing on particular View specializations for its template
  // parameters.
  typedef Kokkos::View<typename AViewType::const_value_type**,
    typename AViewType::array_layout,
    typename AViewType::device_type,
    Kokkos::MemoryTraits<Kokkos::Unmanaged> > AVT;
  typedef Kokkos::View<typename BViewType::const_value_type**,
    typename BViewType::array_layout,
    typename BViewType::device_type,
    Kokkos::MemoryTraits<Kokkos::Unmanaged> > BVT;
  typedef Kokkos::View<typename CViewType::non_const_value_type**,
    typename CViewType::array_layout,
    typename CViewType::device_type,
    Kokkos::MemoryTraits<Kokkos::Unmanaged> > CVT;
  typedef Impl::GEMM<AVT, BVT, CVT> impl_type;
  impl_type::gemm (transA, transB, alpha, A, B, beta, C);
}

} // namespace KokkosBlas

#endif // KOKKOS_BLAS3_MV_HPP_
