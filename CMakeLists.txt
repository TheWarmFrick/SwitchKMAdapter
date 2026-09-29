cmake_minimum_required(VERSION 3.13)

# Default to pico_w (or pico for USB Host)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w)
endif()

include(pico_sdk_import.cmake)

# Input Backend selection: "USB_HOST" (default) or "WIRELESS_BT"
set(ADAPTER_INPUT_BACKEND "USB_HOST" CACHE STRING "Input backend: USB_HOST or WIRELESS_BT")

if(ADAPTER_INPUT_BACKEND STREQUAL "USB_HOST")
    # Pico-PIO-USB (USB-A host port on GPIO pins, default D+ = GP2, D- = GP3)
    if (NOT DEFINED PICO_PIO_USB_PATH)
        if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB/src/pio_usb.c)
            set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/Pico-PIO-USB)
        else()
            include(FetchContent)
            FetchContent_Declare(
                pico_pio_usb_fetch
                GIT_REPOSITORY https://github.com/sekigon-gonnoc/Pico-PIO-USB
                GIT_TAG main
            )
            FetchContent_GetProperties(pico_pio_usb_fetch)
            if (NOT pico_pio_usb_fetch_POPULATED)
                FetchContent_Populate(pico_pio_usb_fetch)
            endif()
            set(PICO_PIO_USB_PATH ${pico_pio_usb_fetch_SOURCE_DIR})
        endif()
    endif()
    message("Pico-PIO-USB path: ${PICO_PIO_USB_PATH}")
endif()

project(SwitchKMAdapter C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Global include directories for project headers (include/) and config headers (src/btstack_config.h, src/sdkconfig.h)
# Directory-scoped include_directories ensures submodules like libbluepad32 also find btstack_config.h
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Common Pro Controller protocol & synchronization sources
set(COMMON_SOURCES
    src/main.c
    src/adapter_led.c
    src/usb_descriptors.c
    src/procon.c
    src/report.c
    src/usb.c
)

if(ADAPTER_INPUT_BACKEND STREQUAL "USB_HOST")
    add_executable(SwitchKMAdapter
        ${COMMON_SOURCES}
        src/usb_host_backend.c
    )
    target_compile_definitions(SwitchKMAdapter PRIVATE
        ADAPTER_INPUT_BACKEND=2 # BACKEND_USB_HOST
    )
    target_include_directories(SwitchKMAdapter PRIVATE
        src
        include
    )
    target_link_libraries(SwitchKMAdapter
        pico_stdlib
        pico_multicore
        pico_unique_id
        hardware_pio
        tinyusb_device
        tinyusb_host
        tinyusb_pico_pio_usb
    )
else()
    # WIRELESS_BT (Bluepad32 on Pico W)
    set(BLUEPAD32_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/bluepad32)
    set(BTSTACK_ROOT ${BLUEPAD32_ROOT}/external/btstack)

    add_subdirectory(bluepad32/src/components/bluepad32 libbluepad32)
    target_include_directories(bluepad32 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    add_executable(SwitchKMAdapter
        ${COMMON_SOURCES}
        src/pico_switch_platform.c
    )
    target_compile_definitions(SwitchKMAdapter PRIVATE
        ADAPTER_INPUT_BACKEND=1 # BACKEND_WIRELESS_BT
    )
    target_include_directories(SwitchKMAdapter PRIVATE
        src
        include
        bluepad32/src/components/bluepad32/include
    )
    target_link_libraries(SwitchKMAdapter
        pico_stdlib
        pico_cyw43_arch_none
        pico_btstack_classic
        pico_btstack_cyw43
        bluepad32
        tinyusb_device
        tinyusb_board
        pico_multicore
        pico_unique_id
    )
endif()

pico_enable_stdio_usb(SwitchKMAdapter 0)
pico_enable_stdio_uart(SwitchKMAdapter 1)
pico_add_extra_outputs(SwitchKMAdapter)
