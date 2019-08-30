cmake_minimum_required(VERSION 3.0)

project (camera_daemon)

add_subdirectory(http-parser)
add_subdirectory(libsrtp)

include_directories(/opt/vc/include)
include_directories(/opt/vc/include/interface/vcos)
include_directories(/opt/vc/include/interface/vcos/pthreads)
include_directories(/opt/vc/include/interface/vmcs_host)
include_directories(/opt/vc/include/interface/vmcs_host/linux)

include_directories(http-parser)
include_directories(libsrtp/include)

link_directories(/opt/vc/lib)

set(SOURCES
    "rtpworker.c"
    "util.c"
    "camera_daemon.c"
    )

add_executable (camera_daemon ${SOURCES})

target_link_libraries(camera_daemon http-parser srtp2
mmal_core mmal_util mmal_vc_client vcos bcm_host )

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} \
-D_GNU_SOURCE")
#-g -fsanitize=address \
#-DHOMEKIT_DEBUG")

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
-lpthread ")

