#pragma once

#include <windows.h>

#include "Localization/strings.h"

// The UI language. Ported from let-it-rain's loc.cpp, minus its right-to-left half: no RTL
// language ships here.

// Reads the user's display language and picks the matching STRINGTABLE built into the exe from
// Localization\strings.rc. Call once, before any window or menu is built. Silent on failure: a
// display language with no block, or none at all, leaves the UI in English.
void LocInit();

// The string for an IDS_ id from Localization\strings.h, in the chosen language. Never null. The
// pointer is into the exe's own resources, null-terminated (strings.rc is compiled with /n) and
// valid for the life of the process.
const wchar_t* T(UINT id);
