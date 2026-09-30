cmake_minimum_required(VERSION 3.20)
project(openblizz VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OPENBLIZZ_BUILD_TESTS "Build OpenBlizz unit tests" ON)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LZ4 REQUIRED IMPORTED_TARGET liblz4)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

add_library(openblizz_core
    src/http.cpp
    src/hash.cpp
    src/formats.cpp
    src/catalog.cpp
    src/installer.cpp
    src/runner.cpp
    src/auth.cpp
)

target_include_directories(openblizz_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${NLOHMANN_JSON_INCLUDE_DIR}
)

target_link_libraries(openblizz_core
    PUBLIC
        CURL::libcurl
        OpenSSL::Crypto
        ZLIB::ZLIB
        PkgConfig::LZ4
)

target_compile_features(openblizz_core PUBLIC cxx_std_20)
target_compile_options(openblizz_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(openblizz src/main.cpp)
target_link_libraries(openblizz PRIVATE openblizz_core)

if(OPENBLIZZ_BUILD_TESTS)
    enable_testing()
    add_executable(openblizz_tests tests/core_tests.cpp)
    target_link_libraries(openblizz_tests PRIVATE openblizz_core)
    add_test(NAME openblizz_tests COMMAND openblizz_tests)
endif()

install(TARGETS openblizz RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
