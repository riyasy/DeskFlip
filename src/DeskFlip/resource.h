#pragma once

// Icons. The LOWEST id is what Explorer, the taskbar and Alt+Tab show as the application's
// icon, so IDI_DESKFLIP must stay below the three "other apps" icons the About box loads --
// give one of them a smaller id and the exe starts wearing that app's logo.
#define IDI_DESKFLIP        102
#define IDI_FLYPHOTOS        103
#define IDI_LETITRAIN        104
#define IDI_DESKTICK         105

// The version, in one place: DeskFlip.rc's VERSIONINFO block (which is what gives the exe an
// identity -- a native binary with no company, product or version is one of the strongest
// signals an antivirus heuristic has) and the line the About box shows. They must agree.
// RC only understands #define, so nothing but #define may appear above.
//
// Keep in step with Package.appxmanifest's Identity Version by hand; XML cannot include a header.
#define VER_NUMBER      1,0,1,0             // VERSIONINFO wants commas
#define VER_STRING      "1.0.1.0"           // and a matching string
#define VER_DISPLAY     "v1.0.1"            // what a person reads, in the About box

#define VER_COMPANY     "RYF Tools"
#define VER_PRODUCT     "DeskFlip"
#define VER_DESCRIPTION "DeskFlip desktop clock widget"
#define VER_COPYRIGHT   "\xA9 RYF Tools. All rights reserved."
#define VER_FILENAME    "DeskFlip.exe"
