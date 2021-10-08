#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Eigen/SparseCore>
#ifdef BLOCK_SPARSE_GEMM
#include <btas/features.h>
#ifdef BTAS_IS_USABLE
#include <btas/btas.h>
#include <btas/optimize/contract.h>
#include <btas/util/mohndle.h>
#else  // defined(BTAS_IS_USABLE)
#error "btas/features.h does not define BTAS_IS_USABLE ... broken BTAS?"
#endif  // defined(BTAS_IS_USABLE)
#endif  // defined(BLOCK_SPARSE_GEMM)

#include <sys/time.h>
#include <boost/graph/rmat_graph_generator.hpp>
#if !defined(BLOCK_SPARSE_GEMM)
#include <boost/graph/directed_graph.hpp>
#include <boost/random/linear_congruential.hpp>
#include <unsupported/Eigen/SparseExtra>
#endif

#ifdef BSPMM_HAS_LIBINT
#include <libint2.hpp>
#include <thread>
#endif

#include "ttg.h"

using namespace ttg;

#include "ttg/util/future.h"

#include "ttg/util/bug.h"

#if defined(BLOCK_SPARSE_GEMM)
using blk_t = btas::Tensor<double, btas::DEFAULT::range, btas::mohndle<btas::varray<double>, btas::Handle::shared_ptr>>;

#if defined(TTG_USE_PARSEC)
namespace ttg {
  template <>
  struct SplitMetadataDescriptor<blk_t> {
    // TODO: this is a quick and dirty approach.
    //   - blk_t could have any number of dimensions, this code only works for 2 dim blocks
    //   - we use Blk{} to send a control flow in some tasks below, these blocks have only
    //     1 dimension (of size 0), to code this, we set the second dimension to 0 in our
    //     quick and dirty linearization, then have a case when we create the object
    //   - when we create the object with the metadata, we use a constructor that initializes
    //     the data to 0, which is useless: the data could be left uninitialized
    static auto get_metadata(const blk_t &b) {
      std::pair<int, int> dim{0, 0};
      if (!b.empty()) {
        assert(b.range().extent().size() == 2);
        std::get<0>(dim) = (int)b.range().extent(0);
        std::get<1>(dim) = (int)b.range().extent(1);
      }
      return dim;
    }
    static auto get_data(blk_t &b) {
      if (!b.empty())
        return boost::container::small_vector<iovec, 1>(1, iovec{b.size() * sizeof(double), b.data()});
      else
        return boost::container::small_vector<iovec, 1>{};
    }
    static auto create_from_metadata(const std::pair<int, int> &meta) {
      if (meta != std::pair{0, 0})
        return blk_t(btas::Range(std::get<0>(meta), std::get<1>(meta)), 0.0);
      else
        return blk_t{};
    }
  };
}  // namespace ttg
#endif /* TTG_USE_PARSEC */

// declare btas::Tensor serializable by Boost
#include "ttg/serialization/backends/boost.h"
namespace ttg::detail {
  // BTAS defines all of its Boost serializers in boost::serialization namespace ... as explained in
  // ttg/serialization/boost.h such functions are not detectable via SFINAE, so must explicitly define serialization
  // traits here
  template <typename Archive>
  inline static constexpr bool is_boost_serializable_v<Archive, blk_t> = is_boost_archive_v<Archive>;
  template <typename Archive>
  inline static constexpr bool is_boost_serializable_v<Archive, const blk_t> = is_boost_archive_v<Archive>;
}  // namespace ttg::detail

#else
using blk_t = double;
#endif
template <typename T = blk_t>
using SpMatrix = Eigen::SparseMatrix<T>;
template <typename T = blk_t>
using SpMatrixTriplet = Eigen::Triplet<T>;  // {row,col,value}

#ifdef BLOCK_SPARSE_GEMM

#if __has_include(<madness/world/archive.h>)

#include <madness/world/archive.h>

#endif  // __has_include(<madness/world/archive.h>)

namespace btas {
  template <typename T_, class Range_, class Store_>
  inline btas::Tensor<T_, Range_, Store_> operator*(const btas::Tensor<T_, Range_, Store_> &A,
                                                    const btas::Tensor<T_, Range_, Store_> &B) {
    btas::Tensor<T_, Range_, Store_> C;
    btas::contract(1.0, A, {1, 2}, B, {2, 3}, 0.0, C, {1, 3});
    return C;
  }

  template <typename T_, class Range_, class Store_>
  btas::Tensor<T_, Range_, Store_> gemm(btas::Tensor<T_, Range_, Store_> &&C, const btas::Tensor<T_, Range_, Store_> &A,
                                        const btas::Tensor<T_, Range_, Store_> &B) {
    using array = btas::DEFAULT::index<int>;
    if (C.empty()) {
      C = btas::Tensor<T_, Range_, Store_>(btas::Range(A.range().extent(0), B.range().extent(1)), 0.0);
    }
    btas::contract_222(1.0, A, array{1, 2}, B, array{2, 3}, 1.0, C, array{1, 3}, false, false);
    return std::move(C);
  }
}  // namespace btas
#endif  // defined(BLOCK_SPARSE_GEMM)
double gemm(double C, double A, double B) { return C + A * B; }
/////////////////////////////////////////////

// template <typename _Scalar, int _Options, typename _StorageIndex>
// struct colmajor_layout;
// template <typename _Scalar, typename _StorageIndex>
// struct colmajor_layout<_Scalar, Eigen::ColMajor, _StorageIndex> : public std::true_type {};
// template <typename _Scalar, typename _StorageIndex>
// struct colmajor_layout<_Scalar, Eigen::RowMajor, _StorageIndex> : public std::false_type {};

template <std::size_t Rank>
struct Key : public std::array<long, Rank> {
  static constexpr const long max_index = 1 << 21;
  static constexpr const long max_index_square = max_index * max_index;
  Key() = default;
  template <typename Integer>
  Key(std::initializer_list<Integer> ilist) {
    std::copy(ilist.begin(), ilist.end(), this->begin());
    assert(valid());
  }
  explicit Key(std::size_t hash) {
    static_assert(Rank == 2 || Rank == 3, "Key<Rank>::Key(hash) only implemented for Rank={2,3}");
    if (Rank == 2) {
      (*this)[0] = hash / max_index;
      (*this)[1] = hash % max_index;
    } else if (Rank == 3) {
      (*this)[0] = hash / max_index_square;
      (*this)[1] = (hash % max_index_square) / max_index;
      (*this)[2] = hash % max_index;
    }
  }
  std::size_t hash() const {
    static_assert(Rank == 2 || Rank == 3, "Key<Rank>::hash only implemented for Rank={2,3}");
    return Rank == 2 ? (*this)[0] * max_index + (*this)[1]
                     : ((*this)[0] * max_index + (*this)[1]) * max_index + (*this)[2];
  }

 private:
  bool valid() {
    bool result = true;
    for (auto &idx : *this) {
      result = result && (idx < max_index);
    }
    return result;
  }
};

template <std::size_t Rank>
std::ostream &operator<<(std::ostream &os, const Key<Rank> &key) {
  os << "{";
  for (size_t i = 0; i != Rank; ++i) os << key[i] << (i + 1 != Rank ? "," : "");
  os << "}";
  return os;
}

// block-cyclic map of tile index {i,j} onto the (2d) PxQ grid
// return process rank, obtained as col-major map of the process grid coordinate
inline int tile2rank(int i, int j, int P, int Q) {
  int p = (i % P);
  int q = (j % Q);
  int pq = (q * P) + p;
  return pq;
}

// flow data from an existing SpMatrix on rank 0
template <typename Blk = blk_t, typename Keymap = std::function<int(const Key<2> &)>>
class Read_SpMatrix : public Op<Key<2>, std::tuple<Out<Key<2>, Blk>>, Read_SpMatrix<Blk>, void> {
 public:
  using baseT = Op<Key<2>, std::tuple<Out<Key<2>, Blk>>, Read_SpMatrix<Blk>, void>;

  Read_SpMatrix(const char *label, const SpMatrix<Blk> &matrix, Edge<Key<2>> &ctl, Edge<Key<2>, Blk> &out,
                Keymap &keymap)
      : baseT(edges(ctl), edges(out), std::string("read_spmatrix(") + label + ")", {"ctl"}, {std::string(label) + "ij"},
              keymap)
      , matrix_(matrix) {}

  void op(const Key<2> &key, std::tuple<Out<Key<2>, Blk>> &out) {
    auto rank = ttg_default_execution_context().rank();
    for (int k = 0; k < matrix_.outerSize(); ++k) {
      for (typename SpMatrix<Blk>::InnerIterator it(matrix_, k); it; ++it) {
        if (rank == this->get_keymap()(Key<2>({it.row(), it.col()})))
          ::send<0>(Key<2>({it.row(), it.col()}), it.value(), out);
      }
    }
  }

 private:
  const SpMatrix<Blk> &matrix_;
};

// flow (move?) data into an existing SpMatrix on rank 0
template <typename Blk = blk_t>
class Write_SpMatrix : public Op<Key<2>, std::tuple<>, Write_SpMatrix<Blk>, Blk> {
 public:
  using baseT = Op<Key<2>, std::tuple<>, Write_SpMatrix<Blk>, Blk>;

  template <typename Keymap>
  Write_SpMatrix(SpMatrix<Blk> &matrix, Edge<Key<2>, Blk> &in, Keymap &&keymap)
      : baseT(edges(in), edges(), "write_spmatrix", {"Cij"}, {}, keymap), matrix_(matrix) {}

