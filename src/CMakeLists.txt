project(libsocket_wrapper C)

include_directories(${CMAKE_BINARY_DIR})

add_library(socket_wrapper SHARED socket_wrapper.c)

target_compile_options(socket_wrapper
                       PRIVATE
                          ${DEFAULT_C_COMPILE_FLAGS}
                          -D_GNU_SOURCE)
target_link_libraries(socket_wrapper ${SWRAP_REQUIRED_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

install(
  TARGETS
    socket_wrapper
  RUNTIME DESTINATION ${BIN_INSTALL_DIR}
  LIBRARY DESTINATION ${LIB_INSTALL_DIR}
  ARCHIVE DESTINATION ${LIB_INSTALL_DIR}
)

set_target_properties(
  socket_wrapper
    PROPERTIES
      VERSION
        ${LIBRARY_VERSION}
      SOVERSION
        ${LIBRARY_SOVERSION}
)

set(SOCKET_WRAPPER_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_LIBRARY_PREFIX}socket_wrapper${CMAKE_SHARED_LIBRARY_SUFFIX}" PARENT_SCOPE)
