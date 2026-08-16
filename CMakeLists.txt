cmake_minimum_required(VERSION 3.16)
project(uvc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Avoid aliasing UB in the entropy coder (we cast buffers to words via memcpy anyway,
# but keep strict-aliasing off for safety on the scaffold).
add_compile_options(-O2 -Wall -Wextra -fno-strict-aliasing)

# ---- Common codec primitives (spec §6, §12, §3) ----
file(GLOB UVC_COMMON common/*.c)
add_library(uvc_common STATIC ${UVC_COMMON})
target_include_directories(uvc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)

# ---- Encoder pieces (spec §9 analyzer, §10 selector) ----
add_library(uvc_enc STATIC encoder/analyzer.c encoder/selector.c)
target_link_libraries(uvc_enc uvc_common)

# ---- Decoder pieces (spec §1 tier negotiation) ----
add_library(uvc_dec STATIC decoder/negotiate.c)
target_link_libraries(uvc_dec uvc_common)

# ---- Self-test / conformance binary ----
add_executable(uvctest tools/uvctest.c)
# Link statically so the binary is self-contained (no MinGW runtime DLL dependency).
target_link_options(uvctest PRIVATE -static)
target_link_libraries(uvctest uvc_common uvc_enc uvc_dec)
target_include_directories(uvctest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/encoder
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder)

enable_testing()
add_test(NAME uvctest COMMAND uvctest)