  void op(const Key<2> &key, typename baseT::input_values_tuple_type &&elem, std::tuple<> &) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (ttg::tracing()) {
      auto &w = get_default_world();
      ttg::print("rank =", w.rank(), "/ thread_id =", reinterpret_cast<std::uintptr_t>(pthread_self()),
                 "spmm.cc Write_SpMatrix wrote {", key[0], ",", key[1], "} = ", baseT::template get<0>(elem), " in ",
                 static_cast<void *>(&matrix_), " with mutex @", static_cast<void *>(&mtx_), " for object @",
                 static_cast<void *>(this));
    }
    values_.emplace_back(key[0], key[1], baseT::template get<0>(elem));
  }

  /// grab completion status as a future<void>
  /// \note cannot be called once this is executable
  const std::shared_future<void> &status() const {
    assert(!this->is_executable());
    if (!completion_status_) {  // if not done yet, register completion work with the world
      auto promise = std::make_shared<std::promise<void>>();
      completion_status_ = std::make_shared<std::shared_future<void>>(promise->get_future());
      ttg_register_status(this->get_world(), std::move(promise));
      ttg_register_callback(this->get_world(),
                            [this]() { this->matrix_.setFromTriplets(this->values_.begin(), this->values_.end()); });
    } else {  // if done already, commit the result
      this->matrix_.setFromTriplets(this->values_.begin(), this->values_.end());
    }
    return *completion_status_;
  }

 private:
  std::mutex mtx_;
  SpMatrix<Blk> &matrix_;
  std::vector<SpMatrixTriplet<Blk>> values_;
  mutable std::shared_ptr<std::shared_future<void>> completion_status_;
};

// sparse mm
template <typename Keymap = std::function<int(const Key<2> &)>, typename Blk = blk_t>
class SpMM {
 public:
  SpMM(Edge<Key<2>, Blk> &a, Edge<Key<2>, Blk> &b, Edge<Key<2>, Blk> &c, const SpMatrix<Blk> &a_mat,
       const SpMatrix<Blk> &b_mat, const std::vector<std::vector<long>> &a_rowidx_to_colidx,
       const std::vector<std::vector<long>> &a_colidx_to_rowidx,
       const std::vector<std::vector<long>> &b_rowidx_to_colidx,
       const std::vector<std::vector<long>> &b_colidx_to_rowidx, const std::vector<int> &mTiles,
       const std::vector<int> &nTiles, const std::vector<int> &kTiles, const Keymap &keymap)
      : a_ijk_()
      , local_a_ijk_()
      , b_ijk_()
      , local_b_ijk_()
      , c_ijk_()
      , a_rowidx_to_colidx_(a_rowidx_to_colidx)
      , b_colidx_to_rowidx_(b_colidx_to_rowidx)
      , a_colidx_to_rowidx_(a_colidx_to_rowidx)
      , b_rowidx_to_colidx_(b_rowidx_to_colidx) {
    bcast_a_ = std::make_unique<BcastA>(a, local_a_ijk_, b_rowidx_to_colidx_, keymap);
    local_bcast_a_ = std::make_unique<LocalBcastA>(local_a_ijk_, a_ijk_, b_rowidx_to_colidx_, keymap);
    bcast_b_ = std::make_unique<BcastB>(b, local_b_ijk_, a_colidx_to_rowidx_, keymap);
    local_bcast_b_ = std::make_unique<LocalBcastB>(local_b_ijk_, b_ijk_, a_colidx_to_rowidx_, keymap);
    multiplyadd_ = std::make_unique<MultiplyAdd>(a_ijk_, b_ijk_, c_ijk_, c, a_rowidx_to_colidx_, b_colidx_to_rowidx_,
                                                 mTiles, nTiles, keymap);

    TTGUNUSED(bcast_a_);
    TTGUNUSED(bcast_b_);
    TTGUNUSED(multiplyadd_);
  }

  /// Locally broadcast A[i][k] to all {i,j,k} such that B[j][k] exists
  class LocalBcastA : public Op<Key<3>, std::tuple<Out<Key<3>, Blk>>, LocalBcastA, Blk> {
   public:
    using baseT = Op<Key<3>, std::tuple<Out<Key<3>, Blk>>, LocalBcastA, Blk>;

    LocalBcastA(Edge<Key<3>, Blk> &a, Edge<Key<3>, Blk> &a_ijk,
                const std::vector<std::vector<long>> &b_rowidx_to_colidx, Keymap keymap)
        : baseT(edges(a), edges(a_ijk), "SpMM::local_bcast_a", {"a_ik"}, {"a_ijk"},
                [](const Key<3> &key) { return key[2]; })
        , b_rowidx_to_colidx_(b_rowidx_to_colidx)
        , keymap_(keymap) {}

    void op(const Key<3> &key, typename baseT::input_values_tuple_type &&a_ik, std::tuple<Out<Key<3>, Blk>> &a_ijk) {
      const auto i = key[0];
      const auto k = key[1];
      auto world = get_default_world();
      assert(key[2] == world.rank());
      if (tracing()) ttg::print("LocalBcastA(", i, ", ", k, ")");
      if (k >= b_rowidx_to_colidx_.size()) return;
      // broadcast a_ik to all existing {i,j,k}
      std::vector<Key<3>> ijk_keys;
      for (auto &j : b_rowidx_to_colidx_[k]) {
        if (tracing()) ttg::print("Broadcasting A[", i, "][", k, "] to j=", j);
        if (keymap_(Key<2>({i, j})) == world.rank()) {
          ijk_keys.emplace_back(Key<3>({i, j, k}));
        }
      }
      ::broadcast<0>(ijk_keys, baseT::template get<0>(a_ik), a_ijk);
    }

   private:
    const std::vector<std::vector<long>> &b_rowidx_to_colidx_;
    Keymap keymap_;
  };  // class LocalBcastA

  /// broadcast A[i][k] to all procs where B[j][k]
  class BcastA : public Op<Key<2>, std::tuple<Out<Key<3>, Blk>>, BcastA, Blk> {
   public:
    using baseT = Op<Key<2>, std::tuple<Out<Key<3>, Blk>>, BcastA, Blk>;

    BcastA(Edge<Key<2>, Blk> &a, Edge<Key<3>, Blk> &a_ikp, const std::vector<std::vector<long>> &b_rowidx_to_colidx,
           Keymap keymap)
        : baseT(edges(a), edges(a_ikp), "SpMM::bcast_a", {"a_ik"}, {"a_ikp"}, keymap)
        , b_rowidx_to_colidx_(b_rowidx_to_colidx) {}

    void op(const Key<2> &key, typename baseT::input_values_tuple_type &&a_ik, std::tuple<Out<Key<3>, Blk>> &a_ikp) {
      const auto i = key[0];
      const auto k = key[1];
      if (tracing()) ttg::print("BcastA(", i, ", ", k, ")");
      // broadcast a_ik to all existing {i,j,k}
      std::vector<Key<3>> ikp_keys;
      if (k >= b_rowidx_to_colidx_.size()) return;
      auto world = get_default_world();
      std::vector<bool> procmap(world.size());
      auto keymap = baseT::get_keymap();
      for (auto &j : b_rowidx_to_colidx_[k]) {
        long proc = keymap(Key<2>({i, j}));
        if (!procmap[proc]) {
          if (tracing()) ttg::print("Broadcasting A[", i, "][", k, "] to proc ", proc);
          ikp_keys.emplace_back(Key<3>({i, k, proc}));
          procmap[proc] = true;
        }
      }
      ::broadcast<0>(ikp_keys, baseT::template get<0>(a_ik), a_ikp);
    }

   private:
    const std::vector<std::vector<long>> &b_rowidx_to_colidx_;
  };  // class BcastA

  /// broadcast B[k][j] to all {i,j,k} such that A[i][k] exists
  class LocalBcastB : public Op<Key<3>, std::tuple<Out<Key<3>, Blk>>, LocalBcastB, Blk> {
   public:
    using baseT = Op<Key<3>, std::tuple<Out<Key<3>, Blk>>, LocalBcastB, Blk>;

    LocalBcastB(Edge<Key<3>, Blk> &b, Edge<Key<3>, Blk> &b_ijk,
                const std::vector<std::vector<long>> &a_colidx_to_rowidx, Keymap keymap)
        : baseT(edges(b), edges(b_ijk), "SpMM::local_bcast_b", {"b_kj"}, {"b_ijk"},
                [](const Key<3> &key) { return key[2]; })
        , a_colidx_to_rowidx_(a_colidx_to_rowidx)
        , keymap_(keymap) {}

    void op(const Key<3> &key, typename baseT::input_values_tuple_type &&b_kj, std::tuple<Out<Key<3>, Blk>> &b_ijk) {
      const auto k = key[0];
      const auto j = key[1];
      auto world = get_default_world();
      assert(key[2] == world.rank());
      if (tracing()) ttg::print("BcastB(", k, ", ", j, ")");
      if (k >= a_colidx_to_rowidx_.size()) return;
      // broadcast b_kj to *jk
      std::vector<Key<3>> ijk_keys;
      for (auto &i : a_colidx_to_rowidx_[k]) {
        if (tracing()) ttg::print("Broadcasting B[", k, "][", j, "] to i=", i);
        if (keymap_(Key<2>({i, j})) == world.rank()) {
          ijk_keys.emplace_back(Key<3>({i, j, k}));
        }
      }
      ::broadcast<0>(ijk_keys, baseT::template get<0>(b_kj), b_ijk);
    }

   private:
    const std::vector<std::vector<long>> &a_colidx_to_rowidx_;
    Keymap keymap_;
  };  // class BcastA

  /// broadcast B[k][j] to all {i,j,k} such that A[i][k] exists
  class BcastB : public Op<Key<2>, std::tuple<Out<Key<3>, Blk>>, BcastB, Blk> {
   public:
    using baseT = Op<Key<2>, std::tuple<Out<Key<3>, Blk>>, BcastB, Blk>;

