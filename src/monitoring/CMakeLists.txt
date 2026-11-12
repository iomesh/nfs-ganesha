########### next target ###############

set(CMAKE_CXX_STANDARD 17)

SET(monitoring_SRCS
  monitoring.cc
  observation_metrics.cc
)

if(USE_PERF_MONITORING)
  list(APPEND monitoring_SRCS monitoring_config.c)
endif()

add_library(monitoring OBJECT ${monitoring_SRCS})
add_sanitizers(monitoring)
set_target_properties(monitoring PROPERTIES COMPILE_FLAGS "-fPIC")

########### install files ###############
