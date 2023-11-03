########### next target ###############

set(CMAKE_CXX_STANDARD 17)

if(USE_DBUS)
  include_directories(
    ${DBUS_INCLUDE_DIRS}
    )
endif(USE_DBUS)

SET(minitrace_SRCS
  minitrace.cc
  minitrace_config.c
)

add_library(minitrace OBJECT ${minitrace_SRCS})
add_sanitizers(minitrace)
set_target_properties(minitrace PROPERTIES COMPILE_FLAGS "-fPIC")

########### install files ###############