    BcastB(Edge<Key<2>, Blk> &b, Edge<Key<3>, Blk> &b_kjp, const std::vector<std::vector<long>> &a_colidx_to_rowidx,
           Keymap keymap)
        : baseT(edges(b), edges(b_kjp), "SpMM::bcast_b", {"b_kjp"}, {"b_ijk"}, keymap)
        , a_colidx_to_rowidx_(a_colidx_to_rowidx) {}

    void op(const Key<2> &key, typename baseT::input_values_tuple_type &&b_kj, std::tuple<Out<Key<3>, Blk>> &b_kjp) {
      const auto k = key[0];
      const auto j = key[1];
      // broadcast b_kj to *jk
      std::vector<Key<3>> kjp_keys;
      if (tracing()) ttg::print("BcastB(", k, ", ", j, ")");
      if (k >= a_colidx_to_rowidx_.size()) return;
      auto world = get_default_world();
      std::vector<bool> procmap(world.size());
      for (auto &i : a_colidx_to_rowidx_[k]) {
        long proc = baseT::get_keymap()(Key<2>({i, j}));
        if (!procmap[proc]) {
          if (tracing()) ttg::print("Broadcasting A[", k, "][", j, "] to proc ", proc);
          kjp_keys.emplace_back(Key<3>({k, j, proc}));
          procmap[proc] = true;
        }
      }
      ::broadcast<0>(kjp_keys, baseT::template get<0>(b_kj), b_kjp);
    }

   private:
    const std::vector<std::vector<long>> &a_colidx_to_rowidx_;
  };  // class BcastA

  /// multiply task has 3 input flows: a_ijk, b_ijk, and c_ijk, c_ijk contains the running total
  class MultiplyAdd
      : public Op<Key<3>, std::tuple<Out<Key<2>, Blk>, Out<Key<3>, Blk>>, MultiplyAdd, const Blk, const Blk, Blk> {
   public:
    using baseT = Op<Key<3>, std::tuple<Out<Key<2>, Blk>, Out<Key<3>, Blk>>, MultiplyAdd, const Blk, const Blk, Blk>;

    MultiplyAdd(Edge<Key<3>, Blk> &a_ijk, Edge<Key<3>, Blk> &b_ijk, Edge<Key<3>, Blk> &c_ijk, Edge<Key<2>, Blk> &c,
                const std::vector<std::vector<long>> &a_rowidx_to_colidx,
                const std::vector<std::vector<long>> &b_colidx_to_rowidx, const std::vector<int> &mTiles,
                const std::vector<int> &nTiles, Keymap keymap)
        : baseT(edges(a_ijk, b_ijk, c_ijk), edges(c, c_ijk), "SpMM::MultiplyAdd", {"a_ijk", "b_ijk", "c_ijk"},
                {"c_ij", "c_ijk"},
                [keymap](const Key<3> &key) {
                  auto key2 = Key<2>({key[0], key[1]});
                  return keymap(key2);
                })
        , a_rowidx_to_colidx_(a_rowidx_to_colidx)
        , b_colidx_to_rowidx_(b_colidx_to_rowidx) {
      this->set_priomap([=](const Key<3> &key) { return this->prio(key); });

      // for each i and j that belongs to this node
      // determine first k that contributes, initialize input {i,j,first_k} flow to 0
      for (auto i = 0ul; i != a_rowidx_to_colidx_.size(); ++i) {
        if (a_rowidx_to_colidx_[i].empty()) continue;
        for (auto j = 0ul; j != b_colidx_to_rowidx_.size(); ++j) {
          if (b_colidx_to_rowidx_[j].empty()) continue;

          // assuming here {i,j,k} for all k map to same node
          auto owner = keymap(Key<2>({i, j}));
          if (owner == ttg_default_execution_context().rank()) {
            if (true) {
              decltype(i) k;
              bool have_k;
              std::tie(k, have_k) = compute_first_k(i, j);
              if (have_k) {
                if (tracing()) ttg::print("Initializing C[", i, "][", j, "] to zero");
#if BLOCK_SPARSE_GEMM
                Blk zero(btas::Range(mTiles[i], nTiles[j]), 0.0);
#else
                Blk zero{0.0};
#endif
                this->template in<2>()->send(Key<3>({i, j, k}), zero);
              } else {
                if (tracing() && a_rowidx_to_colidx_.size() * b_colidx_to_rowidx_.size() < 400)
                  ttg::print("C[", i, "][", j, "] is empty");
              }
            }
          }
        }
      }
    }

    void op(const Key<3> &key, typename baseT::input_values_tuple_type &&_ijk,
            std::tuple<Out<Key<2>, Blk>, Out<Key<3>, Blk>> &result) {
      const auto i = key[0];
      const auto j = key[1];
      const auto k = key[2];
      long next_k;
      bool have_next_k;
      std::tie(next_k, have_next_k) = compute_next_k(i, j, k);
      if (tracing()) {
        ttg::print("Rank ", ttg_default_execution_context().rank(),
                   " :"
                   " C[",
                   i, "][", j, "]  += A[", i, "][", k, "] by B[", k, "][", j, "],  next_k? ",
                   (have_next_k ? std::to_string(next_k) : "does not exist"));
      }
      // compute the contrib, pass the running total to the next flow, if needed
      // otherwise write to the result flow
      if (have_next_k) {
        ::send<1>(
            Key<3>({i, j, next_k}),
            gemm(std::move(baseT::template get<2>(_ijk)), baseT::template get<0>(_ijk), baseT::template get<1>(_ijk)),
            result);
      } else
        ::send<0>(
            Key<2>({i, j}),
            gemm(std::move(baseT::template get<2>(_ijk)), baseT::template get<0>(_ijk), baseT::template get<1>(_ijk)),
            result);
    }

   private:
    const std::vector<std::vector<long>> &a_rowidx_to_colidx_;
    const std::vector<std::vector<long>> &b_colidx_to_rowidx_;

    /* Compute the length of the remaining sequence on that tile */
    int32_t prio(const Key<3> &key) {
      const auto i = key[0];
      const auto j = key[1];
      const auto k = key[2];
      int32_t len = -1;  // will be incremented at least once
      long next_k = k;
      bool have_next_k;
      do {
        std::tie(next_k, have_next_k) = compute_next_k(i, j, next_k);
        ++len;
      } while (have_next_k);
      return len;
    }

    // given {i,j} return first k such that A[i][k] and B[k][j] exist
    std::tuple<long, bool> compute_first_k(long i, long j) {
      const auto &a_k_range = a_rowidx_to_colidx_.at(i);
      auto a_iter = a_k_range.begin();
      auto a_iter_fence = a_k_range.end();
      if (a_iter == a_iter_fence) return std::make_tuple(-1, false);
      const auto &b_k_range = b_colidx_to_rowidx_.at(j);
      auto b_iter = b_k_range.begin();
      auto b_iter_fence = b_k_range.end();
      if (b_iter == b_iter_fence) return std::make_tuple(-1, false);

      {
        auto a_colidx = *a_iter;
        auto b_rowidx = *b_iter;
        while (a_colidx != b_rowidx) {
          if (a_colidx < b_rowidx) {
            ++a_iter;
            if (a_iter == a_iter_fence) return std::make_tuple(-1, false);
            a_colidx = *a_iter;
          } else {
            ++b_iter;
            if (b_iter == b_iter_fence) return std::make_tuple(-1, false);
            b_rowidx = *b_iter;
          }
        }
        return std::make_tuple(a_colidx, true);
      }
      assert(false);
    }

    // given {i,j,k} such that A[i][k] and B[k][j] exist
    // return next k such that this condition holds
    std::tuple<long, bool> compute_next_k(long i, long j, long k) {
      const auto &a_k_range = a_rowidx_to_colidx_.at(i);
      auto a_iter_fence = a_k_range.end();
      auto a_iter = std::find(a_k_range.begin(), a_iter_fence, k);
      assert(a_iter != a_iter_fence);
      const auto &b_k_range = b_colidx_to_rowidx_.at(j);
      auto b_iter_fence = b_k_range.end();
      auto b_iter = std::find(b_k_range.begin(), b_iter_fence, k);
      assert(b_iter != b_iter_fence);
      while (a_iter != a_iter_fence && b_iter != b_iter_fence) {
        ++a_iter;
        ++b_iter;
        if (a_iter == a_iter_fence || b_iter == b_iter_fence) return std::make_tuple(-1, false);
        auto a_colidx = *a_iter;
        auto b_rowidx = *b_iter;
        while (a_colidx != b_rowidx) {
          if (a_colidx < b_rowidx) {
            ++a_iter;
            if (a_iter == a_iter_fence) return std::make_tuple(-1, false);
            a_colidx = *a_iter;
          } else {
            ++b_iter;
            if (b_iter == b_iter_fence) return std::make_tuple(-1, false);
            b_rowidx = *b_iter;
          }
        }
        return std::make_tuple(a_colidx, true);
      }
      abort();  // unreachable
    }
  };

 private:
  Edge<Key<3>, Blk> a_ijk_;
  Edge<Key<3>, Blk> local_a_ijk_;
  Edge<Key<3>, Blk> b_ijk_;
  Edge<Key<3>, Blk> local_b_ijk_;
  Edge<Key<3>, Blk> c_ijk_;
  const std::vector<std::vector<long>> &a_rowidx_to_colidx_;
  const std::vector<std::vector<long>> &b_colidx_to_rowidx_;
  const std::vector<std::vector<long>> &a_colidx_to_rowidx_;
  const std::vector<std::vector<long>> &b_rowidx_to_colidx_;
  std::unique_ptr<BcastA> bcast_a_;
  std::unique_ptr<LocalBcastA> local_bcast_a_;
  std::unique_ptr<BcastB> bcast_b_;
  std::unique_ptr<LocalBcastB> local_bcast_b_;
  std::unique_ptr<MultiplyAdd> multiplyadd_;
};

