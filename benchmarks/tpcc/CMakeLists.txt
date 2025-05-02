set_property(GLOBAL APPEND PROPERTY ALL_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/tpcc-config.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../bench.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../dbtest.cc
)

add_executable(tpcc_SI_sequential ${CMAKE_CURRENT_SOURCE_DIR}/tpcc-sequential.cc)
set_target_properties(tpcc_SI_sequential PROPERTIES COMPILE_FLAGS "")
target_link_libraries(tpcc_SI_sequential preemptdb_si thread_pool)
