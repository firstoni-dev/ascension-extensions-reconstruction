cmake_minimum_required(VERSION 3.25)
project(AscensionExtensions LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Extensions.dll must be built for x86 (32-bit)")
endif()

# Our own Extensions.dll for Ascension.exe (stock Wow.exe 12340 + a LoadLibraryA("Extensions.dll")
# cave), built on the MIT WotLK-Extensions DLL sources vendored under WotLKExtensions/.
# No Patcher, no injector: the exe loads a DLL of this name by itself.

set(CMAKE_CXX_STANDARD 17)
# static CRT everywhere (FreeType included): the DLL must not depend on a VC redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- upstream option set (same names as WotLK-Extensions' PatchConfig.hpp.in) --------------------
option(CUSTOM_DBC "Custom DBC loader" OFF)
option(LFGROLES_DBC "" OFF)
option(ZONELIGHT_DBC "" OFF)
option(OCCLUSIONVOLUME_DBC "" OFF)
option(SPELLATTRIBUTESEXTENDED_DBC "" OFF)
option(NOAMMO_PATCH "" OFF)
option(CHARCREATIONRACE_FIX "" ON)
option(COMBOPOINT_FIX "" ON)
option(CLASSLFDROLES_PATCH "" ON)
option(OOBLUAFUNCTIONS_PATCH "Register Lua functions from outside wow.exe" ON)
option(LUAUNLOCK_PATCH "" OFF)
option(CUSTOMPACKETS_PATCH "Custom opcode dispatch" ON)
option(ITEMMODEXT_PATCH "" OFF)
option(SPELLDESCRIPTIONEXT_PATCH "" OFF)
option(TOOLTIPID_PATCH "" OFF)
option(TOOLTIPDKRUNEEXT_PATCH "" OFF)
option(ITEMTOOLTIPMAPLINE_FIX "" OFF)
option(FRAMEEVENTEXTENSION "Register new FrameXML events" ON)
option(ACTIONBAR_LUA "" OFF)
option(DEVHELPER_LUA "" OFF)
option(GLUEMGREXTENSION "Glue-stage Lua function registration" ON)

# --- Ascension native logon (our reconstruction; not upstream) ------------------------------------
# Installs the original DLL's three logon hooks (client hello builder 0x8CCE00, key-material copy
# 0x9A83E0, 20-byte proof check 0x9A88C0) plus the four exe length-gate patches, replacing the
# client's native SRP6 handshake with Ascension's X25519/ChaCha20/HMAC-SHA256 scheme. OFF by
# default: only a server that speaks the same protocol (the collaborator's fork) can answer it.
# See src/Ascension/AscNativeLogon.cpp for the protocol, provenance and self-test.
option(ASCENSION_NATIVE_LOGON "Install the reconstructed Ascension native logon hooks (server must speak the protocol)" OFF)
set(ASCENSION_NATIVE_LOGON_SERVER_KEY "1a03d4c71f289f39046fad519f8af3e92b793709a8a340e492b4ff5ff55e1537" CACHE STRING "Server X25519 public key (hex, 64 chars) used by ASCENSION_NATIVE_LOGON")

# --- Interface hot patches (our addition; not in the original) ---------------------------------------
# SMSG 0x8F0 HOT_PATCH with its guards and per-realm consent, the 0x8F1/0x8F2 capability and result
# packets, and the SMSG 0x5BC formatter check that refuses Lua smuggled through ChallengeRequirementTypes.
# OFF by default: the exact reconstruction has none of it. See src/Ascension/AscHotPatch.cpp and
# AscensionRebirth/HOT_PATCH_DESIGN.md.
option(ASCENSION_HOT_PATCH "Interface hot-patch channel and the 0x5BC formatter check (not in the original)" OFF)

# --- Streamed client DBC rows (our addition; not in the original) -------------------------------------
# SMSG 0x8F3 / 0x8F4 / 0x8F5: batched server rows for GameObjectDisplayInfo, Map and MapDifficulty, which the
# original has no patch handler for. OFF by default. See src/Ascension/AscDbcStream.cpp.
option(ASCENSION_DBC_STREAM "Server-streamed GameObjectDisplayInfo / Map / MapDifficulty rows (not in the original)" OFF)

# --- Quest log of 64 quests (our addition; not in the original) ---------------------------------------
# SMSG 0x8F7 carries quest log slots 25..63; the DLL announces itself with CMSG 0x8F6 after every world entry.
# ON by default; OFF gives the genuine 25-slot behaviour. See src/Ascension/AscQuestLog64.cpp.
option(ASCENSION_QUEST_LOG_64 "Quest log of 64 quests via SMSG 0x8F7 (not in the original)" ON)

set(UP ${CMAKE_CURRENT_SOURCE_DIR}/WotLKExtensions)
configure_file(${UP}/cmake/PatchConfig.hpp.in ${CMAKE_BINARY_DIR}/gen/PatchConfig.hpp)

file(GLOB_RECURSE UP_SOURCES CONFIGURE_DEPENDS ${UP}/src/*.cpp)
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Main\\.cpp$")   # we own DllMain
# Upstream feature patch sets the original client never applies (live exactness audit, 2026-09-27).
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Client/(CGTooltip|WoWTime|CMissile|CMap|Misc|MacroConditions|Spell)\\.cpp$")
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/GameObjects/CGPlayer\\.cpp$")
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Misc/LuaUnlock\\.cpp$")
# Scaffold call-site hooks replaced by the original's own detours (audit step 3, 2026-09-27).
list(FILTER UP_SOURCES EXCLUDE REGEX ".*/src/Client/(CGlueMgr|CustomLua|CustomFileIO)\\.cpp$")
file(GLOB_RECURSE ASC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(Extensions SHARED ${UP_SOURCES} ${ASC_SOURCES})
set_target_properties(Extensions PROPERTIES OUTPUT_NAME "Extensions" PREFIX "")
target_include_directories(Extensions PRIVATE ${CMAKE_BINARY_DIR}/gen ${UP}/src ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(Extensions PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    # static CRT: the DLL must not depend on a VC redistributable on the player's box
    set_property(TARGET Extensions PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(Extensions PRIVATE /W3 /EHsc /Zi)
    target_link_options(Extensions PRIVATE /DEBUG /SAFESEH:NO)
endif()

# --- FreeType 2.14.1, as the original links it (FontRender.cpp; see AscFontRender.cpp) -------------
# Stock ftmodule.h / ftoption.h: every default module (incl. sdf, bsdf, ot-svg), internal zlib 1.3.1,
# LZW; no bzip2 / png / harfbuzz / brotli -- measured from the original's strings and FT_New_Library.
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB FALSE CACHE BOOL "" FORCE)
set(FT_REQUIRE_ZLIB FALSE CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)   # always the bundled zlib, never a system one
set(SKIP_INSTALL_ALL TRUE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype ${CMAKE_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
target_link_libraries(Extensions PRIVATE freetype)
