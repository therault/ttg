#include <ttg.h>

#include "plgsy.h"
#include "pmw.h"
#include "potrf.h"
#include "result.h"

#include <iomanip>

char* getCmdOption(char ** begin, char ** end, const std::string & option)
{
    char ** itr = std::find(begin, end, option);
    if (itr != end && ++itr != end)
    {
        return *itr;
    }
    return nullptr;
}

bool cmdOptionExists(char** begin, char** end, const std::string& option)
{
    return std::find(begin, end, option) != end;
}

int check_dpotrf( double *A, double *A0, int N );

int main(int argc, char **argv)
{

  std::chrono::time_point<std::chrono::high_resolution_clock> beg, end;
  int NB = 32;
  int N = 5*NB;
  int M = N;
  int nthreads = -1;
  const char* prof_filename = nullptr;
  char *opt = nullptr;
  int ret = EXIT_SUCCESS;

  if( (opt = getCmdOption(argv+1, argv+argc, "-N")) != nullptr ) {
    N = M = atoi(opt);
  }

  if( (opt = getCmdOption(argv+1, argv+argc, "-t")) != nullptr ) {
    NB = atoi(opt);
  }

  if( (opt = getCmdOption(argv+1, argv+argc, "-c")) != nullptr ) {
    nthreads = atoi(opt);
  }

  if( (opt = getCmdOption(argv+1, argv+argc, "-dag")) != nullptr ) {
    prof_filename = opt;
  }

  bool check = !cmdOptionExists(argv+1, argv+argc, "-x");
  bool cow_hint = !cmdOptionExists(argv+1, argv+argc, "-w");

  ttg::initialize(argc, argv, nthreads);

  auto world = ttg::default_execution_context();
  if(nullptr != prof_filename) {
    world.profile_on();
    world.dag_on(prof_filename);
  }

  int P = std::sqrt(world.size());
  int Q = (world.size() + P - 1)/P;

  if(check && (P>1 || Q>1)) {
    std::cerr << "Check is disabled for distributed runs at this time" << std::endl;
    check = false;
  }

  static_assert(ttg::has_split_metadata<MatrixTile<double>>::value);

  std::cout << "Creating 2D block cyclic matrix with NB " << NB << " N " << N << " M " << M << " P " << P << std::endl;

  sym_two_dim_block_cyclic_t dcA;
  sym_two_dim_block_cyclic_init(&dcA, matrix_type::matrix_RealDouble,
                                world.size(), world.rank(), NB, NB, N, M,
                                0, 0, N, M, P, matrix_Lower);
  dcA.mat = parsec_data_allocate((size_t)dcA.super.nb_local_tiles *
                                 (size_t)dcA.super.bsiz *
                                 (size_t)parsec_datadist_getsizeoftype(dcA.super.mtype));
  parsec_data_collection_set_key((parsec_data_collection_t*)&dcA, (char*)"Matrix A");

  if(!check) {
    ttg::Edge<Key2, void> startup("startup");
    ttg::Edge<Key2, MatrixTile<double>> topotrf("To POTRF");
    ttg::Edge<Key2, MatrixTile<double>> result("To result");

    //Matrix<double>* A = new Matrix<double>(n_rows, n_cols, NB, NB);
    MatrixT<double> A{&dcA};
    /* TODO: initialize the matrix */
    /* This works only with the parsec backend! */
    int random_seed = 3872;

    auto init_tt =  ttg::make_tt<void>([&](std::tuple<ttg::Out<Key2, void>>& out) {
      for(int i = 0; i < A.rows(); i++) {
        for(int j = 0; j <= i && j < A.cols(); j++) {
          if(A.is_local(i, j)) {
            if(ttg::tracing()) ttg::print("init(", Key2{i, j}, ")");
            ttg::sendk<0>(Key2{i, j}, out);
          }
        }
      }
    }, ttg::edges(), ttg::edges(startup), "Startup Trigger", {}, {"startup"});
    init_tt->set_keymap([&]() {return world.rank();});

    auto plgsy_ttg = make_plgsy_ttg(A, N, random_seed, startup, topotrf, cow_hint);
    auto potrf_ttg = potrf::make_potrf_ttg(A, topotrf, result, cow_hint);
    auto result_ttg = make_result_ttg(A, result, cow_hint);

    auto connected = make_graph_executable(init_tt.get());
    assert(connected);
    TTGUNUSED(connected);
    std::cout << "Graph is connected: " << connected << std::endl;

    if (world.rank() == 0) {
      std::cout << "==== begin dot ====\n";
      std::cout << ttg::Dot()(init_tt.get()) << std::endl;
      std::cout << "==== end dot ====\n";
      beg = std::chrono::high_resolution_clock::now();
    }
    init_tt->invoke();

    ttg::execute(world);
    ttg::fence(world);
    if (world.rank() == 0) {
      end = std::chrono::high_resolution_clock::now();
      auto elapsed = (std::chrono::duration_cast<std::chrono::microseconds>(end - beg).count());
      end = std::chrono::high_resolution_clock::now();
      std::cout << "TTG Execution Time (milliseconds) : "
                << elapsed / 1E3 << " : Flops " << (potrf::FLOPS_DPOTRF(N)) << " " << (potrf::FLOPS_DPOTRF(N)/1e9)/(elapsed/1e6) << " GF/s" << std::endl;
    }

    world.dag_off();
  } else {
    ttg::Edge<Key2, void> startup("startup");
    ttg::Edge<Key2, MatrixTile<double>> result("To result");

    MatrixT<double> A{&dcA};
    int random_seed = 3872;

    auto init_tt =  ttg::make_tt<void>([&](std::tuple<ttg::Out<Key2, void>>& out) {
      for(int i = 0; i < A.rows(); i++) {
        for(int j = 0; j <= i && j < A.cols(); j++) {
          if(A.is_local(i, j)) {
            if(ttg::tracing()) ttg::print("init(", Key2{i, j}, ")");
            ttg::sendk<0>(Key2{i, j}, out);
          }
        }
      }
    }, ttg::edges(), ttg::edges(startup), "Startup Trigger", {}, {"startup"});
    init_tt->set_keymap([&]() {return world.rank();});

    auto plgsy_ttg = make_plgsy_ttg(A, N, random_seed, startup, result, cow_hint);
    auto result_ttg = make_result_ttg(A, result, cow_hint);

    auto connected = make_graph_executable(init_tt.get());
    assert(connected);
    TTGUNUSED(connected);

    init_tt->invoke();

    ttg::execute(world);
    ttg::fence(world);

    double *A0 = A.getLAPACKMatrix();

    ttg::Edge<Key2, MatrixTile<double>> topotrf("To POTRF");
    ttg::Edge<Key2, MatrixTile<double>> toresult("To Result");

    auto load_plgsy = make_load_tt(A, topotrf, cow_hint);
    auto potrf_ttg = potrf::make_potrf_ttg(A, topotrf, toresult, cow_hint);
    auto result2_ttg = make_result_ttg(A, toresult, cow_hint);

    connected = make_graph_executable(load_plgsy.get());
    assert(connected);
    TTGUNUSED(connected);

    load_plgsy->invoke();

    ttg::fence(world);

    /* Copy result matrix (which is local) into a single LAPACK format matrix */
    double *Acpy = A.getLAPACKMatrix();
    if(-1 == check_dpotrf(Acpy, A0, N)) {
      if(N < 32) {
        print_LAPACK_matrix(A0, N, "Original");
        auto info = lapack::potrf(lapack::Uplo::Lower, N, A0, N);
        print_LAPACK_matrix(A0, N, "lapack::potrf(Original)");
        print_LAPACK_matrix(Acpy, N, "ttg::potrf(Original)");
      }
      ret = EXIT_FAILURE;
    }

    delete [] Acpy;
    delete [] A0;

  }

  /* cleanup allocated matrix before shutting down PaRSEC */
  parsec_data_free(dcA.mat); dcA.mat = NULL;
  parsec_tiled_matrix_dc_destroy( (parsec_tiled_matrix_dc_t*)&dcA);

  world.profile_off();

  ttg::finalize();
  return ret;
}