class Control : public Op<void, std::tuple<Out<Key<2>>>, Control> {
  using baseT = Op<void, std::tuple<Out<Key<2>>>, Control>;
  int P;
  int Q;

 public:
  explicit Control(Edge<Key<2>> &ctl) : baseT(edges(), edges(ctl), "Control", {}, {"ctl"}), P(0), Q(0) {}

  void op(std::tuple<Out<Key<2>>> &out) const {
    for (int i = 0; i < P; i++) {
      for (int j = 0; j < Q; j++) {
        Key<2> k{i, j};
        if (ttg::tracing()) ttg::print("Control: enable {", i, ", ", j, "}");
        ::sendk<0>(k, out);
      }
    }
  }

  void start(const int _p, const int _q) {
    P = _p;
    Q = _q;
    invoke();
  }
};

#ifdef BLOCK_SPARSE_GEMM
template <typename T_, class Range_, class Store_>
std::tuple<T_, T_> norms(const btas::Tensor<T_, Range_, Store_> &t) {
  T_ norm_2_square = 0.0;
  T_ norm_inf = 0.0;
  for (auto k : t) {
    norm_2_square += k * k;
    norm_inf = std::max(norm_inf, std::abs(k));
  }
  return std::make_tuple(norm_2_square, norm_inf);
}
#endif  // defined(BLOCK_SPARSE_GEMM)

std::tuple<double, double> norms(double t) { return std::make_tuple(t * t, std::abs(t)); }

template <typename Blk = blk_t>
std::tuple<double, double> norms(const SpMatrix<Blk> &A) {
  double norm_2_square = 0.0;
  double norm_inf = 0.0;
  for (int i = 0; i < A.outerSize(); ++i) {
    for (typename SpMatrix<Blk>::InnerIterator it(A, i); it; ++it) {
      //  cout << 1+it.row() << "\t"; // row index
      //  cout << 1+it.col() << "\t"; // col index (here it is equal to k)
      //  cout << it.value() << endl;
      auto elem = it.value();
      double elem_norm_2_square, elem_norm_inf;
      std::tie(elem_norm_2_square, elem_norm_inf) = norms(elem);
      norm_2_square += elem_norm_2_square;
      norm_inf = std::max(norm_inf, elem_norm_inf);
    }
  }
  return std::make_tuple(norm_2_square, norm_inf);
}

char *getCmdOption(char **begin, char **end, const std::string &option) {
  static char *empty = (char *)"";
  char **itr = std::find(begin, end, option);
  if (itr != end && ++itr != end) return *itr;
  return empty;
}

bool cmdOptionExists(char **begin, char **end, const std::string &option) {
  return std::find(begin, end, option) != end;
}

int cmdOptionIndex(char **begin, char **end, const std::string &option) {
  char **itr = std::find(begin, end, option);
  if (itr != end) return (int)(itr - begin);
  return -1;
}

static int parseOption(std::string &option, int default_value) {
  size_t pos;
  std::string token;
  int N = default_value;
  if (option.length() == 0) return N;
  pos = option.find(':');
  if (pos == std::string::npos) {
    pos = option.length();
  }
  token = option.substr(0, pos);
  N = std::stoi(token);
  option.erase(0, pos + 1);
  return N;
}

static long parseOption(std::string &option, long default_value) {
  size_t pos;
  std::string token;
  long N = default_value;
  if (option.length() == 0) return N;
  pos = option.find(':');
  if (pos == std::string::npos) {
    pos = option.length();
  }
  token = option.substr(0, pos);
  N = std::stol(token);
  option.erase(0, pos + 1);
  return N;
}

static double parseOption(std::string &option, double default_value = 0.25) {
  size_t pos;
  std::string token;
  double N = default_value;
  if (option.length() == 0) return N;
  pos = option.find(':');
  if (pos == std::string::npos) {
    pos = option.length();
  }
  token = option.substr(0, pos);
  N = std::stod(token);
  option.erase(0, pos + 1);
  return N;
}

#ifndef BLOCK_SPARSE_GEMM
static void initSpMatrixMarket(const std::function<int(const Key<2> &)> &keymap, const char *filename, SpMatrix<> &A,
                               SpMatrix<> &B, SpMatrix<> &C, int &M, int &N, int &K) {
  std::vector<int> sizes;
  // We load the entire matrix on each rank, but we only use the local part for the GEMM
  if (!loadMarket(A, filename)) {
    std::cerr << "Failed to load " << filename << ", bailing out..." << std::endl;
    ttg::ttg_abort();
  }
  if (0 == ttg_default_execution_context().rank()) {
    std::cout << "##MatrixMarket file " << filename << " -- " << A.rows() << " x " << A.cols() << " -- " << A.nonZeros()
              << " nnz (density: " << (float)A.nonZeros() / (float)A.rows() / (float)A.cols() << ")" << std::endl;
  }
  if (A.rows() != A.cols()) {
    B = A.transpose();
  } else {
    B = A;
  }
  C.resize(A.rows(), B.cols());
  M = (int)A.rows();
  N = (int)C.cols();
  K = (int)A.cols();
}

static void initSpRmat(const std::function<int(const Key<2> &)> &keymap, const char *opt, SpMatrix<> &A, SpMatrix<> &B,
                       SpMatrix<> &C, int &M, int &N, int &K, unsigned long seed) {
  int E;
  double a = 0.25, b = 0.25, c = 0.25, d = 0.25;
  size_t nnz = 0;

  if (nullptr == opt) {
    std::cerr << "Usage: -rmat <#nodes>[:<#edges>[:<a>[:<b>:[<c>[:<d>]]]]]" << std::endl;
    exit(1);
  }
  std::string token;
  std::string option = std::string(opt);
  N = parseOption(option, -1);
  K = N;
  M = N;

  // We build the entire sparse matrix on each rank, but use only the local part
  // on a given rank, according to keymap
  A.resize(N, N);

  E = parseOption(option, (int)(0.01 * N * N));
  a = parseOption(option, a);
  b = parseOption(option, b);
  c = parseOption(option, c);
  d = parseOption(option, d);

  if (ttg_default_execution_context().rank() == 0) {
    std::cout << "#R-MAT: " << N << " nodes, " << E << " edges, a/b/c/d = " << a << "/" << b << "/" << c << "/" << d
              << std::endl;
  }

  boost::minstd_rand gen(seed);
  boost::rmat_iterator<boost::minstd_rand, boost::directed_graph<>> rmat_it(gen, N, E, a, b, c, d);

  using triplet_t = Eigen::Triplet<blk_t>;
  std::vector<triplet_t> A_elements;
  for (int i = 0; i < N; i++) {
    nnz++;
    A_elements.emplace_back(i, i, 1.0);
  }
  for (int i = 0; i < E; i++) {
    auto x = *rmat_it++;
    if (x.first != x.second) {
      A_elements.emplace_back(x.first, x.second, 1.0);
      nnz++;
    }
  }
  A.setFromTriplets(A_elements.begin(), A_elements.end());

  B = A;
  C.resize(N, N);

  if (ttg_default_execution_context().rank() == 0) {
    std::cout << "#R-MAT: " << E << " nonzero elements, density: " << (double)nnz / (double)N / (double)N << std::endl;
  }
}

