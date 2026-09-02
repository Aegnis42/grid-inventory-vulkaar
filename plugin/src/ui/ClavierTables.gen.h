// ═══════════════════════════════════════════════════════════════════════════
// GÉNÉRÉ — NE PAS ÉDITER À LA MAIN.
//
// Source : vulkaar rp / packages/ui/src/clavier.js, par
// scripts/generer-clavier-greffon.mjs. La page CEF et ce greffon lisent LA
// MÊME table : une lettre se corrige là-bas, puis on régénère ici.
//
// Position physique (code de balayage) → ce que la disposition DÉCLARÉE par
// le joueur dans le launcher y imprime. Voir Clavier.h pour la traduction.
// ═══════════════════════════════════════════════════════════════════════════
#pragma once
#include <cstddef>
#include <cstdint>

namespace vk::clavier::tables {

    struct Touche {
        std::uint8_t   sc;        // code de balayage DirectX (position)
        const wchar_t* col[4];    // base, Maj, AltGr, AltGr+Maj ; nullptr = absente
        std::uint8_t   morte;     // bit i : la colonne i est une touche morte (accent seul)
        bool           verrMaj;   // Verr. Maj inverse base et Maj
    };

    struct Composition { wchar_t accent; wchar_t lettre; wchar_t resultat; };

    struct Disposition { const char* nom; const Touche* touches; std::size_t n; };

    inline constexpr Touche kAzerty[] = {
        { 0x29, { L"\u00b2", L"", nullptr, nullptr }, 0, false },  // Backquote
        { 0x02, { L"&", L"1", nullptr, nullptr }, 0, true },  // Digit1
        { 0x03, { L"\u00e9", L"2", L"~", nullptr }, 4, true },  // Digit2
        { 0x04, { L"\"", L"3", L"#", nullptr }, 0, true },  // Digit3
        { 0x05, { L"'", L"4", L"{", nullptr }, 0, true },  // Digit4
        { 0x06, { L"(", L"5", L"[", nullptr }, 0, true },  // Digit5
        { 0x07, { L"-", L"6", L"|", nullptr }, 0, true },  // Digit6
        { 0x08, { L"\u00e8", L"7", L"`", nullptr }, 4, true },  // Digit7
        { 0x09, { L"_", L"8", L"\\", nullptr }, 0, true },  // Digit8
        { 0x0a, { L"\u00e7", L"9", L"^", nullptr }, 0, true },  // Digit9
        { 0x0b, { L"\u00e0", L"0", L"@", nullptr }, 0, true },  // Digit0
        { 0x0c, { L")", L"\u00b0", L"]", nullptr }, 0, true },  // Minus
        { 0x0d, { L"=", L"+", L"}", nullptr }, 0, true },  // Equal
        { 0x10, { L"a", L"A", nullptr, nullptr }, 0, true },  // KeyQ
        { 0x11, { L"z", L"Z", nullptr, nullptr }, 0, true },  // KeyW
        { 0x12, { L"e", L"E", L"\u20ac", nullptr }, 0, true },  // KeyE
        { 0x13, { L"r", L"R", nullptr, nullptr }, 0, true },  // KeyR
        { 0x14, { L"t", L"T", nullptr, nullptr }, 0, true },  // KeyT
        { 0x15, { L"y", L"Y", nullptr, nullptr }, 0, true },  // KeyY
        { 0x16, { L"u", L"U", nullptr, nullptr }, 0, true },  // KeyU
        { 0x17, { L"i", L"I", nullptr, nullptr }, 0, true },  // KeyI
        { 0x18, { L"o", L"O", nullptr, nullptr }, 0, true },  // KeyO
        { 0x19, { L"p", L"P", nullptr, nullptr }, 0, true },  // KeyP
        { 0x1a, { L"^", L"\u00a8", nullptr, nullptr }, 3, true },  // BracketLeft
        { 0x1b, { L"$", L"\u00a3", L"\u00a4", nullptr }, 0, true },  // BracketRight
        { 0x2b, { L"*", L"\u00b5", nullptr, nullptr }, 0, true },  // Backslash
        { 0x1e, { L"q", L"Q", nullptr, nullptr }, 0, true },  // KeyA
        { 0x1f, { L"s", L"S", nullptr, nullptr }, 0, true },  // KeyS
        { 0x20, { L"d", L"D", nullptr, nullptr }, 0, true },  // KeyD
        { 0x21, { L"f", L"F", nullptr, nullptr }, 0, true },  // KeyF
        { 0x22, { L"g", L"G", nullptr, nullptr }, 0, true },  // KeyG
        { 0x23, { L"h", L"H", nullptr, nullptr }, 0, true },  // KeyH
        { 0x24, { L"j", L"J", nullptr, nullptr }, 0, true },  // KeyJ
        { 0x25, { L"k", L"K", nullptr, nullptr }, 0, true },  // KeyK
        { 0x26, { L"l", L"L", nullptr, nullptr }, 0, true },  // KeyL
        { 0x27, { L"m", L"M", nullptr, nullptr }, 0, true },  // Semicolon
        { 0x28, { L"\u00f9", L"%", nullptr, nullptr }, 0, true },  // Quote
        { 0x56, { L"<", L">", nullptr, nullptr }, 0, false },  // IntlBackslash
        { 0x2c, { L"w", L"W", nullptr, nullptr }, 0, true },  // KeyZ
        { 0x2d, { L"x", L"X", nullptr, nullptr }, 0, true },  // KeyX
        { 0x2e, { L"c", L"C", nullptr, nullptr }, 0, true },  // KeyC
        { 0x2f, { L"v", L"V", nullptr, nullptr }, 0, true },  // KeyV
        { 0x30, { L"b", L"B", nullptr, nullptr }, 0, true },  // KeyB
        { 0x31, { L"n", L"N", nullptr, nullptr }, 0, true },  // KeyN
        { 0x32, { L",", L"?", nullptr, nullptr }, 0, true },  // KeyM
        { 0x33, { L";", L".", nullptr, nullptr }, 0, true },  // Comma
        { 0x34, { L":", L"/", nullptr, nullptr }, 0, true },  // Period
        { 0x35, { L"!", L"\u00a7", nullptr, nullptr }, 0, true },  // Slash
        { 0x39, { L" ", L" ", nullptr, nullptr }, 0, false },  // Space
    };

