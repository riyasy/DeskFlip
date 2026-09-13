// Which words the UI uses.
//
// Storage is one STRINGTABLE per language in Localization\strings.rc, generated from
// Localization\translations.csv by build.ps1 and compiled into the exe, so the exe ships as a
// single file. A blank translation is filled with the English at generation time, so every
// language block carries every id and a half-translated language is a working one.
//
// LoadStringW is not used because it takes no language: it answers in the thread's UI language,
// and SetThreadUILanguage to steer it would also change the language of system UI on this thread,
// with a fallback order that is not ours. Instead the language is chosen once here and T() reads
// that block directly.

#include "loc.h"

#include <cstdlib>  // _countof
#include <cwchar>

// RT_STRING resources are bundles of 16: ids n*16 .. n*16+15 live in bundle n+1.
static LPCWSTR Bundle(UINT id) { return MAKEINTRESOURCEW((id >> 4) + 1); }

static LANGID s_lang = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);

// The languages strings.rc was built with, read back from the exe rather than listed a second time
// here. Enumeration is in LANGID order, which the same-language tier below relies on.
struct Shipped { LANGID id[32]; UINT n; };

static BOOL CALLBACK CollectLang(HMODULE, LPCWSTR, LPCWSTR, WORD lang, LONG_PTR param) {
    auto* s = reinterpret_cast<Shipped*>(param);
    if (s->n < _countof(s->id)) s->id[s->n++] = lang;
    return TRUE;
}

void LocInit() {
    // The user's *display language* chain, most preferred first -- NOT LOCALE_NAME_USER_DEFAULT,
    // which is the Region setting and answers a different question. English Windows with a German
    // region is a normal setup, and it wants English menus.
    //
    // The chain matters as much as the name: Windows answers e.g. "de-AT" -> "de" -> "de-DE", and
    // following it is how a language pack we have no exact block for still resolves to one we do.
    // A chain that overflows 512 WCHARs leaves us in English.
    wchar_t langs[512] = { 0 };
    ULONG count = 0, cch = _countof(langs);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, langs, &cch)) return;

    Shipped shipped{};
    EnumResourceLanguagesW(nullptr, RT_STRING, Bundle(IDS_LOC_FIRST), CollectLang,
        reinterpret_cast<LONG_PTR>(&shipped));

    // Both tiers for one language before moving to the next, which is the whole point of a
    // preference order: a de-AT primary must reach de-DE before an en-US secondary is considered.
    for (const wchar_t* p = langs; *p; p += wcslen(p) + 1) {
        // Plenty of real locales have no LCID of their own -- pt-AO maps to a transient one with
        // primary language 0 -- so those retry with just the language part, which does have one
        // ("pt" -> 0x16) and is all the same-language tier needs. 0 after that is a name Windows
        // cannot place at all: next.
        LANGID want = LANGIDFROMLCID(LocaleNameToLCID(p, 0));
        if (PRIMARYLANGID(want) == LANG_NEUTRAL) {
            wchar_t lang[LOCALE_NAME_MAX_LENGTH];
            if (wcscpy_s(lang, p) != 0) continue;
            if (wchar_t* dash = wcschr(lang, L'-')) *dash = 0;
            want = LANGIDFROMLCID(LocaleNameToLCID(lang, LOCALE_ALLOW_NEUTRAL_NAMES));
            if (PRIMARYLANGID(want) == LANG_NEUTRAL) continue;
        }

        // Exact first, and that is load-bearing rather than tidy: pt-PT/pt-BR and zh-CN/zh-TW are
        // different blocks, and going straight to the primary language would hand half of those
        // users the other half's translation.
        for (UINT i = 0; i < shipped.n; i++)
            if (shipped.id[i] == want) { s_lang = want; return; }

        // Then any block of the same language, which serves the regional variants nobody ships a
        // block for -- es-MX takes es-ES, de-AT takes de-DE. It resolves in LANGID order, so the
        // language is right and the flavour can be wrong: zh-HK takes zh-TW (0x0404, before
        // zh-CN's 0x0804), which is right for Hong Kong, and zh-SG takes it too, which is not.
        // pt-AO takes pt-BR. The fix is a column in translations.csv, never code.
        for (UINT i = 0; i < shipped.n; i++)
            if (PRIMARYLANGID(shipped.id[i]) == PRIMARYLANGID(want)) { s_lang = shipped.id[i]; return; }
    }
}

const wchar_t* T(UINT id) {
    // s_lang is always a language EnumResourceLanguagesW reported (or en-US, which is always
    // there), so this finds that exact block. A bundle holds 16 entries back to back, each a WORD
    // length and then that many WCHARs; /n appends a null to every string and counts it in the
    // length, so the pointer can be handed out as is. An unused slot has length 0. Walked when the
    // menu or the About box is built, never per frame.
    const HRSRC res = FindResourceExW(nullptr, RT_STRING, Bundle(id), s_lang);
    const HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const wchar_t* p = mem ? static_cast<const wchar_t*>(LockResource(mem)) : nullptr;
    if (!p) return L"";
    for (UINT i = id & 0xF; i; i--) p += 1 + *p;
    return *p ? p + 1 : L"";
}
