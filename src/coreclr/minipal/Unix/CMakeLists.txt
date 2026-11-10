set(SOURCES
    dn-u16.cpp
)

if(CLR_CMAKE_TARGET_HORIZ)
  list(APPEND SOURCES doublemapping-horiz.cpp)
  # horiz_host.h declares the functions exported to the application
  include_directories(${CLR_DIR}/pal/inc)
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