int check_dpotrf( double *A, double *A0, int N )
{
    int ret;
    double Rnorm = 0.0;
    double Anorm = 0.0;
    double result = 0.0;
    double eps = std::numeric_limits< double >::epsilon();

    /* Copy A in LL', setting upper part of LL' to 0 */
    double *LLt = new double[N*N];
    for(int i = 0; i < N; i++) {
      for(int j = 0; j < N; j++) {
        if(j <= i) {
          LLt[i+j*N] = A[i+j*N];
        } else {
          LLt[i+j*N] = 0.0;
        }
      }
    }

    /* Compute LL' */
    blas::trmm( blas::Layout::ColMajor, blas::Side::Right, lapack::Uplo::Lower, blas::Op::ConjTrans, blas::Diag::NonUnit,
                N, N, 1.0, A, N, LLt, N);

    /* compute LL' <- LL' - A0, knowing that LLt and A0 are symmetric, because lange does not do triangular matrices */
    for(int i = 0; i < N; i++) {
      for(int j = 0; j < N; j++) {
        if(j <= i) {
          LLt[i+j*N] -= A0[i+j*N];
        } else {
          LLt[i+j*N]  = LLt[j+i*N] - A0[j+i*N];
        }
      }
    }

    Anorm = lapack::lange(lapack::Norm::Inf, N, N, A0, N);
    Rnorm = lapack::lange(lapack::Norm::Inf, N, N, LLt, N);;

    result = Rnorm / ( Anorm * N * eps ) ;

    std::cout << "============" << std::endl;
    std::cout << "Checking the Cholesky factorization" << std::endl;
    std::cout <<  "-- ||A||_oo = " << Anorm << ", ||LL'-A||_oo = " << Rnorm << std::endl;
    std::cout << "-- ||LL'-A||_oo/(||A||_oo.N.eps) = " << result << std::endl;

    if ( std::isnan(Rnorm)  || std::isinf(Rnorm)  ||
         std::isnan(result) || std::isinf(result) ||
         (result > 60.0) ) {
        std::cout << "-- Factorization is suspicious !" << std::endl;
        ret = -1;
    } else {
        std::cout << "-- Factorization is CORRECT !" << std::endl;
        ret = 0;
    }

    delete [] LLt;

    return ret;
}
