cmake_minimum_required(VERSION 3.30)
project(KryptonSpeech LANGUAGES C CXX ASM_MASM)

if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Use Chromium's Windows x64 clang-cl toolchain")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
set(MOONSHINE_SOURCE "" CACHE PATH "Pinned streaming source")
set(ORT_SOURCE "" CACHE PATH "Pinned ONNX Runtime source")
set(KRYPTON_ORT_PREBUILT_ROOT "" CACHE PATH "Hash-verified official CPU-only ONNX Runtime package")
set(KRYPTON_SPEECH_ABI "" CACHE PATH "Chromium SODA ABI include directory")
set(KRYPTON_MODEL_INCLUDE "" CACHE PATH "Generated hash-pinned model metadata")
if(NOT EXISTS "${KRYPTON_MODEL_INCLUDE}/voice_model_manifest.h")
  message(FATAL_ERROR "Hash-pin the explicitly approved models before configuring")
endif()

add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/guard:cf>"
  "$<$<COMPILE_LANGUAGE:C,CXX>:/Gw>" "$<$<COMPILE_LANGUAGE:C,CXX>:/Gy>")
if(KRYPTON_ORT_PREBUILT_ROOT)
  if(NOT EXISTS "${KRYPTON_ORT_PREBUILT_ROOT}/lib/onnxruntime.dll" OR
     NOT EXISTS "${KRYPTON_ORT_PREBUILT_ROOT}/lib/onnxruntime.lib")
    message(FATAL_ERROR "Verify and extract the official CPU-only package first")
  endif()
  add_library(krypton_onnxruntime SHARED IMPORTED)
  set_target_properties(krypton_onnxruntime PROPERTIES
    IMPORTED_IMPLIB "${KRYPTON_ORT_PREBUILT_ROOT}/lib/onnxruntime.lib"
    IMPORTED_LOCATION "${KRYPTON_ORT_PREBUILT_ROOT}/lib/onnxruntime.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${KRYPTON_ORT_PREBUILT_ROOT}/include")
