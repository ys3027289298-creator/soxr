# SoX Resampler Library       Copyright (c) 2007-13 robs@users.sourceforge.net
# Licence for this file: LGPL v2.1                  See LICENCE for details.

set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${PROJECT_C_FLAGS}")
link_libraries (${PROJECT_NAME} ${LIBM_LIBRARIES})

file (GLOB SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
foreach (fe ${SOURCES})
  get_filename_component (f ${fe} NAME_WE)
  add_executable (${f} ${fe})
endforeach ()

# Can't use c89 for this file:
if (CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set_property (SOURCE throughput APPEND_STRING PROPERTY COMPILE_FLAGS "-std=gnu89")
endif ()

set (sweep_to_freq 22050)
set (leader 1)
set (len 16)
math (EXPR base_rate "${sweep_to_freq} + ${sweep_to_freq}")

macro (add_vector r)
  set (output ${CMAKE_CURRENT_BINARY_DIR}/ref-${r}.s32)
  add_custom_command (OUTPUT ${output} DEPENDS vector-gen ${CMAKE_CURRENT_LIST_FILE}
    COMMAND vector-gen ${r} ${leader} ${len} 0 ${sweep_to_freq} 1 ${output})
  set (vectors ${output} ${vectors})
endmacro ()

macro (add_cmp_test irate orate bits)
  set (name ${bits}-bit-perfect-${irate}-${orate})
  add_test (NAME ${name} COMMAND ${CMAKE_COMMAND} -Dbits=${bits} -DBIN=${BIN}
    -DEXAMPLES_BIN=${EXAMPLES_BIN} -DlenToSkip=${leader} -Dorate=${orate}
    -Dirate=${irate} -Dlen=${len} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmp-test.cmake)
  add_vector (${irate})
  add_vector (${orate})
endmacro ()

unset (test_bits)
if (WITH_CR32 OR WITH_CR32S OR WITH_CR64 OR WITH_CR64S)
  set (test_bits 20)
endif ()
if (WITH_CR64 OR WITH_CR64S)
  set (test_bits ${test_bits} 28)
endif ()

set (rates 192000)
if (WITH_HI_PREC_CLOCK)
  set (rates ${rates} 65537)
endif ()
foreach (b ${test_bits})
  foreach (r ${rates})
    add_cmp_test (${base_rate} ${r} ${b})
    add_cmp_test (${r} ${base_rate} ${b})
  endforeach ()
endforeach ()

if (NOT CMAKE_CROSSCOMPILING)
  add_custom_target (test-vectors ALL DEPENDS ${vectors})
endif ()

add_test (1-delay-clear ${BIN}1-delay-clear)

# Anti-aliasing filter designer: measured stop-band attenuation must be
# within 1 dB of the design value, and one design must take < 0.5 s:
if (TARGET filter-designer)
  add_test (NAME filter-designer-1 COMMAND filter-designer --fp 0.40 --fs 0.50 --att  60 --check)
  add_test (NAME filter-designer-2 COMMAND filter-designer --fp 0.45 --fs 0.50 --att 100 --check)
  add_test (NAME filter-designer-3 COMMAND filter-designer --fp 0.30 --fs 0.42 --att  80 --check)
  add_test (NAME filter-designer-4 COMMAND filter-designer --fp 0.42 --fs 0.46 --att 120 --check)
  add_test (NAME filter-designer-export COMMAND filter-designer
    --rate 48000 --fp 20000 --fs 22000 --att 90 --tradeoffs --check
    --out fd-test.txt --verify fd-test.txt)
endif ()
