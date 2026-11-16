#!/usr/bin/env python3
# Make the C text tables from the language files lang/*.json.
#
# A language file is a JSON object. The key "_language" gives the code, the name and the plural rule:
#   "_language": { "code": "uk", "name": "Українська", "plural": "uk" }
# The other keys are texts. A value is one of:
#   "text"                                   a text; printf conversions (%s, %d) are allowed
#   { "one": "...", "few": "...", ... }      the plural forms of the rule (uk: one, few, many; en: one, other)
#   [ "item", "item", ... ]                  a list (for example the month names)
# Keys that start with "js." go to the page scripts as one JSON object (texts_js()).
#
# The first language (--default) gives the keys. Each other language must have the same keys, the same kinds,
# the same list lengths and the same printf conversions in the same order: else the build stops.
#
# Output: texts_ids.h (the enum of the keys) and texts_data.c (the tables of all languages).

import argparse
import json
import os
import re
import sys

PLURAL_FORMS = {
    'uk': ['one', 'few', 'many'],
    'en': ['one', 'other'],
}
SLOTS_PER_PLURAL = max(len(forms) for forms in PLURAL_FORMS.values())

# A printf conversion: flags, width, precision, length, conversion. "%%" is a plain percent sign.
CONVERSION = re.compile(r'%(?:%|[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(hh|h|ll|l|j|z|t|L)?([diouxXeEfFgGaAcsp]))')


def fail(message):
    print('lang.py: ' + message, file=sys.stderr)
    sys.exit(1)


def conversions(text):
    return [(m.group(1) or '') + m.group(2) for m in CONVERSION.finditer(text) if m.group(0) != '%%']


def c_name(key):
    name = re.sub(r'[^A-Za-z0-9]', '_', key).upper()
    return 'T_' + name


def c_string(text):
    out = []
    for ch in text:
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 32:
            out.append('\\%03o' % ord(ch))
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def load(path):
    try:
        with open(path, encoding='utf-8') as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        fail('%s: %s' % (path, e))
    meta = data.pop('_language', None)
    if not isinstance(meta, dict) or not all(k in meta for k in ('code', 'name', 'plural')):
        fail('%s: no "_language" with "code", "name" and "plural"' % path)
    if meta['plural'] not in PLURAL_FORMS:
        fail('%s: unknown plural rule "%s"' % (path, meta['plural']))
    return meta, data


def kind(value):
    if isinstance(value, str):
        return 'text'
    if isinstance(value, dict):
        return 'plural'
    if isinstance(value, list):
        return 'list'
    return None


def check(path, meta, data, base_meta, base):
    forms = PLURAL_FORMS[meta['plural']]
    for key, value in data.items():
        if key not in base:
            fail('%s: key "%s" is not in the default language' % (path, key))
    for key, base_value in base.items():
        if key not in data:
            fail('%s: no key "%s"' % (path, key))
        value = data[key]
        k = kind(value)
        if k != kind(base_value):
            fail('%s: "%s" must be a %s' % (path, key, kind(base_value)))
        base_texts = list(base_value.values()) if isinstance(base_value, dict) else (
            base_value if isinstance(base_value, list) else [base_value])
        want = conversions(base_texts[0])
        if k == 'plural':
            if sorted(value) != sorted(forms):
                fail('%s: "%s" must have the forms %s' % (path, key, ', '.join(forms)))
            texts = list(value.values())
        elif k == 'list':
            if len(value) != len(base_value):
                fail('%s: "%s" must have %d items' % (path, key, len(base_value)))
            texts = value
            want = None     # list items do not have conversions
        else:
            texts = [value]
        for text in texts:
            if not isinstance(text, str):
                fail('%s: "%s" has a value that is not a text' % (path, key))
            if want is not None and conversions(text) != want:
                fail('%s: "%s" has the conversions %s, the default language has %s'
                     % (path, key, conversions(text) or 'none', want or 'none'))
            if k == 'list' and conversions(text):
                fail('%s: "%s" is a list: no conversions in its items' % (path, key))


