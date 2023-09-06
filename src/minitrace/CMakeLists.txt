########### next target ###############

set(CMAKE_CXX_STANDARD 17)

SET(minitrace_SRCS
  minitrace.cc
)

add_library(minitrace OBJECT ${minitrace_SRCS})
add_sanitizers(minitrace)
set_target_properties(minitrace PROPERTIES COMPILE_FLAGS "-fPIC")

########### install files ###############