static void initSpHardCoded(const std::function<int(const Key<2> &)> &keymap, SpMatrix<> &A, SpMatrix<> &B,
                            SpMatrix<> &C, int &m, int &n, int &k) {
  m = 2;
  n = 3;
  k = 4;

  std::cout << "#HardCoded A, B, C" << std::endl;
  A.resize(m, k);
  B.resize(k, n);
  C.resize(m, n);
  // We initialize the same matrices on all the ranks, but we will use only the local part
  // following the keymap
  using triplet_t = Eigen::Triplet<blk_t>;
  std::vector<triplet_t> A_elements;
  A_elements.emplace_back(0, 1, 12.3);
  A_elements.emplace_back(0, 2, 10.7);
  A_elements.emplace_back(0, 3, -2.3);
  A_elements.emplace_back(1, 0, -0.3);
  A_elements.emplace_back(1, 2, 1.2);
  A.setFromTriplets(A_elements.begin(), A_elements.end());

  std::vector<triplet_t> B_elements;
  B_elements.emplace_back(0, 0, 12.3);
  B_elements.emplace_back(1, 0, 10.7);
  B_elements.emplace_back(3, 0, -2.3);
  B_elements.emplace_back(1, 1, -0.3);
  B_elements.emplace_back(1, 2, 1.2);
  B_elements.emplace_back(2, 2, 7.2);
  B_elements.emplace_back(3, 2, 0.2);
  B.setFromTriplets(B_elements.begin(), B_elements.end());
}
#else  // !defined(BLOCK_SPARSE_GEMM)
static void initBlSpHardCoded(const std::function<int(const Key<2> &)> &keymap, SpMatrix<> &A, SpMatrix<> &B,
                              SpMatrix<> &C, SpMatrix<> &Aref, SpMatrix<> &Bref, bool buildRefs,
                              std::vector<int> &mTiles, std::vector<int> &nTiles, std::vector<int> &kTiles,
                              std::vector<std::vector<long>> &a_rowidx_to_colidx,
                              std::vector<std::vector<long>> &a_colidx_to_rowidx,
                              std::vector<std::vector<long>> &b_rowidx_to_colidx,
                              std::vector<std::vector<long>> &b_colidx_to_rowidx, int &m, int &n, int &k) {
  m = 2;
  n = 3;
  k = 4;

  std::cout << "#HardCoded A, B, C" << std::endl;
  A.resize(m, k);
  B.resize(k, n);
  C.resize(m, n);
  if (buildRefs) {
    Aref.resize(m, k);
    Bref.resize(k, n);
  }

  for (int mt = 0; mt < m; mt++) mTiles.push_back(128);
  for (int nt = 0; nt < n; nt++) nTiles.push_back(196);
  for (int kt = 0; kt < k; kt++) kTiles.push_back(256);

  int rank = ttg_default_execution_context().rank();

  using triplet_t = Eigen::Triplet<blk_t>;
  std::vector<triplet_t> A_elements;
  std::vector<triplet_t> Aref_elements;

  auto emplace_A_element = [&A_elements, &Aref_elements, &keymap, rank, buildRefs, &a_rowidx_to_colidx,
                            &a_colidx_to_rowidx](std::initializer_list<int> ij, const auto &value) {
    auto i = *(ij.begin());
    auto j = *(ij.begin() + 1);
    if (i >= a_rowidx_to_colidx.size()) a_rowidx_to_colidx.resize(i + 1);
    a_rowidx_to_colidx[i].emplace_back(j);
    if (j >= a_colidx_to_rowidx.size()) a_colidx_to_rowidx.resize(j + 1);
    a_colidx_to_rowidx[j].emplace_back(i);
    if (keymap(ij) == rank) {
      A_elements.emplace_back(*(ij.begin()), *(ij.begin() + 1), blk_t(btas::Range(128, 256), value));
    }
    if (buildRefs && rank == 0) {
      Aref_elements.emplace_back(*(ij.begin()), *(ij.begin() + 1), blk_t(btas::Range(128, 256), value));
    }
  };

  emplace_A_element({0, 1}, 12.3);
  emplace_A_element({0, 2}, 10.7);
  emplace_A_element({0, 3}, -2.3);
  emplace_A_element({1, 0}, -0.3);
  emplace_A_element({1, 2}, 1.2);

  A.setFromTriplets(A_elements.begin(), A_elements.end());
  if (buildRefs && 0 == rank) {
    Aref.setFromTriplets(Aref_elements.begin(), Aref_elements.end());
  }

  std::vector<triplet_t> B_elements;
  std::vector<triplet_t> Bref_elements;
  auto emplace_B_element = [&B_elements, &Bref_elements, &keymap, rank, buildRefs, &b_rowidx_to_colidx,
                            &b_colidx_to_rowidx](std::initializer_list<int> ij, const auto &value) {
    auto i = *(ij.begin());
    auto j = *(ij.begin() + 1);
    if (i >= b_rowidx_to_colidx.size()) b_rowidx_to_colidx.resize(i + 1);
    b_rowidx_to_colidx[i].emplace_back(j);
    if (j >= b_colidx_to_rowidx.size()) b_colidx_to_rowidx.resize(j + 1);
    b_colidx_to_rowidx[j].emplace_back(i);
    if (keymap(ij) == rank) {
      B_elements.emplace_back(*(ij.begin()), *(ij.begin() + 1), blk_t(btas::Range(128, 256), value));
    }
    if (buildRefs && rank == 0) {
      Bref_elements.emplace_back(*(ij.begin()), *(ij.begin() + 1), blk_t(btas::Range(128, 256), value));
    }
  };
  emplace_B_element({0, 0}, 12.3);
  emplace_B_element({1, 0}, 10.7);
  emplace_B_element({3, 0}, -2.3);
  emplace_B_element({1, 1}, -0.3);
  emplace_B_element({1, 2}, 1.2);
  emplace_B_element({2, 2}, 7.2);
  emplace_B_element({3, 2}, 0.2);

  B.setFromTriplets(B_elements.begin(), B_elements.end());
  if (buildRefs && 0 == rank) {
    Bref.setFromTriplets(Bref_elements.begin(), Bref_elements.end());
  }
}

static void initBlSpRandom(const std::function<int(const Key<2> &)> &keymap, size_t M, size_t N, size_t K, int minTs,
                           int maxTs, double avgDensity, SpMatrix<> &A, SpMatrix<> &B, SpMatrix<> &Aref,
                           SpMatrix<> &Bref, bool buildRefs, std::vector<int> &mTiles, std::vector<int> &nTiles,
                           std::vector<int> &kTiles, std::vector<std::vector<long>> &a_rowidx_to_colidx,
                           std::vector<std::vector<long>> &a_colidx_to_rowidx,
                           std::vector<std::vector<long>> &b_rowidx_to_colidx,
                           std::vector<std::vector<long>> &b_colidx_to_rowidx, double &average_tile_volume,
                           double &Adensity, double &Bdensity, unsigned int seed) {
  int rank = ttg_default_execution_context().rank();

  int ts;
  std::mt19937 gen(seed);
  std::mt19937 genv(seed + 1);

  std::uniform_int_distribution<> dist(minTs, maxTs);
  using triplet_t = Eigen::Triplet<blk_t>;
  std::vector<triplet_t> A_elements;
  std::vector<triplet_t> B_elements;
  std::vector<triplet_t> Aref_elements;
  std::vector<triplet_t> Bref_elements;

  for (int m = 0; m < M; m += ts) {
    ts = dist(gen);
    if (ts > M - m) ts = M - m;
    mTiles.push_back(ts);
  }
  for (int n = 0; n < N; n += ts) {
    ts = dist(gen);
    if (ts > N - n) ts = N - n;
    nTiles.push_back(ts);
  }
  for (int k = 0; k < K; k += ts) {
    ts = dist(gen);
    if (ts > K - k) ts = K - k;
    kTiles.push_back(ts);
  }

  A.resize(mTiles.size(), kTiles.size());
  B.resize(kTiles.size(), nTiles.size());
  if (buildRefs) {
    Aref.resize(mTiles.size(), kTiles.size());
    Bref.resize(kTiles.size(), nTiles.size());
  }

  std::uniform_int_distribution<> mDist(0, mTiles.size() - 1);
  std::uniform_int_distribution<> nDist(0, nTiles.size() - 1);
  std::uniform_int_distribution<> kDist(0, kTiles.size() - 1);
  std::uniform_real_distribution<> vDist(-1.0, 1.0);

  size_t filling = 0;
  size_t avg_nb = 0;
  int avg_nb_nb = 0;

  struct tuple_hash : public std::unary_function<std::tuple<int, int>, std::size_t> {
    std::size_t operator()(const std::tuple<int, int> &k) const {
      return static_cast<size_t>(std::get<0>(k)) | (static_cast<size_t>(std::get<1>(k)) << 32);
    }
  };

  std::unordered_set<std::tuple<int, int>, tuple_hash> fills;

  fills.clear();
  while ((double)filling / (double)(M * K) < avgDensity) {
    int mt = mDist(gen);
    int kt = kDist(gen);

    if (fills.find({mt, kt}) != fills.end()) continue;
    fills.insert({mt, kt});

    if (mt >= a_rowidx_to_colidx.size()) a_rowidx_to_colidx.resize(mt + 1);
    a_rowidx_to_colidx[mt].emplace_back(kt);
    if (kt >= a_colidx_to_rowidx.size()) a_colidx_to_rowidx.resize(kt + 1);
    a_colidx_to_rowidx[kt].emplace_back(mt);

    filling += mTiles[mt] * kTiles[kt];
    avg_nb += mTiles[mt] * kTiles[kt];
    avg_nb_nb++;
    double value = vDist(genv);
    if (0 == rank && buildRefs) Aref_elements.emplace_back(mt, kt, blk_t(btas::Range(mTiles[mt], kTiles[kt]), value));
    if (rank != keymap({mt, kt})) continue;
    A_elements.emplace_back(mt, kt, blk_t(btas::Range(mTiles[mt], kTiles[kt]), value));
  }
  for (auto &row : a_rowidx_to_colidx) {
    std::sort(row.begin(), row.end());
  }
  for (auto &col : a_colidx_to_rowidx) {
    std::sort(col.begin(), col.end());
  }
  A.setFromTriplets(A_elements.begin(), A_elements.end());
  Adensity = (double)filling / (double)(M * K);
  if (0 == rank && buildRefs) Aref.setFromTriplets(Aref_elements.begin(), Aref_elements.end());

  filling = 0;
  fills.clear();
  while ((double)filling / (double)(K * N) < avgDensity) {
    int nt = nDist(gen);
    int kt = kDist(gen);

    if (fills.find({kt, nt}) != fills.end()) continue;
    fills.insert({kt, nt});

    if (kt >= b_rowidx_to_colidx.size()) b_rowidx_to_colidx.resize(kt + 1);
    b_rowidx_to_colidx[kt].emplace_back(nt);
    if (nt >= b_colidx_to_rowidx.size()) b_colidx_to_rowidx.resize(nt + 1);
    b_colidx_to_rowidx[nt].emplace_back(kt);

    filling += kTiles[kt] * nTiles[nt];
    avg_nb += kTiles[kt] * nTiles[nt];
    avg_nb_nb++;
    double value = vDist(genv);
    if (0 == rank && buildRefs) Bref_elements.emplace_back(kt, nt, blk_t(btas::Range(kTiles[kt], nTiles[nt]), value));
    if (rank != keymap({kt, nt})) continue;
    B_elements.emplace_back(kt, nt, blk_t(btas::Range(kTiles[kt], nTiles[nt]), value));
  }
  for (auto &row : b_rowidx_to_colidx) {
    std::sort(row.begin(), row.end());
  }
  for (auto &col : b_colidx_to_rowidx) {
    std::sort(col.begin(), col.end());
  }
  B.setFromTriplets(B_elements.begin(), B_elements.end());
  Bdensity = (double)filling / (double)(K * N);
  if (0 == rank && buildRefs) Bref.setFromTriplets(Bref_elements.begin(), Bref_elements.end());
  fills.clear();

  average_tile_volume = (double)avg_nb / avg_nb_nb;
}

