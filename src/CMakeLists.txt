project(libsocket_wrapper C)

add_library(socket_wrapper SHARED socket_wrapper.c)
target_include_directories(socket_wrapper
                           PRIVATE
                               ${CMAKE_BINARY_DIR})
target_compile_options(socket_wrapper
                       PRIVATE
                          ${DEFAULT_C_COMPILE_FLAGS}
                          -D_GNU_SOURCE)
target_link_libraries(socket_wrapper ${SWRAP_REQUIRED_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

set_target_properties(socket_wrapper
                      PROPERTIES
                          VERSION ${LIBRARY_VERSION}
                          SOVERSION ${LIBRARY_SOVERSION})
if (DEFINED DEFAULT_LINK_FLAGS)
    set_target_properties(socket_wrapper
                          PROPERTIES
                              LINK_FLAGS ${DEFAULT_LINK_FLAGS})
endif()

install(TARGETS socket_wrapper
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

set(SOCKET_WRAPPER_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}socket_wrapper${CMAKE_SHARED_LIBRARY_SUFFIX}" PARENT_SCOPE)