    inline constexpr Touche kQwerty[] = {
        { 0x29, { L"`", L"~", nullptr, nullptr }, 0, false },  // Backquote
        { 0x02, { L"1", L"!", nullptr, nullptr }, 0, false },  // Digit1
        { 0x03, { L"2", L"@", nullptr, nullptr }, 0, false },  // Digit2
        { 0x04, { L"3", L"#", nullptr, nullptr }, 0, false },  // Digit3
        { 0x05, { L"4", L"$", nullptr, nullptr }, 0, false },  // Digit4
        { 0x06, { L"5", L"%", nullptr, nullptr }, 0, false },  // Digit5
        { 0x07, { L"6", L"^", nullptr, nullptr }, 0, false },  // Digit6
        { 0x08, { L"7", L"&", nullptr, nullptr }, 0, false },  // Digit7
        { 0x09, { L"8", L"*", nullptr, nullptr }, 0, false },  // Digit8
        { 0x0a, { L"9", L"(", nullptr, nullptr }, 0, false },  // Digit9
        { 0x0b, { L"0", L")", nullptr, nullptr }, 0, false },  // Digit0
        { 0x0c, { L"-", L"_", nullptr, nullptr }, 0, false },  // Minus
        { 0x0d, { L"=", L"+", nullptr, nullptr }, 0, false },  // Equal
        { 0x10, { L"q", L"Q", nullptr, nullptr }, 0, true },  // KeyQ
        { 0x11, { L"w", L"W", nullptr, nullptr }, 0, true },  // KeyW
        { 0x12, { L"e", L"E", nullptr, nullptr }, 0, true },  // KeyE
        { 0x13, { L"r", L"R", nullptr, nullptr }, 0, true },  // KeyR
        { 0x14, { L"t", L"T", nullptr, nullptr }, 0, true },  // KeyT
        { 0x15, { L"y", L"Y", nullptr, nullptr }, 0, true },  // KeyY
        { 0x16, { L"u", L"U", nullptr, nullptr }, 0, true },  // KeyU
        { 0x17, { L"i", L"I", nullptr, nullptr }, 0, true },  // KeyI
        { 0x18, { L"o", L"O", nullptr, nullptr }, 0, true },  // KeyO
        { 0x19, { L"p", L"P", nullptr, nullptr }, 0, true },  // KeyP
        { 0x1a, { L"[", L"{", nullptr, nullptr }, 0, false },  // BracketLeft
        { 0x1b, { L"]", L"}", nullptr, nullptr }, 0, false },  // BracketRight
        { 0x2b, { L"\\", L"|", nullptr, nullptr }, 0, false },  // Backslash
        { 0x1e, { L"a", L"A", nullptr, nullptr }, 0, true },  // KeyA
        { 0x1f, { L"s", L"S", nullptr, nullptr }, 0, true },  // KeyS
        { 0x20, { L"d", L"D", nullptr, nullptr }, 0, true },  // KeyD
        { 0x21, { L"f", L"F", nullptr, nullptr }, 0, true },  // KeyF
        { 0x22, { L"g", L"G", nullptr, nullptr }, 0, true },  // KeyG
        { 0x23, { L"h", L"H", nullptr, nullptr }, 0, true },  // KeyH
        { 0x24, { L"j", L"J", nullptr, nullptr }, 0, true },  // KeyJ
        { 0x25, { L"k", L"K", nullptr, nullptr }, 0, true },  // KeyK
        { 0x26, { L"l", L"L", nullptr, nullptr }, 0, true },  // KeyL
        { 0x27, { L";", L":", nullptr, nullptr }, 0, false },  // Semicolon
        { 0x28, { L"'", L"\"", nullptr, nullptr }, 0, false },  // Quote
        { 0x56, { L"\\", L"|", nullptr, nullptr }, 0, false },  // IntlBackslash
        { 0x2c, { L"z", L"Z", nullptr, nullptr }, 0, true },  // KeyZ
        { 0x2d, { L"x", L"X", nullptr, nullptr }, 0, true },  // KeyX
        { 0x2e, { L"c", L"C", nullptr, nullptr }, 0, true },  // KeyC
        { 0x2f, { L"v", L"V", nullptr, nullptr }, 0, true },  // KeyV
        { 0x30, { L"b", L"B", nullptr, nullptr }, 0, true },  // KeyB
        { 0x31, { L"n", L"N", nullptr, nullptr }, 0, true },  // KeyN
        { 0x32, { L"m", L"M", nullptr, nullptr }, 0, true },  // KeyM
        { 0x33, { L",", L"<", nullptr, nullptr }, 0, false },  // Comma
        { 0x34, { L".", L">", nullptr, nullptr }, 0, false },  // Period
        { 0x35, { L"/", L"?", nullptr, nullptr }, 0, false },  // Slash
        { 0x39, { L" ", L" ", nullptr, nullptr }, 0, false },  // Space
    };

