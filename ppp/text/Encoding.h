#pragma once

#include <ppp/stdafx.h>

namespace ppp {
    namespace text {
        /*
            European languages
                ASCII, ISO−8859−{1,2,3,4,5,7,9,10,13,14,15,16}, KOI8−R, KOI8−U, KOI8−RU, CP{1250,1251,1252,1253,1254,1257}, CP{850,866,1131}, Mac{Roman,CentralEurope,Iceland,Croatian,Romania}, Mac{Cyrillic,Ukraine,Greek,Turkish}, Macintosh
            Semitic languages
                ISO−8859−{6,8}, CP{1255,1256}, CP862, Mac{Hebrew,Arabic}
            Japanese
                EUC−JP, SHIFT_JIS, CP932, ISO−2022−JP, ISO−2022−JP−2, ISO−2022−JP−1
            Chinese
                EUC−CN, HZ, GBK, CP936, GB2312, GB18030, EUC−TW, BIG5, CP950, BIG5−HKSCS, BIG5−HKSCS:2001, BIG5−HKSCS:1999, ISO−2022−CN, ISO−2022−CN−EXT
            Korean
                EUC−KR, CP949, ISO−2022−KR, JOHAB
            Armenian
                ARMSCII−8
            Georgian
                Georgian−Academy, Georgian−PS
            Tajik
                KOI8−T
            Kazakh
                PT154, RK1048
            Thai
                TIS−620, CP874, MacThai
            Laotian
                MuleLao−1, CP1133
            Vietnamese
                VISCII, TCVN, CP1258
            Platform specifics
                HP−ROMAN8, NEXTSTEP
            Full Unicode
                UTF−8
                UCS−2, UCS−2BE, UCS−2LE
                UCS−4, UCS−4BE, UCS−4LE
                UTF−16, UTF−16BE, UTF−16LE
                UTF−32, UTF−32BE, UTF−32LE
                UTF−7
                C99, JAVA
            Full Unicode, in terms of UInt16 or UInt32
            (with machine dependent endianness and alignment)
                UCS−2−INTERNAL, UCS−4−INTERNAL
            Locale dependent, in terms of char or wchar_t
                (with machine dependent endianness and alignment, and with semantics depending on the OS and the current LC_CTYPE locale facet)
                char, wchar_t
            When configured with the option −−enable−extra−encodings, it also provides support for a few extra encodings:
            European languages
                CP{437,737,775,852,853,855,857,858,860,861,863,865,869,1125}
            Semitic languages
            CP864
            Japanese
                EUC−JISX0213, Shift_JISX0213, ISO−2022−JP−3
            Chinese
                BIG5−2003 (experimental)
            Turkmen
                TDS565
            Platform specifics
            ATARIST, RISCOS−LATIN1
        */
        class Encoding final {
        public:
            static constexpr int                            ASCII            = 0;
            static constexpr int                            UTF8             = 1;
            static constexpr int                            Unicode          = 2;
            static constexpr int                            BigEndianUnicode = 3;

        public:
            static std::wstring                             utf8_to_wstring(const std::string& s) noexcept;
            static std::string                              wstring_to_utf8(const std::wstring& s) noexcept;
            static std::wstring                             ascii_to_wstring(const std::string& s) noexcept;
            static std::wstring                             ascii_to_wstring2(const std::string& s) noexcept;
            static std::string                              wstring_to_ascii(const std::wstring& s) noexcept;
        };
    }
}