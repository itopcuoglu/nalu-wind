#include "matrix_free/FilterJacobi.h"

#include "Teuchos_RCP.hpp"

#include "Tpetra_Operator.hpp"
#include "matrix_free/FilterDiagonal.h"

#include "matrix_free/PolynomialOrders.h"
#include "matrix_free/KokkosFramework.h"

namespace sierra {
namespace nalu {
namespace matrix_free {

namespace {

using tpetra_view_type = typename Tpetra::MultiVector<>::dual_view_type::t_dev;
using const_tpetra_view_type =
  typename Tpetra::MultiVector<>::dual_view_type::t_dev_const;
void
reciprocal(tpetra_view_type x)
{
  // be brave
  Kokkos::parallel_for(
    "invert", x.extent_int(0), KOKKOS_LAMBDA(int k) { x(k, 0) = 1 / x(k, 0); });
}
} // namespace
template <int p>
FilterJacobiOperator<p>::FilterJacobiOperator(
  const_elem_offset_view<p> elem_offsets_in,
  const export_type& exporter_in,
  int num_sweeps_in)
  : elem_offsets(elem_offsets_in),
    exporter(exporter_in),
    num_sweeps(num_sweeps_in),
    owned_diagonal(exporter.getTargetMap(), num_vectors),
    shared_diagonal(exporter.getSourceMap(), num_vectors),
    cached_mv(exporter.getTargetMap(), num_vectors)
{
}

template <int p>
void
FilterJacobiOperator<p>::set_linear_operator(
  Teuchos::RCP<const Tpetra::Operator<>> op_in)
{
  op = op_in;
}

namespace {
void
element_multiply(
  const_tpetra_view_type inv_diag, const_tpetra_view_type b, tpetra_view_type y)
{
  constexpr int dim = FilterJacobiOperator<inst::P1>::num_vectors;

  Kokkos::parallel_for(
    "element_multiply", b.extent_int(0), KOKKOS_LAMBDA(int index) {
      for (int d = 0; d < dim; ++d) {
        y(index, d) = inv_diag(index, 0) * b(index, d);
      }
    });
}

void
update_jacobi_sweep(
  const_tpetra_view_type inv_diag,
  const_tpetra_view_type axprev,
  const_tpetra_view_type b,
  tpetra_view_type y)
{
  constexpr int dim = FilterJacobiOperator<inst::P1>::num_vectors;
  Kokkos::parallel_for(
    "jacobi_sweep", inv_diag.extent_int(0), KOKKOS_LAMBDA(int index) {
      for (int d = 0; d < dim; ++d) {
        y(index, d) += inv_diag(index, 0) * (b(index, d) - axprev(index, d));
      }
    });
}
} // namespace
template <int p>
void
FilterJacobiOperator<p>::apply(
  const mv_type& x, mv_type& y, Teuchos::ETransp, double, double) const
{
  element_multiply(
    owned_diagonal.getLocalViewDevice(), x.getLocalViewDevice(),
    y.getLocalViewDevice());
  for (int n = 1; n < num_sweeps; ++n) {
    op->apply(y, cached_mv);
    update_jacobi_sweep(
      owned_diagonal.getLocalViewDevice(), cached_mv.getLocalViewDevice(),
      x.getLocalViewDevice(), y.getLocalViewDevice());
  }
  y.modify_device();
}

template <int p>
void
FilterJacobiOperator<p>::compute_diagonal(const_scalar_view<p> vols)
{
  shared_diagonal.putScalar(0.);
  filter_diagonal<p>(elem_offsets, vols, shared_diagonal.getLocalViewDevice());
  shared_diagonal.modify_device();
  owned_diagonal.putScalar(0.);
  owned_diagonal.doExport(shared_diagonal, exporter, Tpetra::ADD);
  reciprocal(owned_diagonal.getLocalViewDevice());
}
INSTANTIATE_POLYCLASS(FilterJacobiOperator);
} // namespace matrix_free
} // namespace nalu
} // namespace sierra