def slots_of(meta, key, value):
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return list(value)
    forms = PLURAL_FORMS[meta['plural']]
    out = [value[f] for f in forms]
    while len(out) < SLOTS_PER_PLURAL:
        out.append(out[-1])
    return out


def js_object(data):
    obj = {k[3:]: v for k, v in data.items() if k.startswith('js.')}
    # Inside <script>: "</" must not close the element.
    return json.dumps(obj, ensure_ascii=False, separators=(',', ':')).replace('</', '<\\/')


def write_if_changed(path, text):
    try:
        with open(path, encoding='utf-8', newline='') as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True, help='folder for texts_ids.h and texts_data.c')
    parser.add_argument('--default', default='uk', help='the code of the default language (the keys)')
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()

    langs = [load(p) + (p,) for p in sorted(args.files)]
    langs.sort(key=lambda l: (l[0]['code'] != args.default, l[0]['code']))
    base_meta, base, base_path = langs[0]
    if base_meta['code'] != args.default:
        fail('no language "%s"' % args.default)
    for key, value in base.items():
        k = kind(value)
        if k is None:
            fail('%s: "%s" has an unknown kind' % (base_path, key))
        if k == 'plural' and sorted(value) != sorted(PLURAL_FORMS[base_meta['plural']]):
            fail('%s: "%s" must have the forms %s' % (base_path, key, ', '.join(PLURAL_FORMS[base_meta['plural']])))
        if k == 'plural':
            forms = list(value.values())
            if any(conversions(f) != conversions(forms[0]) for f in forms):
                fail('%s: the forms of "%s" have other conversions' % (base_path, key))
    for meta, data, path in langs[1:]:
        check(path, meta, data, base_meta, base)

    keys = list(base)
    names = [c_name(k) for k in keys]
    if len(set(names)) != len(names):
        fail('two keys give the same C name')

    # The slot of each key: the same for all languages.
    first_slot = []
    count = 0
    for key in keys:
        first_slot.append(count)
        count += len(slots_of(base_meta, key, base[key]))

    h = ['// Made by tools/lang.py from lang/*.json. Do not edit.', '#pragma once', '',
         'typedef enum {']
    h += ['    %s,' % n for n in names]
    h += ['    T_COUNT', '} text_id_t;', '']
    for key, name in zip(keys, names):
        if isinstance(base[key], list):
            h.append('#define %s_ITEMS %d' % (name, len(base[key])))
    h += ['', '#define TEXTS_LANGUAGE_COUNT %d' % len(langs), '#define TEXTS_SLOT_COUNT %d' % count, '']

    c = ['// Made by tools/lang.py from lang/*.json. Do not edit.', '#include "texts_data.h"', '',
         'const unsigned short texts_first_slot[T_COUNT] = {']
    c += ['    %d, // %s' % (s, k) for s, k in zip(first_slot, keys)]
    c += ['};', '']
    for meta, data, _ in langs:
        c.append('static const char *const SLOTS_%s[%d] = {' % (meta['code'].upper(), count))
        for key in keys:
            for text in slots_of(meta, key, data[key]):
                c.append('    %s,' % c_string(text))
        c += ['};', '']
    c.append('const texts_language_t texts_languages[TEXTS_LANGUAGE_COUNT] = {')
    for meta, data, _ in langs:
        c.append('    { %s, %s, texts_plural_%s, SLOTS_%s, %s },' % (
            c_string(meta['code']), c_string(meta['name']), meta['plural'], meta['code'].upper(),
            c_string(js_object(data))))
    c += ['};', '']

    os.makedirs(args.out, exist_ok=True)
    write_if_changed(os.path.join(args.out, 'texts_ids.h'), '\n'.join(h))
    write_if_changed(os.path.join(args.out, 'texts_data.c'), '\n'.join(c))


if __name__ == '__main__':
    main()
