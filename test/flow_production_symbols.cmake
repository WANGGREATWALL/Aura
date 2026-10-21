# Do not require a demangler: these fragments also match Itanium ABI symbols
# emitted by Android/Linux/macOS toolchains. MSVC is covered by build flags.
if(NOT DEFINED AURA_NM OR NOT DEFINED AURA_LIBRARY)
    message(FATAL_ERROR "Expected AURA_NM and AURA_LIBRARY")
endif()
execute_process(COMMAND "${AURA_NM}" "${AURA_LIBRARY}"
    RESULT_VARIABLE result OUTPUT_VARIABLE symbols ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot inspect production archive: ${errors}")
endif()
foreach(hook IN ITEMS
    "au4flow7testing" "au4flow6detail4fail" "au4flow6detail8gFailure"
    "au4flow6detail17gOutstandingLimit" "au4flow6detail13gSnapshotHook" "au4flow6detail12gRuntimeHook" "au4flow6detail12runtimePoint"
    "au3gpu6detail12setNativeApi" "au3gpu6detail15backendIdentity"
    "au3sys6memory13setAccessHook")
    string(FIND "${symbols}" "${hook}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "Production archive contains test hook: ${hook}")
    endif()
endforeach()
