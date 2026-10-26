cmake_minimum_required(VERSION 3.22)
project(DjehutiMath VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32)
    message(FATAL_ERROR "This initial application foundation targets Windows.")
endif()
set(JUCE_PATH "D:/JUCE" CACHE PATH "Shared JUCE source")
set(FRUSTLANG_ROOT "D:/FrustLang" CACHE PATH "Shared FRust checkout")
set(FRUST_VCPKG_INSTALLED_DIR "D:/000 Creation Suite/apps/CreationEngine/vcpkg_installed/x64-windows" CACHE PATH "Existing LLVM installation")
list(PREPEND CMAKE_PREFIX_PATH "${FRUST_VCPKG_INSTALLED_DIR}")
find_package(LLVM CONFIG REQUIRED PATHS "${FRUST_VCPKG_INSTALLED_DIR}/share/llvm" NO_DEFAULT_PATH)
llvm_map_components_to_libnames(MATH_LLVM_LIBS Core ExecutionEngine OrcJIT Support native nativecodegen)
add_subdirectory("${JUCE_PATH}" juce_build EXCLUDE_FROM_ALL)

# Use the canonical, already built FRust libraries. Do not compile another
# language toolchain (or another copy of each JUCE module) for this shell.
foreach(dependency CreationDock ai_provider frate_lib frust_lang frust_runtime frust_plugin_host)
    add_library(${dependency} STATIC IMPORTED)
    set_target_properties(${dependency} PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release
        IMPORTED_LOCATION_DEBUG "${FRUSTLANG_ROOT}/lib/Debug/${dependency}.lib"
        IMPORTED_LOCATION_RELEASE "${FRUSTLANG_ROOT}/lib/Release/${dependency}.lib")
endforeach()
juce_add_gui_app(DjehutiMath PRODUCT_NAME "Djehuti Math" COMPANY_NAME "LagDaemon Software" BUNDLE_ID "com.lagdaemon.djehuti.math" VERSION 0.1.0
    ICON_BIG "${CMAKE_SOURCE_DIR}/Resources/branding/djehuti-math-256.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/Resources/branding/djehuti-math-32.png")
set_target_properties(DjehutiMath PROPERTIES OUTPUT_NAME djehuti_math
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin/Release")
target_sources(DjehutiMath PRIVATE
    Source/Main.cpp Source/DjehutiMath.cpp Source/WorksheetPanel.cpp
    Source/MathSession.cpp Source/MathKnowledge.cpp
    Source/DjehutiMenuBarComponent.cpp Source/DjehutiAgent.cpp Source/ProtectedCredentialStore.cpp
    Source/Auth/DesktopAuthSession.cpp Source/Auth/ProfileAvatarComponent.cpp
    Source/FrustEngine.cpp Source/FrustExecution.cpp Source/FrustPanel.cpp
    Source/NodeCompiler.cpp Source/NodeDesignerPanel.cpp Source/FrustDebuggerPanel.cpp
    Source/AudioPipeline.cpp Source/DspEffect.cpp Source/DspPanel.cpp Source/VstExporter.cpp
    Source/DjehutiMathTools.cpp Source/LocalAgentApi.cpp Source/DspCheck.cpp
    Source/MathDoc.cpp Source/MathLinear.cpp Source/MathFont.cpp Source/MathLayout.cpp Source/MathView.cpp Source/MathCheck.cpp)
target_include_directories(DjehutiMath PRIVATE Source
    "${FRUSTLANG_ROOT}/projects/03_creation_dock/include"
    "${FRUSTLANG_ROOT}/projects/04_ai_provider/include"
    "${FRUSTLANG_ROOT}/projects/09_frust_plugin_host/include"
    "${FRUSTLANG_ROOT}/projects/01_language_paradigms/02_functional")
target_compile_definitions(DjehutiMath PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1 JUCE_APPLICATION_NAME_STRING="Djehuti Math" JUCE_APPLICATION_VERSION_STRING="0.1.0"
    DJEHUTIMATH_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_options(DjehutiMath PRIVATE /FS /MP1)
# Source text containing mathematical/UI Unicode is UTF-8. Limit this source
# option to application files so it does not rebuild the shared JUCE modules.
set_source_files_properties(Source/DjehutiMath.cpp Source/WorksheetPanel.cpp Source/NodeDesignerPanel.cpp Source/NodeCompiler.cpp
    Source/FrustDebuggerPanel.cpp Source/DspPanel.cpp Source/DjehutiMathTools.cpp Source/MathCheck.cpp Source/MathView.cpp PROPERTIES COMPILE_OPTIONS /utf-8)
target_link_libraries(DjehutiMath PRIVATE CreationDock ai_provider frust_plugin_host frust_lang
    frust_runtime frate_lib ${MATH_LLVM_LIBS} crypt32
    juce::juce_core juce::juce_data_structures juce::juce_events juce::juce_graphics
    juce::juce_gui_basics juce::juce_gui_extra
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats juce::juce_audio_utils
    juce::juce_audio_processors)
juce_generate_juce_header(DjehutiMath)

# The VST3 runtime every exported effect is packaged on. The application build
# builds it and copies the bundle to templates/vst3 beside the executable.
juce_add_plugin(DjehutiMathEffectVst
    COMPANY_NAME "Djehuti Math"
    BUNDLE_ID "com.lagdaemon.djehuti.math.effect"
    PRODUCT_NAME "Djehuti Math Effect"
    PLUGIN_MANUFACTURER_CODE Djmt
    PLUGIN_CODE DmFx
    FORMATS VST3
    IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_AUTO_MANIFEST FALSE
    VERSION 1.0.0)
target_sources(DjehutiMathEffectVst PRIVATE
    Source/GeneratedVst/MathEffectVst.h Source/GeneratedVst/MathEffectVst.cpp
    Source/DspEffect.h Source/DspEffect.cpp Source/FrustEngine.h Source/FrustEngine.cpp
    Source/MathSession.h Source/MathSession.cpp)
target_include_directories(DjehutiMathEffectVst PRIVATE Source Source/GeneratedVst
    "${FRUSTLANG_ROOT}/projects/09_frust_plugin_host/include"
    "${FRUSTLANG_ROOT}/projects/01_language_paradigms/02_functional")
target_compile_definitions(DjehutiMathEffectVst PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_compile_options(DjehutiMathEffectVst PRIVATE /FS /MP1)
target_link_libraries(DjehutiMathEffectVst PRIVATE frust_plugin_host frust_lang frust_runtime frate_lib ${MATH_LLVM_LIBS}
    juce::juce_core juce::juce_data_structures juce::juce_events juce::juce_graphics juce::juce_gui_basics
    juce::juce_audio_basics juce::juce_audio_processors)
juce_generate_juce_header(DjehutiMathEffectVst)
add_dependencies(DjehutiMath DjehutiMathEffectVst_VST3)

add_custom_command(TARGET DjehutiMath POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:DjehutiMathEffectVst_VST3>/../.."
        "$<TARGET_FILE_DIR:DjehutiMath>/templates/vst3/Djehuti Math Effect.vst3"
    COMMENT "Packaging the VST3 runtime template")
add_custom_command(TARGET DjehutiMath POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Resources/fonts" "$<TARGET_FILE_DIR:DjehutiMath>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/Resources/branding" "$<TARGET_FILE_DIR:DjehutiMath>/branding"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/knowledge" "$<TARGET_FILE_DIR:DjehutiMath>/knowledge"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/docs" "$<TARGET_FILE_DIR:DjehutiMath>/docs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/Resources/default_layouts.json" "$<TARGET_FILE_DIR:DjehutiMath>/default_layouts.json")
