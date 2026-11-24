########### next target ###############

set(CMAKE_CXX_STANDARD 17)

SET(monitoring_SRCS
  monitoring.cc
)

if(ENABLE_FAULT_INJECTION)
  # Test builds: the combined civetweb server that serves /metrics and
  # /debug/failpoints (drives the pure-C registry in support/fail_inject.c).
  set(monitoring_SRCS
    ${monitoring_SRCS}
    fault_control.cc
    )
endif(ENABLE_FAULT_INJECTION)

add_library(monitoring OBJECT ${monitoring_SRCS})
add_sanitizers(monitoring)
set_target_properties(monitoring PROPERTIES COMPILE_FLAGS "-fPIC")

########### install files ###############