#ifdef BSPMM_HAS_LIBINT
static void initBlSpLibint2(libint2::Operator libint2_op, libint2::any libint2_op_params,
                            const std::vector<libint2::Atom> atoms, const std::string &basis_set_name,
                            double tile_perelem_2norm_threshold, const std::function<int(const Key<2> &)> &keymap,
                            int maxTs, int nthreads, SpMatrix<> &A, SpMatrix<> &B, SpMatrix<> &Aref, SpMatrix<> &Bref,
                            bool buildRefs, std::vector<int> &mTiles, std::vector<int> &nTiles,
                            std::vector<int> &kTiles, std::vector<std::vector<long>> &a_rowidx_to_colidx,
                            std::vector<std::vector<long>> &a_colidx_to_rowidx,
                            std::vector<std::vector<long>> &b_rowidx_to_colidx,
                            std::vector<std::vector<long>> &b_colidx_to_rowidx, double &average_tile_volume,
                            double &Adensity, double &Bdensity) {
  libint2::initialize();
  int rank = ttg_default_execution_context().rank();

  std::mutex mtx;  // will serialize access to non-concurrent data

  /// fires off nthreads instances of lambda in parallel, using use C++ threads
  auto parallel_do = [&nthreads, &mtx](auto &lambda) {
    std::vector<std::thread> threads;
    for (int thread_id = 0; thread_id != nthreads; ++thread_id) {
      if (thread_id != nthreads - 1)
        threads.push_back(std::thread(lambda, thread_id, &mtx));
      else
        lambda(thread_id, &mtx);
    }  // threads_id
    for (int thread_id = 0; thread_id < nthreads - 1; ++thread_id) threads[thread_id].join();
  };

  auto invert = [](const long dim2, const std::vector<size_t> &map12) {
    std::vector<long> map21(dim2, -1);
    for (size_t i1 = 0; i1 != map12.size(); ++i1) {
      const auto i2 = map12[i1];
      map21.at(i2) = i1;
    }
    return map21;
  };

  libint2::BasisSet bs(basis_set_name, atoms, /* throw_if_no_match = */ true);
  auto atom2shell = bs.atom2shell(atoms);
  auto shell2bf = bs.shell2bf();
  auto bf2shell = invert(bs.nbf(), shell2bf);

  // compute basis tilings by chopping into groups of atoms that are small enough
  std::vector<int> bsTiles;
  {
    const int natoms = atoms.size();
    int tile_size = 0;
    for (int a = 0; a != natoms; ++a) {
      auto &a_shells = atom2shell.at(a);
      const auto nbf_a = std::accumulate(a_shells.begin(), a_shells.end(), 0,
                                         [&bs](auto nbf, const auto &sh_idx) { return nbf + bs.at(sh_idx).size(); });
      if (tile_size + nbf_a <= maxTs) {
        tile_size += nbf_a;
      } else {
        if (tile_size == 0)  // 1 atom exceed max tile size, make the 1-atom tile
          bsTiles.emplace_back(nbf_a);
        else {
          bsTiles.emplace_back(tile_size);
          tile_size = nbf_a;
        }
        if (tile_size > maxTs) {
          bsTiles.emplace_back(tile_size);
          tile_size = 0;
        }
      }
    }
    if (tile_size > 0)  // last time
      bsTiles.emplace_back(tile_size);
  }
  mTiles = bsTiles;
  nTiles = bsTiles;
  kTiles = bsTiles;

  // fill the matrix, only insert tiles with norm greater than the threshold
  auto fill_matrix = [&](const auto &tiles) {
    SpMatrix<> M, Mref;

    const auto ntiles = tiles.size();

    M.resize(ntiles, ntiles);
    if (buildRefs && rank == 0) {
      Mref.resize(tiles.size(), tiles.size());
    }

    // this data will be computed concurrently
    using triplet_t = Eigen::Triplet<blk_t>;
    std::vector<triplet_t> elements;
    std::vector<triplet_t> ref_elements;
    double total_tile_volume = 0.;
    std::vector<std::vector<long>> rowidx_to_colidx(ntiles), colidx_to_rowidx(ntiles);

    auto fill_matrix_impl = [&](int thread_id, std::mutex *mtx) {
      libint2::Engine engine(libint2_op, bs.max_nprim(), bs.max_l(), 0, std::numeric_limits<double>::epsilon(),
                             libint2_op_params, libint2::BraKet::xs_xs);

      const auto ntiles = tiles.size();
      const auto nshell = bs.size();
      const auto nbf = bs.nbf();
      long row_bf_offset = 0;
      for (auto row_tile_idx = 0; row_tile_idx != tiles.size(); ++row_tile_idx) {
        const auto row_bf_fence = row_bf_offset + tiles[row_tile_idx];
        const auto row_sh_offset = bf2shell.at(row_bf_offset);
        assert(row_sh_offset != 1);
        const auto row_sh_fence = (row_bf_fence != nbf) ? bf2shell.at(row_bf_fence) : nshell;
        assert(row_sh_fence != 1);

        long col_bf_offset = 0;
        for (auto col_tile_idx = 0; col_tile_idx != tiles.size(); ++col_tile_idx) {
          const auto col_bf_fence = col_bf_offset + tiles[col_tile_idx];

          // skip this tile if it does not belong to this rank
          const auto my_tile = (rank == keymap({row_tile_idx, col_tile_idx}) || (buildRefs && rank == 0)) &&
                               ((row_tile_idx * ntiles + col_tile_idx) % nthreads == thread_id);
          const auto really_my_tile = (rank == keymap({row_tile_idx, col_tile_idx})) &&
                                      ((row_tile_idx * ntiles + col_tile_idx) % nthreads == thread_id);
          if (my_tile) {
            const auto col_sh_offset = bf2shell.at(col_bf_offset);
            assert(col_sh_offset != 1);
            const auto col_sh_fence = (col_bf_fence != nbf) ? bf2shell.at(col_bf_fence) : nshell;
            assert(col_sh_fence != 1);

            blk_t tile(btas::Range({row_bf_offset, col_bf_offset}, {row_bf_fence, col_bf_fence}), 0.);

            for (auto row_sh_idx = row_sh_offset; row_sh_idx != row_sh_fence; ++row_sh_idx) {
              const auto &row_sh = bs.at(row_sh_idx);
              const auto row_sh_bf_offset = shell2bf.at(row_sh_idx);
              for (auto col_sh_idx = col_sh_offset; col_sh_idx != col_sh_fence; ++col_sh_idx) {
                const auto &col_sh = bs.at(col_sh_idx);
                const auto col_sh_bf_offset = shell2bf.at(col_sh_idx);

                engine.compute(row_sh, col_sh);

                // copy to the tile
                {
                  const auto *shellset = engine.results()[0];
                  for (auto bf0 = 0, bf01 = 0; bf0 != row_sh.size(); ++bf0)
                    for (auto bf1 = 0; bf1 != col_sh.size(); ++bf1, ++bf01)
                      tile(row_sh_bf_offset + bf0, col_sh_bf_offset + bf1) = shellset[bf01];
                }
              }
            }

            const auto tile_volume = tile.range().volume();
            const auto tile_perelem_2norm = std::sqrt(btas::dot(tile, tile)) / tile_volume;

            if (tile_perelem_2norm >= tile_perelem_2norm_threshold) {
              {
                std::scoped_lock<std::mutex> lock(*mtx);
                if (buildRefs && rank == 0) {
                  ref_elements.emplace_back(row_tile_idx, col_tile_idx, tile);
                }
                if (really_my_tile) {
                  elements.emplace_back(row_tile_idx, col_tile_idx, tile);
                  rowidx_to_colidx.at(row_tile_idx).emplace_back(col_tile_idx);
                  colidx_to_rowidx.at(col_tile_idx).emplace_back(row_tile_idx);
                  total_tile_volume += tile.range().volume();
                }
              }
            }
          }  // !my_tile

          col_bf_offset = col_bf_fence;
        }
        row_bf_offset = row_bf_fence;
      }
    };

    parallel_do(fill_matrix_impl);

    long nnz_tiles = elements.size();  // # of nonzero tiles, currently on this rank only

    // allreduce metadata: rowidx_to_colidx, colidx_to_rowidx, total_tile_volume, nnz_tiles
    ttg_sum(ttg_default_execution_context(), nnz_tiles);
    ttg_sum(ttg_default_execution_context(), total_tile_volume);
    auto allreduce_vevveclong = [&](std::vector<std::vector<long>> &vvl) {
      std::vector<std::vector<long>> vvl_result(vvl.size());
      for (long source_rank = 0; source_rank != ttg_default_execution_context().size(); ++source_rank) {
        for (auto rowidx = 0; rowidx != ntiles; ++rowidx) {
          long sz = vvl.at(rowidx).size();
          MPI_Bcast(&sz, 1, MPI_LONG, source_rank, ttg_default_execution_context().impl().comm());
          if (rank == source_rank) {
            MPI_Bcast(vvl[rowidx].data(), sz, MPI_LONG, source_rank, ttg_default_execution_context().impl().comm());
            vvl_result.at(rowidx).insert(vvl_result[rowidx].end(), vvl[rowidx].begin(), vvl[rowidx].end());
          } else {
            std::vector<long> colidxs(sz);
            MPI_Bcast(colidxs.data(), sz, MPI_LONG, source_rank, ttg_default_execution_context().impl().comm());
            vvl_result.at(rowidx).insert(vvl_result[rowidx].end(), colidxs.begin(), colidxs.end());
          }
        }
      }
      vvl = std::move(vvl_result);
    };
    allreduce_vevveclong(rowidx_to_colidx);
    allreduce_vevveclong(colidx_to_rowidx);

    for (auto &row : rowidx_to_colidx) {
      std::sort(row.begin(), row.end());
    }
    for (auto &col : colidx_to_rowidx) {
      std::sort(col.begin(), col.end());
    }

    const auto nbf = bs.nbf();
    const double density = total_tile_volume / (nbf * nbf);
    const auto avg_tile_volume = total_tile_volume / elements.size();
    M.setFromTriplets(elements.begin(), elements.end());
    if (buildRefs && rank == 0) Mref.setFromTriplets(ref_elements.begin(), ref_elements.end());

    return std::make_tuple(M, Mref, rowidx_to_colidx, colidx_to_rowidx, avg_tile_volume, density);
  };

  std::tie(A, Aref, a_rowidx_to_colidx, a_colidx_to_rowidx, average_tile_volume, Adensity) = fill_matrix(bsTiles);
  B = A;
  Bref = Aref;
  b_rowidx_to_colidx = a_rowidx_to_colidx;
  b_colidx_to_rowidx = a_colidx_to_rowidx;
  Bdensity = Adensity;

  libint2::finalize();
}
#endif  // defined(BSPMM_HAS_LIBINT)

