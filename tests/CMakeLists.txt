project(tests C)

include_directories(
  ${CMAKE_BINARY_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src
  ${CMOCKA_INCLUDE_DIR}
)

set(TORTURE_LIBRARY torture)

# RFC862 echo server
add_executable(echo_srv echo_srv.c)
target_compile_options(echo_srv
                       PRIVATE
                           ${DEFAULT_C_COMPILE_FLAGS}
                           -D_GNU_SOURCE)
target_link_libraries(echo_srv ${SWRAP_REQUIRED_LIBRARIES})
if (DEFINED DEFAULT_LINK_FLAGS)
    set_target_properties(echo_srv
                          PROPERTIES
                              LINK_FLAGS ${DEFAULT_LINK_FLAGS})
endif()

add_library(${TORTURE_LIBRARY} STATIC torture.c)
target_compile_options(${TORTURE_LIBRARY}
                       PRIVATE
                           ${DEFAULT_C_COMPILE_FLAGS}
                           -D_GNU_SOURCE)
target_link_libraries(${TORTURE_LIBRARY}
    ${CMOCKA_LIBRARY}
    ${SWRAP_REQUIRED_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT})

set(SWRAP_THREADED_TESTS
    test_thread_sockets
    test_thread_echo_tcp_connect
    test_thread_echo_tcp_write_read
    test_thread_echo_tcp_sendmsg_recvmsg
    test_thread_echo_udp_send_recv)

set(SWRAP_TESTS
    test_ioctl
    test_tcp_listen
    test_tcp_dup2
    test_fcntl
    test_echo_tcp_connect
    test_echo_tcp_bind
    test_echo_tcp_socket_options
    test_echo_tcp_sendmsg_recvmsg
    test_echo_tcp_write_read
    test_echo_tcp_writev_readv
    test_echo_tcp_get_peer_sock_name
    test_echo_udp_sendto_recvfrom
    test_echo_udp_send_recv
    test_echo_udp_sendmsg_recvmsg
    test_swrap_unit
    test_max_sockets
    test_close_failure
    test_tcp_socket_overwrite
    ${SWRAP_THREADED_TESTS})

if (HAVE_STRUCT_MSGHDR_MSG_CONTROL)
    set(SWRAP_TESTS ${SWRAP_TESTS} test_sendmsg_recvmsg_fd)
endif (HAVE_STRUCT_MSGHDR_MSG_CONTROL)

function(ADD_CMOCKA_TEST_ENVIRONMENT _TEST_NAME)
    if (CMAKE_BUILD_TYPE)
        string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LOWER)
        if (CMAKE_BUILD_TYPE_LOWER STREQUAL "addresssanitizer")
            find_library(ASAN_LIBRARY
                         NAMES asan)
            if (NOT ASAN_LIBRARY)
                foreach(version RANGE 10 1)
                    if (NOT ASAN_LIBRARY)
                        find_library(ASAN_LIBRARY libasan.so.${version})
                    endif()
                endforeach()
            endif()
        endif()
    endif()

    if (ASAN_LIBRARY)
        list(APPEND PRELOAD_LIBRARIES ${ASAN_LIBRARY})
    endif()
    list(APPEND PRELOAD_LIBRARIES ${SOCKET_WRAPPER_LOCATION})

    if (OSX)
        set(TORTURE_ENVIRONMENT "DYLD_FORCE_FLAT_NAMESPACE=1;DYLD_INSERT_LIBRARIES=${SOCKET_WRAPPER_LOCATION}")
    else ()
        string(REPLACE ";" ":" _TMP_ENV "${PRELOAD_LIBRARIES}")
        set(TORTURE_ENVIRONMENT "LD_PRELOAD=${_TMP_ENV}")
    endif()

    set_property(TEST
                    ${_TEST_NAME}
                PROPERTY
                    ENVIRONMENT "${TORTURE_ENVIRONMENT}")
endfunction()

foreach(_SWRAP_TEST ${SWRAP_TESTS})
    add_cmocka_test(${_SWRAP_TEST}
                    SOURCES ${_SWRAP_TEST}.c
                    COMPILE_OPTIONS ${DEFAULT_C_COMPILE_FLAGS} -D_GNU_SOURCE
                    LINK_LIBRARIES ${TORTURE_LIBRARY}
                    LINK_OPTIONS ${DEFAULT_LINK_FLAGS})
    add_cmocka_test_environment(${_SWRAP_TEST})
endforeach()

if (HELGRIND_TESTING)
    find_program(VALGRIND_EXECUTABLE valgrind)
    if (VALGRIND_EXECUTABLE)
        set(VALGRIND_HELGRIND_OPTIONS -v --trace-children=yes --tool=helgrind --error-exitcode=1 --read-var-info=yes --suppressions=${CMAKE_CURRENT_SOURCE_DIR}/helgrind.supp)

        foreach(_TEST ${SWRAP_THREADED_TESTS})
            set(_HELGRIND_TEST "helgrind_${_TEST}")

            add_test(NAME ${_HELGRIND_TEST} COMMAND ${VALGRIND_EXECUTABLE} ${VALGRIND_HELGRIND_OPTIONS} ${CMAKE_CURRENT_BINARY_DIR}/${_TEST})
            if (OSX)
                set_property(
                    TEST
                        ${_HELGRIND_TEST}
                    PROPERTY
                        ENVIRONMENT DYLD_FORCE_FLAT_NAMESPACE=1;DYLD_INSERT_LIBRARIES=${SOCKET_WRAPPER_LOCATION})
            else ()
                set_property(
                    TEST
                        ${_HELGRIND_TEST}
                    PROPERTY
                        ENVIRONMENT LD_PRELOAD=${SOCKET_WRAPPER_LOCATION} SOCKET_WRAPPER_DISABLE_DEEPBIND=1)
            endif()
        endforeach()
    endif()
endif()

# test_fork_pthread
add_library(thread_deadlock SHARED thread_deadlock.c)
target_link_libraries(thread_deadlock ${CMAKE_THREAD_LIBS_INIT})
target_compile_options(thread_deadlock PRIVATE ${DEFAULT_C_COMPILE_FLAGS})

add_cmocka_test(test_fork_thread_deadlock
                SOURCES test_fork_thread_deadlock.c
                COMPILE_OPTIONS ${DEFAULT_C_COMPILE_FLAGS} -D_GNU_SOURCE
                LINK_LIBRARIES ${TORTURE_LIBRARY} thread_deadlock
                LINK_OPTIONS ${DEFAULT_LINK_FLAGS})
add_cmocka_test_environment(test_fork_thread_deadlock)
