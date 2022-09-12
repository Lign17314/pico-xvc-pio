cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD "pico")

#set(CMAKE_BUILD_TYPE "Debug")

include(pico_sdk_import.cmake)
project(test)
add_executable(test)

pico_sdk_init()
set(TOP ${PICO_SDK_PATH})
SET(FREERTOS_KERNEL_PATH "${PICO_SDK_PATH}/lib/FreeRTOS-Kernel")

if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    message("Skipping Pico W FreeRTOS examples as FREERTOS_KERNEL_PATH not defined")
else()

    include(FreeRTOS_Kernel_import.cmake)
    set(TOP ${PICO_SDK_PATH})
    include(${TOP}/lib/tinyusb/hw/bsp/family_support.cmake)

    #add_subdirectory(iperf)
    #add_subdirectory(ping)
    pico_enable_stdio_usb(test 0)
    pico_enable_stdio_uart(test 1)

    pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/tdata.pio)

    target_compile_definitions(test PRIVATE
    NO_SYS=0            # don't want NO_SYS (generally this would be in your lwipopts.h)
    LWIP_SOCKET=1
    LWIP_PROVIDE_ERRNO=1
    #LWIP_NETCONN=1         
    #LWIP_NETIF_API=1  
    LWIP_IPV4=1 
    #LWIP_TCP=1
    DEFAULT_TCP_RECVMBOX_SIZE=12   
    DEFAULT_ACCEPTMBOX_SIZE=12
    TCPIP_THREAD_PRIO=4
    #LIB_CMSIS_CORE =1
    LWIP_DBG_LEVEL=1
    #LWIP_DEBUG =1
    mainRUN_FREE_RTOS_ON_CORE=1
    PICO_STACK_SIZE=0x1000
    PICO_STDIO_STACK_BUFFER_SIZE=64 # use a small printf on stack buffer
    )
    target_include_directories(test PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        #${PICO_LWIP_CONTRIB_PATH}/../src/include
        ${PICO_LWIP_CONTRIB_PATH}/apps/ping
        ${TOP}/lib/lwip/src/include/
        #${PICO_LWIP_CONTRIB_PATH}/ports/freertos
        ${TOP}/lib/tinyusb/lib/networking
        )
    
    target_sources(test PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/freertos_hook.c
        ${CMAKE_CURRENT_SOURCE_DIR}/usb_descriptors.c
        ${CMAKE_CURRENT_SOURCE_DIR}/pio_xfer.c
        ${PICO_LWIP_CONTRIB_PATH}/apps/ping/ping.c
        ${TOP}/lib/tinyusb/lib/networking/dhserver.c
        ${TOP}/lib/tinyusb/lib/networking/dnserver.c
        ${TOP}/lib/tinyusb/lib/networking/rndis_reports.c
        )
    target_link_libraries(test PUBLIC 
        pico_stdlib 
        hardware_pio 
        cmsis_core
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        pico_lwip_iperf
        pico_lwip
        pico_lwip_contrib_freertos
        tinyusb_device 
        tinyusb_board  
        )
        pico_add_extra_outputs(test)
endif()