    inline constexpr Touche kQwertz[] = {
        { 0x29, { L"^", L"\u00b0", nullptr, nullptr }, 1, false },  // Backquote
        { 0x02, { L"1", L"!", nullptr, nullptr }, 0, true },  // Digit1
        { 0x03, { L"2", L"\"", L"\u00b2", nullptr }, 0, true },  // Digit2
        { 0x04, { L"3", L"\u00a7", L"\u00b3", nullptr }, 0, true },  // Digit3
        { 0x05, { L"4", L"$", nullptr, nullptr }, 0, true },  // Digit4
        { 0x06, { L"5", L"%", nullptr, nullptr }, 0, true },  // Digit5
        { 0x07, { L"6", L"&", nullptr, nullptr }, 0, true },  // Digit6
        { 0x08, { L"7", L"/", L"{", nullptr }, 0, true },  // Digit7
        { 0x09, { L"8", L"(", L"[", nullptr }, 0, true },  // Digit8
        { 0x0a, { L"9", L")", L"]", nullptr }, 0, true },  // Digit9
        { 0x0b, { L"0", L"=", L"}", nullptr }, 0, true },  // Digit0
        { 0x0c, { L"\u00df", L"?", L"\\", L"\u1e9e" }, 0, true },  // Minus
        { 0x0d, { L"\u00b4", L"`", nullptr, nullptr }, 3, false },  // Equal
        { 0x10, { L"q", L"Q", L"@", nullptr }, 0, true },  // KeyQ
        { 0x11, { L"w", L"W", nullptr, nullptr }, 0, true },  // KeyW
        { 0x12, { L"e", L"E", L"\u20ac", nullptr }, 0, true },  // KeyE
        { 0x13, { L"r", L"R", nullptr, nullptr }, 0, true },  // KeyR
        { 0x14, { L"t", L"T", nullptr, nullptr }, 0, true },  // KeyT
        { 0x15, { L"z", L"Z", nullptr, nullptr }, 0, true },  // KeyY
        { 0x16, { L"u", L"U", nullptr, nullptr }, 0, true },  // KeyU
        { 0x17, { L"i", L"I", nullptr, nullptr }, 0, true },  // KeyI
        { 0x18, { L"o", L"O", nullptr, nullptr }, 0, true },  // KeyO
        { 0x19, { L"p", L"P", nullptr, nullptr }, 0, true },  // KeyP
        { 0x1a, { L"\u00fc", L"\u00dc", nullptr, nullptr }, 0, true },  // BracketLeft
        { 0x1b, { L"+", L"*", L"~", nullptr }, 0, true },  // BracketRight
        { 0x2b, { L"#", L"'", nullptr, nullptr }, 0, true },  // Backslash
        { 0x1e, { L"a", L"A", nullptr, nullptr }, 0, true },  // KeyA
        { 0x1f, { L"s", L"S", nullptr, nullptr }, 0, true },  // KeyS
        { 0x20, { L"d", L"D", nullptr, nullptr }, 0, true },  // KeyD
        { 0x21, { L"f", L"F", nullptr, nullptr }, 0, true },  // KeyF
        { 0x22, { L"g", L"G", nullptr, nullptr }, 0, true },  // KeyG
        { 0x23, { L"h", L"H", nullptr, nullptr }, 0, true },  // KeyH
        { 0x24, { L"j", L"J", nullptr, nullptr }, 0, true },  // KeyJ
        { 0x25, { L"k", L"K", nullptr, nullptr }, 0, true },  // KeyK
        { 0x26, { L"l", L"L", nullptr, nullptr }, 0, true },  // KeyL
        { 0x27, { L"\u00f6", L"\u00d6", nullptr, nullptr }, 0, true },  // Semicolon
        { 0x28, { L"\u00e4", L"\u00c4", nullptr, nullptr }, 0, true },  // Quote
        { 0x56, { L"<", L">", L"|", nullptr }, 0, false },  // IntlBackslash
        { 0x2c, { L"y", L"Y", nullptr, nullptr }, 0, true },  // KeyZ
        { 0x2d, { L"x", L"X", nullptr, nullptr }, 0, true },  // KeyX
        { 0x2e, { L"c", L"C", nullptr, nullptr }, 0, true },  // KeyC
        { 0x2f, { L"v", L"V", nullptr, nullptr }, 0, true },  // KeyV
        { 0x30, { L"b", L"B", nullptr, nullptr }, 0, true },  // KeyB
        { 0x31, { L"n", L"N", nullptr, nullptr }, 0, true },  // KeyN
        { 0x32, { L"m", L"M", L"\u00b5", nullptr }, 0, true },  // KeyM
        { 0x33, { L",", L";", nullptr, nullptr }, 0, true },  // Comma
        { 0x34, { L".", L":", nullptr, nullptr }, 0, true },  // Period
        { 0x35, { L"-", L"_", nullptr, nullptr }, 0, false },  // Slash
        { 0x39, { L" ", L" ", nullptr, nullptr }, 0, false },  // Space
    };

