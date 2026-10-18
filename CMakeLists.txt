cmake_minimum_required(VERSION 3.24)
project(software_anc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "software_anc targets Windows (WASAPI).")
endif()

# ---------------------------------------------------------------------------
# Core static library shared by all executables
# ---------------------------------------------------------------------------
add_library(anc_core STATIC
  src/audio/wasapi_utils.cpp
  src/audio/device_probe.cpp
  src/audio/wasapi_streams.cpp
  src/audio/wasapi_streams_run.cpp
  src/audio/wav_writer.cpp
  src/dsp/pass_through.cpp
  src/dsp/fir_filter.cpp
  src/dsp/tone_generator.cpp
  src/dsp/chirp_emitter.cpp
  src/dsp/narrowband_experiment.cpp
)

target_include_directories(anc_core PUBLIC src)
target_compile_definitions(anc_core PUBLIC
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(anc_core PUBLIC ole32 uuid avrt)

if(MSVC)
  target_compile_options(anc_core PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
function(anc_add_exe name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE anc_core)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
  endif()
endfunction()

anc_add_exe(audio_probe        src/tools/audio_probe_main.cpp)
anc_add_exe(audio_passthrough  src/tools/audio_passthrough_main.cpp)
anc_add_exe(dsp_sandbox        src/tools/dsp_sandbox_main.cpp)
anc_add_exe(narrowband_experiment src/tools/narrowband_main.cpp)

# ---------------------------------------------------------------------------
# Unit tests (plain C++ runner, registered with CTest)
# ---------------------------------------------------------------------------
anc_add_exe(anc_tests
  tests/test_main.cpp
  tests/test_phase_math.cpp
  tests/test_filter.cpp
  tests/test_ring_buffer.cpp
)

enable_testing()
add_test(NAME unit COMMAND anc_tests)
