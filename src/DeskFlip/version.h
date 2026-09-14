// version.h -- the version, in one place.
//
// Included by DeskFlip.rc (the VERSIONINFO block that gives the exe its identity -- a native
// binary with no company, product or version is one of the strongest signals an antivirus
// heuristic has) and by main.cpp (the line the About box shows). They must agree.
//
// XML cannot include this, so two copies are kept by hand: Version= in Package.appxmanifest and
// the assemblyIdentity in DeskFlip.manifest. CI reads VER_DISPLAY from here for the zip's name.
//
// RC only understands #define, so this file must stay free of anything else.

#define VER_NUMBER      1,0,2,0             // VERSIONINFO wants commas
#define VER_STRING      "1.0.2.0"           // and a matching string
#define VER_DISPLAY     "v1.0.2"            // what a person reads, in the About box

#define VER_COMPANY     "RYF Tools"
#define VER_PRODUCT     "DeskFlip"
#define VER_DESCRIPTION "DeskFlip desktop clock widget"
#define VER_COPYRIGHT   "\xA9 RYF Tools. All rights reserved."
#define VER_FILENAME    "DeskFlip.exe"
