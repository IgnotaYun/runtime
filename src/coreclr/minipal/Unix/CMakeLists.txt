set(SOURCES
    dn-u16.cpp
    dn-stdio.cpp
    memory.cpp
)

if (CLR_CMAKE_TARGET_LIBNX)
  list(APPEND SOURCES doublemapping-libnx.cpp)
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

if (CLR_CMAKE_TARGET_ARCH_WASM)
  install_clr(TARGETS coreclrminipal DESTINATIONS sharedFramework COMPONENT runtime)
endif (CLR_CMAKE_TARGET_ARCH_WASM)