    inline constexpr Touche kQwertyIntl[] = {
        { 0x29, { L"`", L"~", nullptr, nullptr }, 3, false },  // Backquote
        { 0x02, { L"1", L"!", L"\u00a1", L"\u00b9" }, 0, false },  // Digit1
        { 0x03, { L"2", L"@", L"\u00b2", nullptr }, 0, false },  // Digit2
        { 0x04, { L"3", L"#", L"\u00b3", nullptr }, 0, false },  // Digit3
        { 0x05, { L"4", L"$", L"\u00a4", L"\u00a3" }, 0, false },  // Digit4
        { 0x06, { L"5", L"%", L"\u20ac", nullptr }, 0, false },  // Digit5
        { 0x07, { L"6", L"^", L"\u00bc", nullptr }, 2, false },  // Digit6
        { 0x08, { L"7", L"&", L"\u00bd", nullptr }, 0, false },  // Digit7
        { 0x09, { L"8", L"*", L"\u00be", nullptr }, 0, false },  // Digit8
        { 0x0a, { L"9", L"(", L"\u2018", nullptr }, 0, false },  // Digit9
        { 0x0b, { L"0", L")", L"\u2019", nullptr }, 0, false },  // Digit0
        { 0x0c, { L"-", L"_", L"\u00a5", nullptr }, 0, false },  // Minus
        { 0x0d, { L"=", L"+", L"\u00d7", L"\u00f7" }, 0, false },  // Equal
        { 0x10, { L"q", L"Q", L"\u00e4", L"\u00c4" }, 0, true },  // KeyQ
        { 0x11, { L"w", L"W", L"\u00e5", L"\u00c5" }, 0, true },  // KeyW
        { 0x12, { L"e", L"E", L"\u00e9", L"\u00c9" }, 0, true },  // KeyE
        { 0x13, { L"r", L"R", L"\u00ae", nullptr }, 0, true },  // KeyR
        { 0x14, { L"t", L"T", L"\u00fe", L"\u00de" }, 0, true },  // KeyT
        { 0x15, { L"y", L"Y", L"\u00fc", L"\u00dc" }, 0, true },  // KeyY
        { 0x16, { L"u", L"U", L"\u00fa", L"\u00da" }, 0, true },  // KeyU
        { 0x17, { L"i", L"I", L"\u00ed", L"\u00cd" }, 0, true },  // KeyI
        { 0x18, { L"o", L"O", L"\u00f3", L"\u00d3" }, 0, true },  // KeyO
        { 0x19, { L"p", L"P", L"\u00f6", L"\u00d6" }, 0, true },  // KeyP
        { 0x1a, { L"[", L"{", L"\u00ab", nullptr }, 0, false },  // BracketLeft
        { 0x1b, { L"]", L"}", L"\u00bb", nullptr }, 0, false },  // BracketRight
        { 0x2b, { L"\\", L"|", L"\u00ac", L"\u00a6" }, 0, false },  // Backslash
        { 0x1e, { L"a", L"A", L"\u00e1", L"\u00c1" }, 0, true },  // KeyA
        { 0x1f, { L"s", L"S", L"\u00df", L"\u00a7" }, 0, true },  // KeyS
        { 0x20, { L"d", L"D", L"\u00f0", L"\u00d0" }, 0, true },  // KeyD
        { 0x21, { L"f", L"F", nullptr, nullptr }, 0, true },  // KeyF
        { 0x22, { L"g", L"G", nullptr, nullptr }, 0, true },  // KeyG
        { 0x23, { L"h", L"H", nullptr, nullptr }, 0, true },  // KeyH
        { 0x24, { L"j", L"J", nullptr, nullptr }, 0, true },  // KeyJ
        { 0x25, { L"k", L"K", nullptr, nullptr }, 0, true },  // KeyK
        { 0x26, { L"l", L"L", L"\u00f8", L"\u00d8" }, 0, true },  // KeyL
        { 0x27, { L";", L":", L"\u00b6", L"\u00b0" }, 0, false },  // Semicolon
        { 0x28, { L"'", L"\"", L"\u00b4", L"\u00a8" }, 3, false },  // Quote
        { 0x56, { L"\\", L"|", nullptr, nullptr }, 0, false },  // IntlBackslash
        { 0x2c, { L"z", L"Z", L"\u00e6", L"\u00c6" }, 0, true },  // KeyZ
        { 0x2d, { L"x", L"X", nullptr, nullptr }, 0, true },  // KeyX
        { 0x2e, { L"c", L"C", L"\u00a9", L"\u00a2" }, 0, true },  // KeyC
        { 0x2f, { L"v", L"V", nullptr, nullptr }, 0, true },  // KeyV
        { 0x30, { L"b", L"B", nullptr, nullptr }, 0, true },  // KeyB
        { 0x31, { L"n", L"N", L"\u00f1", L"\u00d1" }, 0, true },  // KeyN
        { 0x32, { L"m", L"M", L"\u00b5", nullptr }, 0, true },  // KeyM
        { 0x33, { L",", L"<", L"\u00e7", L"\u00c7" }, 0, false },  // Comma
        { 0x34, { L".", L">", nullptr, nullptr }, 0, false },  // Period
        { 0x35, { L"/", L"?", L"\u00bf", nullptr }, 0, false },  // Slash
        { 0x39, { L" ", L" ", nullptr, nullptr }, 0, false },  // Space
    };

