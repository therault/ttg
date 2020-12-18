#
# MADNESS Testings
#

if (TARGET MADworld)
    message(STATUS "Defining MADNESS tests")
    add_test(shm_basic_test_mad ${SHM_TEST_CMD_LIST} ./test-mad)

    add_test(mpi_basic_test_mad ${MPI_TEST_CMD_LIST} 3 ./test-mad)
else()
    message(WARNING "No TARGET MADworld: no tests for MADNESS")
endif()

#
# PaRSEC Testings
#

if (TARGET parsec)
    message(STATUS "Defining PaRSEC shared memory tests")
    add_test(shm_basic_test_parsec ${SHM_TEST_CMD_LIST} ./test-parsec)

    if(PaRSEC::PARSEC_HAVE_MPI)
        message(STATUS "Defining PaRSEC MPI tests")
        add_test(mpi_basic_test_parsec ${MPI_TEST_CMD_LIST} 3 ./test-parsec)
    else()
        message(WARNING "PaRSEC::PARSEC_HAVE_MPI not defined")
    endif()
else()
    message(WARNING "NO parsec TARGET: no tests for PaRSEC")
endif()

