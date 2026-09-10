# assets

**`JetBrainsMono-ExtraBold.ttf`** lives here, with its licence (`JetBrainsMono-OFL.txt`) beside it.
It is redistributable under the SIL Open Font License, which is why it can be committed.

The build copies whatever is here next to the `.exe`, and the app loads the font from disk at
startup. It is deliberately *not* embedded as an `.rc` resource: an `.rc` reference to a missing
file breaks the build, and a missing font should only cost you the typeface.

Without it the clock falls back to **Cascadia Mono** and still runs — the digits just won't match
`html/index.html`.

## Why ExtraBold (800) and not Black (900)

The page asks for `font-weight: 900` but only loads weights 700 and 800 from Google Fonts. Since
900 isn't in the loaded set, the browser clamps to 800 — so 800 is the weight that actually
renders in the HTML, and the one to match here.

Upstream is <https://github.com/JetBrains/JetBrainsMono/releases>; the committed copy is v2.304.