    inline constexpr Composition kCompositions[] = {
        { L'^', L'a', L'\u00e2' },
        { L'^', L'e', L'\u00ea' },
        { L'^', L'i', L'\u00ee' },
        { L'^', L'o', L'\u00f4' },
        { L'^', L'u', L'\u00fb' },
        { L'^', L'A', L'\u00c2' },
        { L'^', L'E', L'\u00ca' },
        { L'^', L'I', L'\u00ce' },
        { L'^', L'O', L'\u00d4' },
        { L'^', L'U', L'\u00db' },
        { L'\u00a8', L'a', L'\u00e4' },
        { L'\u00a8', L'e', L'\u00eb' },
        { L'\u00a8', L'i', L'\u00ef' },
        { L'\u00a8', L'o', L'\u00f6' },
        { L'\u00a8', L'u', L'\u00fc' },
        { L'\u00a8', L'y', L'\u00ff' },
        { L'\u00a8', L'A', L'\u00c4' },
        { L'\u00a8', L'E', L'\u00cb' },
        { L'\u00a8', L'I', L'\u00cf' },
        { L'\u00a8', L'O', L'\u00d6' },
        { L'\u00a8', L'U', L'\u00dc' },
        { L'\u00a8', L'Y', L'\u0178' },
        { L'~', L'a', L'\u00e3' },
        { L'~', L'n', L'\u00f1' },
        { L'~', L'o', L'\u00f5' },
        { L'~', L'A', L'\u00c3' },
        { L'~', L'N', L'\u00d1' },
        { L'~', L'O', L'\u00d5' },
        { L'`', L'a', L'\u00e0' },
        { L'`', L'e', L'\u00e8' },
        { L'`', L'i', L'\u00ec' },
        { L'`', L'o', L'\u00f2' },
        { L'`', L'u', L'\u00f9' },
        { L'`', L'A', L'\u00c0' },
        { L'`', L'E', L'\u00c8' },
        { L'`', L'I', L'\u00cc' },
        { L'`', L'O', L'\u00d2' },
        { L'`', L'U', L'\u00d9' },
        { L'\u00b4', L'a', L'\u00e1' },
        { L'\u00b4', L'e', L'\u00e9' },
        { L'\u00b4', L'i', L'\u00ed' },
        { L'\u00b4', L'o', L'\u00f3' },
        { L'\u00b4', L'u', L'\u00fa' },
        { L'\u00b4', L'y', L'\u00fd' },
        { L'\u00b4', L'A', L'\u00c1' },
        { L'\u00b4', L'E', L'\u00c9' },
        { L'\u00b4', L'I', L'\u00cd' },
        { L'\u00b4', L'O', L'\u00d3' },
        { L'\u00b4', L'U', L'\u00da' },
        { L'\u00b4', L'Y', L'\u00dd' },
        { L'\'', L'a', L'\u00e1' },
        { L'\'', L'e', L'\u00e9' },
        { L'\'', L'i', L'\u00ed' },
        { L'\'', L'o', L'\u00f3' },
        { L'\'', L'u', L'\u00fa' },
        { L'\'', L'y', L'\u00fd' },
        { L'\'', L'c', L'\u00e7' },
        { L'\'', L'A', L'\u00c1' },
        { L'\'', L'E', L'\u00c9' },
        { L'\'', L'I', L'\u00cd' },
        { L'\'', L'O', L'\u00d3' },
        { L'\'', L'U', L'\u00da' },
        { L'\'', L'Y', L'\u00dd' },
        { L'\'', L'C', L'\u00c7' },
        { L'"', L'a', L'\u00e4' },
        { L'"', L'e', L'\u00eb' },
        { L'"', L'i', L'\u00ef' },
        { L'"', L'o', L'\u00f6' },
        { L'"', L'u', L'\u00fc' },
        { L'"', L'y', L'\u00ff' },
        { L'"', L'A', L'\u00c4' },
        { L'"', L'E', L'\u00cb' },
        { L'"', L'I', L'\u00cf' },
        { L'"', L'O', L'\u00d6' },
        { L'"', L'U', L'\u00dc' },
    };

    inline constexpr Disposition kDispositions[] = {
        { "azerty", kAzerty, std::size(kAzerty) },
        { "qwerty", kQwerty, std::size(kQwerty) },
        { "qwertz", kQwertz, std::size(kQwertz) },
        { "qwerty-intl", kQwertyIntl, std::size(kQwertyIntl) },
    };

}  // namespace vk::clavier::tables