#endif  // defined(BLOCK_SPARSE_GEMM)

static void timed_measurement(SpMatrix<> &A, SpMatrix<> &B, const std::function<int(const Key<2> &)> &keymap,
                              const std::string &tiling_type, double gflops, double avg_nb, double Adensity,
                              double Bdensity, const std::vector<std::vector<long>> &a_rowidx_to_colidx,
                              const std::vector<std::vector<long>> &a_colidx_to_rowidx,
                              const std::vector<std::vector<long>> &b_rowidx_to_colidx,
                              const std::vector<std::vector<long>> &b_colidx_to_rowidx, std::vector<int> &mTiles,
                              std::vector<int> &nTiles, std::vector<int> &kTiles, int M, int N, int K, int P, int Q) {
  int MT = (int)A.rows();
  int NT = (int)B.cols();
  int KT = (int)A.cols();
  assert(KT == B.rows());

  SpMatrix<> C;
  C.resize(MT, NT);

  // flow graph needs to exist on every node
  Edge<Key<2>> ctl("control");
  Control control(ctl);
  Edge<Key<2>, blk_t> eA, eB, eC;

  Read_SpMatrix a("A", A, ctl, eA, keymap);
  Read_SpMatrix b("B", B, ctl, eB, keymap);
  Write_SpMatrix<> c(C, eC, keymap);
  auto &c_status = c.status();
  assert(!has_value(c_status));
  //  SpMM a_times_b(world, eA, eB, eC, A, B);
  SpMM<> a_times_b(eA, eB, eC, A, B, a_rowidx_to_colidx, a_colidx_to_rowidx, b_rowidx_to_colidx, b_colidx_to_rowidx,
                   mTiles, nTiles, kTiles, keymap);
  TTGUNUSED(a);
  TTGUNUSED(b);
  TTGUNUSED(a_times_b);

  auto connected = make_graph_executable(&control);
  assert(connected);
  TTGUNUSED(connected);

  struct timeval start {
    0
  }, end{0}, diff{0};
  gettimeofday(&start, nullptr);
  // ready, go! need only 1 kick, so must be done by 1 thread only
  if (ttg_default_execution_context().rank() == 0) control.start(P, Q);
  ttg_fence(ttg_default_execution_context());
  gettimeofday(&end, nullptr);
  timersub(&end, &start, &diff);
  double tc = (double)diff.tv_sec + (double)diff.tv_usec / 1e6;
#if defined(TTG_USE_MADNESS)
  std::string rt("MAD");
#elif defined(TTG_USE_PARSEC)
  std::string rt("PARSEC");
#else
  std::string rt("Unkown???");
#endif
  if (ttg_default_execution_context().rank() == 0) {
    std::cout << "TTG-" << rt << " PxQxg=   " << P << " " << Q << " 1 average_NB= " << avg_nb << " M= " << M
              << " N= " << N << " K= " << K << " Tiling= " << tiling_type << " A_density= " << Adensity
              << " B_density= " << Bdensity << " gflops= " << gflops << " seconds= " << tc
              << " gflops/s= " << gflops / tc << std::endl;
  }
}

#ifndef BLOCK_SPARSE_GEMM
static void make_rowidx_to_colidx_from_eigen(const SpMatrix<> &mat, std::vector<std::vector<long>> &r2c) {
  for (int k = 0; k < mat.outerSize(); ++k) {  // cols, if col-major, rows otherwise
    for (typename SpMatrix<blk_t>::InnerIterator it(mat, k); it; ++it) {
      const long row = it.row();
      const long col = it.col();
      if (row >= r2c.size()) r2c.resize(row + 1);
      r2c[row].push_back(col);
    }
  }
  // Sort each vector of column indices, as we pushed them in an arbitrary order
  for (auto &row : r2c) {
    std::sort(row.begin(), row.end());
  }
}

static void make_colidx_to_rowidx_from_eigen(const SpMatrix<> &mat, std::vector<std::vector<long>> &c2r) {
  for (int k = 0; k < mat.outerSize(); ++k) {  // cols, if col-major, rows otherwise
    for (typename SpMatrix<blk_t>::InnerIterator it(mat, k); it; ++it) {
      const long row = it.row();
      const long col = it.col();

      if (col >= c2r.size()) c2r.resize(col + 1);
      c2r[col].push_back(row);
    }
    // Sort each vector of row indices, as we pushed them in an arbitrary order
    for (auto &col : c2r) {
      std::sort(col.begin(), col.end());
    }
  }
}
#endif  // !defined(BLOCK_SPARSE_GEMM)

static double compute_gflops(const std::vector<std::vector<long>> &a_r2c, const std::vector<std::vector<long>> &b_r2c,
                             const std::vector<int> &mTiles, const std::vector<int> &nTiles,
                             const std::vector<int> &kTiles) {
  unsigned long flops = 0;
  for (auto i = 0; i < a_r2c.size(); i++) {
    for (auto kk = 0; kk < a_r2c[i].size(); kk++) {
      auto k = a_r2c[i][kk];
      if (k > b_r2c.size()) continue;
      for (auto jj = 0; jj < b_r2c[k].size(); jj++) {
        auto j = b_r2c[k][jj];
        flops += mTiles[i] * nTiles[j] * kTiles[k];
      }
    }
  }
  return 2.0 * (double)flops / 1e9;
}

