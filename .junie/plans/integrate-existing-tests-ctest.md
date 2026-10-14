---
sessionId: session-261014-111135-1qev
---

# Requirements

### Overview & Goals
The objective is to integrate the project's existing test suites into CMake's native test driver, CTest. By establishing standardized CTest integration, developers and automated CI pipelines can execute, monitor, and report all test results through standard CMake test commands (`ctest`), maximizing testing efficiency and minimizing developer toil.

### Scope
- **In Scope:**
  - Enabling CTest support in `CMakeLists.txt` via CMake standard testing mechanisms.
  - Registering all existing test executables (`SessionStateTests`, `LegacyAdapterTests`, `DataManagerTests`, `EndToEndParityTests`) as CTest tests.
  - Ensuring testing can be toggled via standard CMake options (`BUILD_TESTING`).
- **Out of Scope:**
  - Modifying the test logic, assertions, or test cases inside `Tests/*.cpp`.
  - Adding third-party testing frameworks (e.g., GoogleTest, Catch2).

### Functional Requirements
- Invoking `ctest` (or `ctest --output-on-failure`) in the build directory must discover and execute all 4 test suites.
- Each test target must execute its corresponding binary and return a non-zero exit code upon failure, correctly reflected in CTest summary results.
- Test building and registration must respect the standard `BUILD_TESTING` configuration option.

### Non-Functional Requirements
- **Efficiency & Portability:** Build and test configuration must rely strictly on built-in CMake / CTest facilities with zero extra external dependencies.
- **Maintainability:** Standard CMake conventions must be followed so future test suites can be registered straightforwardly.

# Technical Design

### Current Implementation
The project defines four standalone test executables in `CMakeLists.txt`:
- `SessionStateTests` (from `Tests/SessionStateTests.cpp`)
- `LegacyAdapterTests` (from `Tests/LegacyAdapterTests.cpp`)
- `DataManagerTests` (from `Tests/DataManagerTests.cpp`)
- `EndToEndParityTests` (from `Tests/EndToEndParityTests.cpp`)

Currently, `CMakeLists.txt` builds these executables as targets, but neither `enable_testing()` nor `include(CTest)` is called, and none of the test targets are registered via `add_test()`. As a result, CTest cannot discover or run any tests.

### Key Decisions
- **Use `include(CTest)` with `BUILD_TESTING` Guard:**
  - *Decision:* Utilize CMake's standard `include(CTest)` module and wrap test target definitions and `add_test` declarations within `if(BUILD_TESTING)`.
  - *Rationale:* `include(CTest)` automatically manages the `BUILD_TESTING` option (defaulting to `ON`) and calls `enable_testing()`. This maximizes practical utility by allowing developers and CI pipelines to disable test targets during production builds while enabling automated test discovery by default.

### Proposed Changes
1. **`CMakeLists.txt`:**
   - Add `include(CTest)` after project setup and library definitions.
   - Wrap the test executables and their target linkages in `if(BUILD_TESTING) ... endif()`.
   - Register each test using `add_test()`:
     ```cmake
     include(CTest)

     if(BUILD_TESTING)
         # Existing test executable definitions:
         # SessionStateTests, LegacyAdapterTests, DataManagerTests, EndToEndParityTests

         add_test(NAME SessionStateTests COMMAND SessionStateTests)
         add_test(NAME LegacyAdapterTests COMMAND LegacyAdapterTests)
         add_test(NAME DataManagerTests COMMAND DataManagerTests)
         add_test(NAME EndToEndParityTests COMMAND EndToEndParityTests)
     endif()
     ```

### Risks
- **Working Directory Dependencies:** If any test requires relative assets, running from arbitrary working directories could fail.
  - *Mitigation:* The existing test suites utilize in-memory fixtures and inline JSON strings; no file system dependencies exist in the current test suite.

# Testing

### Validation Approach
Verification focuses on ensuring reliable test discovery, accurate pass/fail reporting, and proper integration with CMake workflows.

### Key Scenarios
1. **CTest Test Discovery and Execution:**
   - Configure project with `cmake -B build -S .` and build with `cmake --build build`.
   - Run `ctest --test-dir build --output-on-failure`.
   - Verify that all 4 tests (`SessionStateTests`, `LegacyAdapterTests`, `DataManagerTests`, `EndToEndParityTests`) are discovered, executed, and reported as 100% passed.

2. **Conditional Build Validation (`BUILD_TESTING=OFF`):**
   - Configure project with `cmake -B build_notest -S . -DBUILD_TESTING=OFF` and build with `cmake --build build_notest`.
   - Verify that test executables are not compiled and no CTest tests are registered.

# Delivery Steps

### ✓ Step 1: Enable CTest and register test targets in CMakeLists.txt
CTest support is enabled in `CMakeLists.txt` and all existing test executables are registered as CTest targets.

- Include the standard `CTest` module (`include(CTest)`) or `enable_testing()` in `CMakeLists.txt`.
- Guard test targets under `if(BUILD_TESTING)` to support clean release builds and reduce unnecessary compilation overhead when testing is disabled.
- Register all 4 existing test targets using `add_test()`:
  - `add_test(NAME SessionStateTests COMMAND SessionStateTests)`
  - `add_test(NAME LegacyAdapterTests COMMAND LegacyAdapterTests)`
  - `add_test(NAME DataManagerTests COMMAND DataManagerTests)`
  - `add_test(NAME EndToEndParityTests COMMAND EndToEndParityTests)`

### ✓ Step 2: Verify test execution and build target behavior via CTest
All test targets compile, register with CTest, and pass execution with detailed reporting.

- Configure the build with tests enabled and compile the test targets.
- Execute `ctest --output-on-failure` to verify that all 4 test suites run and report success.
- Verify that `BUILD_TESTING=OFF` cleanly disables test compilation and CTest registration.