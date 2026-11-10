set(SOURCES
    dn-u16.cpp
)

if(CLR_CMAKE_TARGET_HORIZ)
  set(CODESLAB_DIR "" CACHE PATH "Path to codeslab repository")
  if(NOT CODESLAB_DIR)
    if(DEFINED ENV{CODESLAB_ROOT} AND EXISTS "$ENV{CODESLAB_ROOT}/include/codeslab/codeslab.h")
      set(CODESLAB_DIR "$ENV{CODESLAB_ROOT}")
    elseif(EXISTS "${CLR_REPO_ROOT_DIR}/../../codeslab/include/codeslab/codeslab.h")
      set(CODESLAB_DIR "${CLR_REPO_ROOT_DIR}/../../codeslab")
    elseif(EXISTS "${CLR_REPO_ROOT_DIR}/../codeslab/include/codeslab/codeslab.h")
      set(CODESLAB_DIR "${CLR_REPO_ROOT_DIR}/../codeslab")
    elseif(DEFINED ENV{HOME} AND EXISTS "$ENV{HOME}/src/codeslab/include/codeslab/codeslab.h")
      set(CODESLAB_DIR "$ENV{HOME}/src/codeslab")
    endif()
  endif()
  if(NOT EXISTS "${CODESLAB_DIR}/include/codeslab/codeslab.h")
    message(FATAL_ERROR "codeslab not found at '${CODESLAB_DIR}'; pass -DCODESLAB_DIR=/path/to/codeslab")
  endif()
  list(APPEND SOURCES doublemapping-horiz.cpp "${CODESLAB_DIR}/src/codeslab.c")
  # horiz_host.h declares the functions exported to the application
  include_directories(${CLR_DIR}/pal/inc "${CODESLAB_DIR}/include")
else()
  list(APPEND SOURCES doublemapping.cpp)
endif()

add_library(coreclrminipal_objects
    OBJECT
    ${SOURCES}
)

add_library(coreclrminipal
    STATIC
    $<TARGET_OBJECTS:coreclrminipal_objects>
)

target_link_libraries(coreclrminipal PRIVATE minipal)