else()
# Source-only reduced build retained as an optional development alternative.
set(onnxruntime_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(onnxruntime_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(onnxruntime_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(onnxruntime_ENABLE_TRAINING OFF CACHE BOOL "" FORCE)
set(onnxruntime_USE_CUDA OFF CACHE BOOL "" FORCE)
set(onnxruntime_USE_DML OFF CACHE BOOL "" FORCE)
set(onnxruntime_USE_WEBGPU OFF CACHE BOOL "" FORCE)
set(onnxruntime_USE_OPENVINO OFF CACHE BOOL "" FORCE)
set(onnxruntime_USE_MIMALLOC OFF CACHE BOOL "" FORCE)
set(onnxruntime_USE_FULL_PROTOBUF OFF CACHE BOOL "" FORCE)
set(onnxruntime_DISABLE_ML_OPS ON CACHE BOOL "" FORCE)
set(onnxruntime_ENABLE_TELEMETRY OFF CACHE BOOL "" FORCE)
set(onnxruntime_ENABLE_LTO OFF CACHE BOOL "" FORCE)
set(onnxruntime_MSVC_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(ABSL_MSVC_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(protobuf_MSVC_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(onnxruntime_MINIMAL_BUILD ON CACHE BOOL "" FORCE)
set(onnxruntime_EXTENDED_MINIMAL_BUILD ON CACHE BOOL "" FORCE)
set(onnxruntime_REDUCED_OPS_BUILD ON CACHE BOOL "" FORCE)
set(onnxruntime_REDUCED_OPS_CONFIG_FILE "${KRYPTON_MODEL_INCLUDE}/required_operators.config" CACHE FILEPATH "" FORCE)
add_subdirectory("${ORT_SOURCE}/cmake" ort EXCLUDE_FROM_ALL)
# The pinned release omits clang-cl ISA options on these dispatch kernels.
# Keep them local to their CPU-dispatched files, never on baseline code.
set_source_files_properties(
  "${ORT_SOURCE}/onnxruntime/core/mlas/lib/rotary_embedding_kernel_avx2.cpp"
  "${ORT_SOURCE}/onnxruntime/core/mlas/lib/qkv_quant_kernel_avx2.cpp"
  TARGET_DIRECTORY onnxruntime_mlas
  PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/clang:-mfma;/clang:-mf16c")
set_source_files_properties("${ORT_SOURCE}/onnxruntime/core/mlas/lib/qgemm_kernel_sse41.cpp"
  TARGET_DIRECTORY onnxruntime_mlas PROPERTIES COMPILE_OPTIONS "/clang:-msse4.1")
set_source_files_properties("${ORT_SOURCE}/onnxruntime/core/mlas/lib/qgemm_kernel_amx.cpp"
  TARGET_DIRECTORY onnxruntime_mlas PROPERTIES COMPILE_OPTIONS "/clang:-mamx-tile;/clang:-mamx-int8;/clang:-mavx512f;/clang:-mavx512bw;/clang:-mavx512vnni")
get_target_property(mlas_sources onnxruntime_mlas SOURCES)
foreach(mlas_source IN LISTS mlas_sources)
  if(mlas_source MATCHES "\\.asm$")
    # These are MASM sources; clang-cl's generic assembler cannot parse them.
    set_source_files_properties("${mlas_source}" TARGET_DIRECTORY onnxruntime_mlas PROPERTIES LANGUAGE ASM_MASM)
  elseif(mlas_source MATCHES "avx512.*\\.cpp$")
    set(isa_options "/arch:AVX512;/clang:-mfma;/clang:-mf16c")
    if(mlas_source MATCHES "vnni")
      list(APPEND isa_options /clang:-mavx512vnni)
    endif()
    if(mlas_source MATCHES "fp16")
      list(APPEND isa_options /clang:-mavx512fp16)
    endif()
    if(mlas_source MATCHES "bf16")
      list(APPEND isa_options /clang:-mavx512bf16)
    endif()
    set_source_files_properties("${mlas_source}" TARGET_DIRECTORY onnxruntime_mlas PROPERTIES COMPILE_OPTIONS "${isa_options}")
  elseif(mlas_source MATCHES "avx2.*\\.cpp$")
    set_source_files_properties("${mlas_source}" TARGET_DIRECTORY onnxruntime_mlas PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/clang:-mfma;/clang:-mf16c")
  endif()
endforeach()
foreach(protobuf_target libprotobuf libprotobuf-lite libprotoc)
  if(TARGET ${protobuf_target})
    # The pinned serializer deliberately uses offsetof on generated classes.
    target_compile_options(${protobuf_target} PRIVATE /clang:-Wno-invalid-offsetof /clang:-Wno-unused-lambda-capture)
  endif()
endforeach()
endif()

set(core "${MOONSHINE_SOURCE}/core")
add_library(krypton_moonshine STATIC
  "${core}/moonshine-streaming-model.cpp"
  "${core}/context-biaser.cpp"
  "${core}/word-alignment.cpp"
  "${core}/bin-tokenizer/bin-tokenizer.cpp"
  "${core}/moonshine-utils/string-utils.cpp"
  "${core}/ort-utils/moonshine-ort-allocator.cpp"
  "${core}/ort-utils/ort-utils.cpp"
  src/ort_policy.cpp)
target_include_directories(krypton_moonshine SYSTEM PUBLIC "${core}" "${core}/bin-tokenizer"
  "${core}/moonshine-utils" "${core}/ort-utils")
target_compile_definitions(krypton_moonshine PRIVATE NOMINMAX)
target_compile_options(krypton_moonshine PRIVATE /W4)
if(KRYPTON_ORT_PREBUILT_ROOT)
target_link_libraries(krypton_moonshine PUBLIC krypton_onnxruntime)
else()
target_include_directories(krypton_moonshine SYSTEM PUBLIC "${ORT_SOURCE}/include/onnxruntime/core/session")
target_link_libraries(krypton_moonshine PUBLIC onnxruntime_session onnxruntime_optimizer
  onnxruntime_providers onnxruntime_framework onnxruntime_graph onnxruntime_util
  onnxruntime_mlas onnxruntime_common onnxruntime_flatbuffers)
endif()

add_library(krypton_speech SHARED src/soda_abi.cc src/speech_exports.def)
target_include_directories(krypton_speech PRIVATE "${KRYPTON_SPEECH_ABI}" "${KRYPTON_MODEL_INCLUDE}")
target_compile_definitions(krypton_speech PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(krypton_speech PRIVATE /W4 /WX)
target_link_libraries(krypton_speech PRIVATE krypton_moonshine bcrypt)
target_link_options(krypton_speech PRIVATE /guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /OPT:REF /OPT:ICF /DEBUG:FULL)

include("${CMAKE_CURRENT_LIST_DIR}/../tools/krypton/compiler_inputs.cmake")
krypton_track_compiler_inputs("${CMAKE_CURRENT_SOURCE_DIR}")
