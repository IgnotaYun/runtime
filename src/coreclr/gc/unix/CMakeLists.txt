set(CMAKE_INCLUDE_CURRENT_DIR ON)
include_directories("../env")
include_directories("..")
if(CLR_CMAKE_TARGET_HORIZ)
  # horiz_host.h declares the memory range the application provides
  include_directories(${CLR_DIR}/pal/inc)
endif()

include(configure.cmake)

set(GC_PAL_SOURCES
    gcenv.unix.cpp
    numasupport.cpp
    events.cpp
    cgroup.cpp)

add_library(gc_pal OBJECT ${GC_PAL_SOURCES} ${VERSION_FILE_PATH})
