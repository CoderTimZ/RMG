#
# RMG-Netplay-New CMakeLists.txt
#
# The new netplay: the session, the protocol and the network.
# No Qt, so a future traversal or relay server can reuse it.
#

# zpl-c/enet: ENet as a single header, with IPv6 (dual-stack) support
set(ENET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdParty/enet)

add_library(enet STATIC ${ENET_DIR}/enet.c)
target_include_directories(enet PUBLIC ${ENET_DIR})
set_target_properties(enet PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm iphlpapi)
endif(WIN32)

# miniupnpc (UPnP-IGD) and libnatpmp (NAT-PMP), for asking the host's router
# to forward the netplay port
set(MINIUPNPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdParty/miniupnpc)
add_library(miniupnpc STATIC
    ${MINIUPNPC_DIR}/src/addr_is_reserved.c
    ${MINIUPNPC_DIR}/src/connecthostport.c
    ${MINIUPNPC_DIR}/src/igd_desc_parse.c
    ${MINIUPNPC_DIR}/src/minisoap.c
    ${MINIUPNPC_DIR}/src/minissdpc.c
    ${MINIUPNPC_DIR}/src/miniupnpc.c
    ${MINIUPNPC_DIR}/src/miniwget.c
    ${MINIUPNPC_DIR}/src/minixml.c
    ${MINIUPNPC_DIR}/src/portlistingparse.c
    ${MINIUPNPC_DIR}/src/receivedata.c
    ${MINIUPNPC_DIR}/src/upnpcommands.c
    ${MINIUPNPC_DIR}/src/upnpdev.c
    ${MINIUPNPC_DIR}/src/upnperrors.c
    ${MINIUPNPC_DIR}/src/upnpreplyparse.c
)
target_include_directories(miniupnpc PUBLIC ${MINIUPNPC_DIR}/include PRIVATE ${MINIUPNPC_DIR}/src)
target_compile_definitions(miniupnpc PUBLIC MINIUPNP_STATICLIB)
set_target_properties(miniupnpc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (WIN32)
    target_link_libraries(miniupnpc PUBLIC ws2_32 iphlpapi)
else(WIN32)
    target_compile_definitions(miniupnpc PRIVATE
        MINIUPNPC_SET_SOCKET_TIMEOUT MINIUPNPC_GET_SRC_ADDR _BSD_SOURCE _DEFAULT_SOURCE)
    if (NOT APPLE)
        target_compile_definitions(miniupnpc PRIVATE _XOPEN_SOURCE=600)
    endif()
endif(WIN32)

set(LIBNATPMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rdParty/libnatpmp)
set(LIBNATPMP_SOURCES ${LIBNATPMP_DIR}/natpmp.c ${LIBNATPMP_DIR}/getgateway.c)
if (WIN32)
    list(APPEND LIBNATPMP_SOURCES ${LIBNATPMP_DIR}/wingettimeofday.c)
endif(WIN32)
add_library(natpmp STATIC ${LIBNATPMP_SOURCES})
target_include_directories(natpmp PUBLIC ${LIBNATPMP_DIR})
target_compile_definitions(natpmp PUBLIC ENABLE_STRNATPMPERR)
set_target_properties(natpmp PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (WIN32)
    target_compile_definitions(natpmp PUBLIC NATPMP_STATICLIB)
    target_link_libraries(natpmp PUBLIC ws2_32 iphlpapi)
endif(WIN32)

add_library(RMG-Netplay-New STATIC
    Transport.cpp
    Session.cpp
    Nat.cpp
)

set_target_properties(RMG-Netplay-New PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(RMG-Netplay-New PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(RMG-Netplay-New PUBLIC enet PRIVATE miniupnpc natpmp)

find_package(Threads REQUIRED)
target_link_libraries(RMG-Netplay-New PUBLIC Threads::Threads)

# a loopback test of the session: a host, a player and a spectator in one
# process (run it with RMG_NETPLAY_NEW_DROP_PERCENT=30 to drop input packets)
option(NETPLAY_NEW_TESTS "Builds the new netplay's tests" OFF)
if (NETPLAY_NEW_TESTS)
    add_executable(RMG-Netplay-New-Test Tests/SessionTest.cpp)
    set_target_properties(RMG-Netplay-New-Test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(RMG-Netplay-New-Test RMG-Netplay-New)
endif(NETPLAY_NEW_TESTS)
