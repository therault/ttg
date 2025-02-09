include(AddTTGExecutable)

add_ttg_executable(test test/test.cc)
add_ttg_executable(t9 t9/t9.cc)
add_ttg_executable(t9-streaming t9/t9_streaming.cc)

# sparse matmul
if (TARGET eigen3)
    # MADworld used for MADNESS serialization
    add_ttg_executable(spmm spmm/spmm.cc LINK_LIBRARIES eigen3)
    # block-sparse needs BTAS
    if (TARGET BTAS::BTAS)
        # since only need to use matrices, limit BTAS_TARGET_MAX_INDEX_RANK to 2
        add_ttg_executable(bspmm spmm/spmm.cc LINK_LIBRARIES eigen3 BTAS Boost::boost COMPILE_DEFINITIONS BLOCK_SPARSE_GEMM=1;BTAS_TARGET_MAX_INDEX_RANK=2)
    endif (TARGET BTAS::BTAS)
endif(TARGET eigen3)

if (TARGET MADworld)
  add_ttg_executable(madness-1d madness/madness-1d/madness-1d.cc RUNTIMES "mad")
  if (TARGET blaspp) #(CBLAS_FOUND AND MKL_FOUND)
    add_ttg_executable(mrattg madness/mrattg.cc mragl.cc mratwoscale.cc mradomain.h mrafunctiondata.h mrafunctionfunctor.h mrafunctionnode.h mragl.h mrahash.h mrakey.h mramisc.h mramxm.h mrarange.h mrasimpletensor.h mratwoscale.h mratypes.h LINK_LIBRARIES blaspp MADworld)

    add_ttg_executable(mrattg-streaming madness/mrattg_streaming.cc mragl.cc mratwoscale.cc mradomain.h mrafunctiondata.h mrafunctionfunctor.h mrafunctionnode.h mragl.h mrahash.h mrakey.h mramisc.h mramxm.h mrarange.h mrasimpletensor.h mratwoscale.h mratypes.h LINK_LIBRARIES blaspp MADworld)
  endif ()
endif (TARGET MADworld)

add_ttg_executable(wavefront-wf wavefront/wavefront-wf.cc SINGLERANKONLY)
add_ttg_executable(wavefront-wf2 wavefront/wavefront-wf2.cc SINGLERANKONLY)
add_ttg_executable(wavefront-df wavefront/wavefront-df.cc)
add_ttg_executable(wavefront-pull wavefront/wavefront-pull.cc LINK_LIBRARIES MADworld)
add_ttg_executable(fw-apsp floyd-warshall/floyd_warshall.cc LINK_LIBRARIES MADworld SINGLERANKONLY)
add_ttg_executable(helloworld helloworld/helloworld.cpp)
add_ttg_executable(simplegenerator simplegenerator/simplegenerator.cc RUNTIMES "mad")

add_ttg_executable(testing_dpotrf potrf/testing_dpotrf.cc LINK_LIBRARIES lapackpp)
add_ttg_executable(testing_dtrtri potrf/testing_dtrtri.cc LINK_LIBRARIES lapackpp)
add_ttg_executable(testing_dlauum potrf/testing_dlauum.cc LINK_LIBRARIES lapackpp)
add_ttg_executable(testing_dpoinv potrf/testing_dpoinv.cc LINK_LIBRARIES lapackpp)

if (OpenMP_CXX_FOUND AND TARGET std::execution)
        add_ttg_executable(fw-apsp-df floyd-warshall/floyd_warshall_df.cc LINK_LIBRARIES OpenMP::OpenMP_CXX std::execution MADworld)
endif ()
add_ttg_executable(ge ge/ge.cc SINGLERANKONLY)
if (TARGET std::execution)
        add_ttg_executable(ge-df ge/ge_df.cc LINK_LIBRARIES std::execution MADworld)
endif (TARGET std::execution)
add_ttg_executable(sw sw/sw.cc)

# RandomAccess HPCC Benchmark
if (TARGET MADworld)
  add_ttg_executable(randomaccess randomaccess/randomaccess.cc RUNTIMES "mad")
endif (TARGET MADworld)
