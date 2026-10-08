set(SO_VERSION 4)

find_package(LibUUID REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL 1.1.0 REQUIRED)
find_package(EXPAT REQUIRED)

# Service discovery is answered directly over multicast DNS in discover.c, so
# avahi-client is not linked and no daemon has to be running on the device.

aux_source_directory(./ GAMESTREAM_SRC_LIST)
aux_source_directory(../third_party/h264bitstream GAMESTREAM_SRC_LIST)

aux_source_directory(../third_party/moonlight-common-c/enet MOONLIGHT_COMMON_SRC_LIST)
aux_source_directory(../third_party/moonlight-common-c/src MOONLIGHT_COMMON_SRC_LIST)
aux_source_directory(../third_party/moonlight-common-c/reedsolomon MOONLIGHT_COMMON_SRC_LIST)

# Static so the finished moonlight executable is one self-contained file with
# nothing of ours to ship beside it.
add_library(moonlight-common STATIC ${MOONLIGHT_COMMON_SRC_LIST})

add_library(gamestream STATIC ${GAMESTREAM_SRC_LIST})
target_link_libraries(gamestream moonlight-common)

set_target_properties(gamestream PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(moonlight-common PROPERTIES VERSION ${PROJECT_VERSION})

target_include_directories(gamestream PRIVATE ../third_party/moonlight-common-c/src ../third_party/h264bitstream ${LibUUID_INCLUDE_DIRS})
target_include_directories(moonlight-common PRIVATE ../third_party/moonlight-common-c/reedsolomon ../third_party/moonlight-common-c/enet/include)
target_link_libraries(gamestream ${CURL_LIBRARIES} ${OPENSSL_LIBRARIES} ${EXPAT_LIBRARIES} ${LibUUID_LIBRARIES})

target_link_libraries(gamestream ${CMAKE_THREAD_LIBS_INIT} ${CMAKE_DL_LIBS})

install(TARGETS gamestream moonlight-common DESTINATION ${CMAKE_INSTALL_LIBDIR})
