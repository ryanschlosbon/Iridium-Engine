# Compiler settings shared by every target, including FetchContent dependencies
# added after this file is included.

if(MSVC)
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    )

    # Self-heal stale caches. A build directory first configured without the MSVC
    # developer environment (for example by an IDE, before cl.exe was resolved)
    # can cache *empty* per-configuration flag strings. Cache values win over the
    # platform defaults on every later configure, so such a Debug tree silently
    # compiles without /Zi /Ob0 /Od /RTC1 and links without /debug. A clean
    # configure is unaffected: there the cached values already equal the defaults.
    foreach(_iridium_flags_var IN ITEMS
            CMAKE_CXX_FLAGS
            CMAKE_CXX_FLAGS_DEBUG
            CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_MINSIZEREL
            CMAKE_EXE_LINKER_FLAGS_DEBUG
            CMAKE_EXE_LINKER_FLAGS_RELEASE
            CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_EXE_LINKER_FLAGS_MINSIZEREL)
        string(STRIP "${${_iridium_flags_var}}" _iridium_current)
        string(STRIP "${${_iridium_flags_var}_INIT}" _iridium_default)
        if(_iridium_current STREQUAL "" AND NOT _iridium_default STREQUAL "")
            message(WARNING
                "${_iridium_flags_var} is empty in this build directory's cache "
                "(stale cache); restoring the MSVC default '${_iridium_default}'.")
            get_property(_iridium_doc CACHE ${_iridium_flags_var} PROPERTY HELPSTRING)
            set(${_iridium_flags_var} "${_iridium_default}" CACHE STRING
                "${_iridium_doc}" FORCE)
        endif()
    endforeach()
    unset(_iridium_flags_var)
    unset(_iridium_current)
    unset(_iridium_default)
    unset(_iridium_doc)
endif()