int main(int argc, char **argv) {
  bool timing;
  double gflops;

  int cores = -1;
  std::string nbCoreStr(getCmdOption(argv, argv + argc, "-c"));
  cores = parseOption(nbCoreStr, cores);

  if (int dashdash = cmdOptionIndex(argv, argv + argc, "--") > -1) {
    ttg_initialize(argc - dashdash, argv + dashdash, cores);
  } else {
    ttg_initialize(1, argv, cores);
  }

  // launch_lldb(ttg_default_execution_context().rank(), argv[0]);

  std::string debugStr(getCmdOption(argv, argv + argc, "-d"));
  auto debug = (unsigned int)parseOption(debugStr, 0);

  if (debug & (1 << 1)) {
    using mpqc::Debugger;
    auto debugger = std::make_shared<Debugger>();
    Debugger::set_default_debugger(debugger);
    debugger->set_exec(argv[0]);
    debugger->set_prefix(ttg_default_execution_context().rank());
    debugger->set_cmd("lldb_xterm");
    // debugger->set_cmd("gdb_xterm");
  }

  int mpi_size = ttg_default_execution_context().size();
  int mpi_rank = ttg_default_execution_context().rank();
  int best_pq = mpi_size;
  int P, Q;
  for (int p = 1; p <= (int)sqrt(mpi_size); p++) {
    if ((mpi_size % p) == 0) {
      int q = mpi_size / p;
      if (abs(p - q) < best_pq) {
        best_pq = abs(p - q);
        P = p;
        Q = q;
      }
    }
  }
  // ttg::launch_lldb(ttg_default_execution_context().rank(), argv[0]);

  {
    if (debug & (1 << 0)) {
      ttg::trace_on();
      OpBase::set_trace_all(true);
    }

    SpMatrix<> A, B, C, Aref, Bref;
    std::stringstream tiling_type;
    int M = -1, N = -1, K = -1;

    double avg_nb = nan("undefined");
    double Adensity = nan("undefined");
    double Bdensity = nan("undefined");

    std::string PStr(getCmdOption(argv, argv + argc, "-P"));
    P = parseOption(PStr, P);
    std::string QStr(getCmdOption(argv, argv + argc, "-Q"));
    Q = parseOption(QStr, Q);

    if (P * Q != mpi_size) {
      if (!cmdOptionExists(argv, argv + argc, "-Q") && (mpi_size % P) == 0)
        Q = mpi_size / P;
      else if (!cmdOptionExists(argv, argv + argc, "-P") && (mpi_size % Q) == 0)
        P = mpi_size / Q;
      else {
        if (0 == mpi_rank) {
          std::cerr << P << "x" << Q << " is not a valid process grid -- bailing out" << std::endl;
          MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
      }
    }

    // block-cyclic map
    auto bc_keymap = [P, Q](const Key<2> &key) {
      int i = (int)key[0];
      int j = (int)key[1];
      int pq = tile2rank(i, j, P, Q);
      return pq;
    };

    std::string seedStr(getCmdOption(argv, argv + argc, "-s"));
    unsigned int seed = parseOption(seedStr, 0);
    if (seed == 0) {
      std::random_device rd;
      seed = rd();
      if (0 == ttg_default_execution_context().rank()) std::cerr << "#Random seeded with " << seed << std::endl;
    }
    ttg_broadcast(ttg_default_execution_context(), seed, 0);

    std::vector<int> mTiles;
    std::vector<int> nTiles;
    std::vector<int> kTiles;
    std::vector<std::vector<long>> a_rowidx_to_colidx;
    std::vector<std::vector<long>> a_colidx_to_rowidx;
    std::vector<std::vector<long>> b_rowidx_to_colidx;
    std::vector<std::vector<long>> b_colidx_to_rowidx;

    std::string checkStr(getCmdOption(argv, argv + argc, "-x"));
    int check = parseOption(checkStr, !(argc >= 2));
    timing = (check == 0);

#ifndef BLOCK_SPARSE_GEMM
    if (cmdOptionExists(argv, argv + argc, "-mm")) {
      char *filename = getCmdOption(argv, argv + argc, "-mm");
      tiling_type << filename;
      initSpMatrixMarket(bc_keymap, filename, A, B, C, M, N, K);
    } else if (cmdOptionExists(argv, argv + argc, "-rmat")) {
      char *opt = getCmdOption(argv, argv + argc, "-rmat");
      tiling_type << "RandomSparseMatrix";
      initSpRmat(bc_keymap, opt, A, B, C, M, N, K, seed);
    } else {
      tiling_type << "HardCodedSparseMatrix";
      initSpHardCoded(bc_keymap, A, B, C, M, N, K);
    }

    if (check) {
      // We don't generate the sparse matrices in distributed, so Aref and Bref can
      // just point to the same matrix, or be a local copy.
      Aref = A;
      Bref = B;
    }

    // We still need to build the metadata from the  matrices.
    make_rowidx_to_colidx_from_eigen(A, a_rowidx_to_colidx);
    make_colidx_to_rowidx_from_eigen(A, a_colidx_to_rowidx);
    make_rowidx_to_colidx_from_eigen(B, b_rowidx_to_colidx);
    make_colidx_to_rowidx_from_eigen(B, b_colidx_to_rowidx);
    // This is only needed to compute the flops
    for (int mt = 0; mt < M; mt++) mTiles.emplace_back(1);
    for (int nt = 0; nt < N; nt++) nTiles.emplace_back(1);
    for (int kt = 0; kt < K; kt++) kTiles.emplace_back(1);
#else  // !defined(BLOCK_SPARSE_GEMM)
    if (argc >= 4) {
#ifndef BSPMM_HAS_LIBINT
      std::string Mstr(getCmdOption(argv, argv + argc, "-M"));
      M = parseOption(Mstr, 1200);
      std::string Nstr(getCmdOption(argv, argv + argc, "-N"));
      N = parseOption(Nstr, 1200);
      std::string Kstr(getCmdOption(argv, argv + argc, "-K"));
      K = parseOption(Kstr, 1200);
      std::string minTsStr(getCmdOption(argv, argv + argc, "-t"));
      int minTs = parseOption(minTsStr, 32);
      std::string maxTsStr(getCmdOption(argv, argv + argc, "-T"));
      int maxTs = parseOption(maxTsStr, 256);
      std::string avgStr(getCmdOption(argv, argv + argc, "-a"));
      double avg = parseOption(avgStr, 0.3);
      timing = (check == 0);
      tiling_type << "RandomIrregularTiling";
      initBlSpRandom(bc_keymap, M, N, K, minTs, maxTs, avg, A, B, Aref, Bref, check, mTiles, nTiles, kTiles,
                     a_rowidx_to_colidx, a_colidx_to_rowidx, b_rowidx_to_colidx, b_colidx_to_rowidx, avg_nb, Adensity,
                     Bdensity, seed);
#else
      std::string xyz_filename(getCmdOption(argv, argv + argc, "-y"));
      if (xyz_filename.empty()) throw std::runtime_error("missing -y argument to the libint2-based bspmm example");
      std::ifstream xyz_file(xyz_filename);
      auto atoms = libint2::read_dotxyz(xyz_file);
      std::string basis_name(getCmdOption(argv, argv + argc, "-b"));
      if (basis_name.empty()) basis_name = "cc-pvdz";
      std::string op_param_str(getCmdOption(argv, argv + argc, "-p"));
      auto op_param = parseOption(op_param_str, 1.);
      std::string maxTsStr(getCmdOption(argv, argv + argc, "-T"));
      int maxTs = parseOption(maxTsStr, 256);
      std::string eps_param_str(getCmdOption(argv, argv + argc, "-e"));
      double tile_perelem_2norm_threshold = parseOption(eps_param_str, 1e-5);
      std::cerr << "#Generating matrices with Libint2 on " << xyz_filename << " and " << cores << " cores" << std::endl;
      auto start = std::chrono::high_resolution_clock::now();
      initBlSpLibint2(libint2::Operator::yukawa, libint2::any{op_param}, atoms, basis_name,
                      tile_perelem_2norm_threshold, bc_keymap, maxTs, cores == -1 ? 1 : cores, A, B, Aref, Bref, check,
                      mTiles, nTiles, kTiles, a_rowidx_to_colidx, a_colidx_to_rowidx, b_rowidx_to_colidx,
                      b_colidx_to_rowidx, avg_nb, Adensity, Bdensity);
      auto end = std::chrono::high_resolution_clock::now();
      auto duration = duration_cast<std::chrono::seconds>(end - start);
      std::cerr << "#Generation done (" << duration.count() << "s)" << std::endl;
      tiling_type << xyz_filename << "_" << basis_name << "_" << tile_perelem_2norm_threshold << "_" << op_param;
#endif
      C.resize(A.rows(), B.cols());
    } else {
      tiling_type << "HardCodedBlockSparseMatrix";
      initBlSpHardCoded(bc_keymap, A, B, C, Aref, Bref, true, mTiles, nTiles, kTiles, a_rowidx_to_colidx,
                        a_colidx_to_rowidx, b_rowidx_to_colidx, b_colidx_to_rowidx, M, N, K);
    }
#endif  // !defined(BLOCK_SPARSE_GEMM)

    if (M == -1) M = std::accumulate(mTiles.begin(), mTiles.end(), 0);
    if (N == -1) N = std::accumulate(nTiles.begin(), nTiles.end(), 0);
    if (K == -1) K = std::accumulate(kTiles.begin(), kTiles.end(), 0);

    gflops = compute_gflops(a_rowidx_to_colidx, b_rowidx_to_colidx, mTiles, nTiles, kTiles);

    std::string nbrunStr(getCmdOption(argv, argv + argc, "-n"));
    int nb_runs = parseOption(nbrunStr, 1);

    if (timing) {
      // Start up engine
      ttg_execute(ttg_default_execution_context());
      for (int nrun = 0; nrun < nb_runs; nrun++) {
        timed_measurement(A, B, bc_keymap, tiling_type.str(), gflops, avg_nb, Adensity, Bdensity, a_rowidx_to_colidx,
                          a_colidx_to_rowidx, b_rowidx_to_colidx, b_colidx_to_rowidx, mTiles, nTiles, kTiles, M, N, K,
                          P, Q);
      }
    } else {
      // flow graph needs to exist on every node
      auto rank0_keymap = [](const Key<2> &key) { return 0; };
      Edge<Key<2>> ctl("control");
      Control control(ctl);
      Edge<Key<2>, blk_t> eA, eB, eC;
      Read_SpMatrix a("A", A, ctl, eA, bc_keymap);
      Read_SpMatrix b("B", B, ctl, eB, bc_keymap);
      Write_SpMatrix<> c(C, eC, rank0_keymap);
      auto &c_status = c.status();
      assert(!has_value(c_status));
      //  SpMM a_times_b(world, eA, eB, eC, A, B);
      SpMM<> a_times_b(eA, eB, eC, A, B, a_rowidx_to_colidx, a_colidx_to_rowidx, b_rowidx_to_colidx, b_colidx_to_rowidx,
                       mTiles, nTiles, kTiles, bc_keymap);
      TTGUNUSED(a_times_b);

      if (get_default_world().rank() == 0) std::cout << Dot{}(&a, &b) << std::endl;

      // ready to run!
      auto connected = make_graph_executable(&control);
      assert(connected);
      TTGUNUSED(connected);

      // ready, go! need only 1 kick, so must be done by 1 thread only
      if (ttg_default_execution_context().rank() == 0) control.start(P, Q);

      ttg_execute(ttg_default_execution_context());
      ttg_fence(ttg_default_execution_context());

      // validate C=A*B against the reference output
      assert(has_value(c_status));
      if (ttg_default_execution_context().rank() == 0) {
        std::cout << "Product done, computing locally with Eigen to check" << std::endl;

        SpMatrix<> Cref = Aref * Bref;

        double norm_2_square, norm_inf;
        std::tie(norm_2_square, norm_inf) = norms<blk_t>(Cref - C);
        std::cout << "||Cref - C||_2      = " << std::sqrt(norm_2_square) << std::endl;
        std::cout << "||Cref - C||_\\infty = " << norm_inf << std::endl;
        if (norm_inf > 1e-9) {
          if (Cref.nonZeros() < 100) {
            std::cout << "Cref:\n" << Cref << std::endl;
            std::cout << "C:\n" << C << std::endl;
          }
          ttg_abort();
        }
      }

      // validate Acopy=A against the reference output
      //      assert(has_value(copy_status));
      //      if (ttg_default_execution_context().rank() == 0) {
      //        double norm_2_square, norm_inf;
      //        std::tie(norm_2_square, norm_inf) = norms<blk_t>(Acopy - A);
      //        std::cout << "||Acopy - A||_2      = " << std::sqrt(norm_2_square) << std::endl;
      //        std::cout << "||Acopy - A||_\\infty = " << norm_inf << std::endl;
      //        if (::ttg::tracing()) {
      //          std::cout << "Acopy (" << static_cast<void *>(&Acopy) << "):\n" << Acopy << std::endl;
      //          std::cout << "A (" << static_cast<void *>(&A) << "):\n" << A << std::endl;
      //        }
      //        if (norm_inf != 0) {
      //          ttg_abort();
      //        }
      //      }
    }
  }

  ttg_finalize();

  return 0;
}
