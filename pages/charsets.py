# Наборы символов G0 телетекста — как в decode-orc
# (orc/plugins/stages/common/vbi-services/teletext_page_decoder.cpp, ETSI EN 300 706 §15).
#
# Основной набор — свойство передачи, а не страницы: его объявляют пакеты X/28/0
# Format 1 или M/29/0 (обозначение 0100 + национальные биты 000/100/101 = кириллица
# сербская/русская/украинская), а без них он задаётся «местной практикой», то есть
# настройкой декодера. Для латиницы национальный вариант (13 позиций) выбирают
# флаги C12–C14 в заголовке страницы (C12 — старший).
#   table(набор, вариант) -> строка из 96 символов для кодов 0x20..0x7F
CHARSETS = {                                   # настройка проекта -> название
    'latin': 'Latin (variant from the page flags)',
    'cyr2': 'Cyrillic — Russian/Bulgarian',
    'cyr1': 'Cyrillic — Serbian/Croatian',
    'cyr3': 'Cyrillic — Ukrainian',
}
NATIONAL = ['English', 'German', 'Swedish/Finnish/Hungarian', 'Italian',
            'French', 'Portuguese/Spanish', 'Czech/Slovak', 'English']

LATIN = ''.join(chr(c) for c in range(0x20, 0x7f)) + '■'
NAT_POS = [0x23, 0x24, 0x40, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x7B, 0x7C, 0x7D, 0x7E]
NAT_SUBSETS = [                                # Table 36, по порядку C12 C13 C14
    '£$@←½→↑#—¼‖¾÷',                           # английский
    '#$§ÄÖÜ^_°äöüß',                           # немецкий
    '#¤ÉÄÖÅÜ_éäöåü',                           # шведский/финский/венгерский
    '£$é°ç→↑#ùàòèì',                           # итальянский
    'éïàëêùî#èâôûç',                           # французский
    'ç$¡áéíóú¿üñèà',                           # португальский/испанский
    '#ůčťžýířéáěúš',                           # чешский/словацкий
]
_DIG = ' !"#$%&\'()*+,-./0123456789:;<=>?'
CYR = {                                        # Tables 38–40
    'cyr1': _DIG + 'ЧАБЦДЕФГХИЈКЛМНОПЌРСТУВЃЉЊЗЋЖЂШЏчабцдефгхијклмнопќрстувѓљњзћжђш■',
    'cyr2': _DIG.replace('&', 'ы') + 'ЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬЪЗШЭЩЧЫюабцдефгхийклмнопярстужвьъзшэщч■',
    'cyr3': _DIG.replace('&', 'ї') + 'ЮАБЦДЕФГХИЙКЛМНОПЯРСТУЖВЬІЗШЄЩЧЇюабцдефгхийклмнопярстужвьізшєщч■',
}
assert all(len(t) == 96 for t in CYR.values())

_cache = {}
def table(charset='latin', national=1):
    """96 символов для кодов 0x20..0x7F."""
    key = (charset, national)
    if key not in _cache:
        if charset in CYR: t = CYR[charset]
        else:
            t = list(LATIN); sub = NAT_SUBSETS[national if 0 <= (national or 0) < 7 else 0]
            for p, ch in zip(NAT_POS, sub): t[p - 0x20] = ch
            t = ''.join(t)
        _cache[key] = t
    return _cache[key]

def reverse(t):
    """Символ с клавиатуры -> код (для правки страниц)."""
    return {ch: i + 0x20 for i, ch in enumerate(t) if ch != '■'}

def national_of(ctrl_byte_value):
    """Хэмминг‑декодированный 10‑й байт заголовка (C11..C14 в битах 0..3) -> вариант C12C13C14."""
    d = ctrl_byte_value
    return (((d >> 1) & 1) << 2) | (((d >> 2) & 1) << 1) | ((d >> 3) & 1)

# ---------- угадывание набора по тексту (decode-orc этого не делает: у него это настройка)
WORDS = {
    'cyr': {'и', 'в', 'на', 'не', 'с', 'по', 'от', 'для', 'что', 'из', 'к', 'о', 'за', 'до', 'как',
            'это', 'стр', 'россии', 'года', 'при'},
    'latin': {'der', 'die', 'das', 'und', 'mit', 'für', 'von', 'the', 'and', 'of', 'to', 'for', 'le',
              'la', 'les', 'et', 'des', 'el', 'los', 'y', 'del', 'il', 'di', 'che', 'och', 'och', 'på'},
}
def guess(rows_iter, national=1):
    """rows_iter — ряды (40 кодов) с нескольких страниц. -> 'latin' или 'cyr2'."""
    import re
    score = {'latin': 0, 'cyr2': 0}
    tl, tc = table('latin', national), table('cyr2')
    for b in rows_iter:
        for cs, t, words in (('latin', tl, WORDS['latin']), ('cyr2', tc, WORDS['cyr'])):
            s = ''.join(t[(c & 0x7f) - 0x20] if (c & 0x7f) >= 0x20 else ' ' for c in b).lower()
            score[cs] += sum(1 for w in re.findall(r'\w+', s) if w in words)
    return 'cyr2' if score['cyr2'] > 1.5 * score['latin'] and score['cyr2'] >= 10 else 'latin'
