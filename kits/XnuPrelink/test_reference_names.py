"""The data-only naming reference: a same-build kernel as digest/length/symbol rows."""
import gzip
import json
import unittest
from types import SimpleNamespace

from fileset_match import (REFERENCE_NAMES_FORMAT, Matcher, ReferenceNames, map_signature,
                           matcher_signature, reference_input)


def table(rows, **extra):
    """A reference-names table from `(digest, length, [symbols])` rows."""
    return json.dumps({'format': REFERENCE_NAMES_FORMAT, 'kernel': {'input_name': 'kernel'},
                       'counts': {'rows': len(rows)},
                       'functions': [list(row) for row in rows], **extra}).encode()


def side(functions, names=None, data_names=None, mapped=()):
    """A Side stub: `functions` maps address -> (digest, length), `names` address -> symbols."""
    symbol_names = {address: list(symbols) for address, symbols in (names or {}).items()}
    symbol_names.update({address: list(symbols) for address, symbols in (data_names or {}).items()})
    owners = {}
    for address, symbols in symbol_names.items():
        if address in functions:
            for symbol in symbols:
                owners.setdefault(symbol, set()).add(address)
    named = {symbol: next(iter(addresses)) for symbol, addresses in owners.items() if len(addresses) == 1}
    mapped = list(mapped) or [(address, address + 8) for address in symbol_names if address not in functions]
    return SimpleNamespace(
        functions={address: {'digest': digest, 'length': length}
                   for address, (digest, length) in functions.items()},
        checked_functions={address: {'digest': digest, 'length': length}
                           for address, (digest, length) in functions.items()},
        symbol_names=symbol_names, named_functions=named,
        in_exec=lambda address: address in functions,
        in_mapped=lambda address: any(b <= address < e for b, e in mapped))


class ReferenceNamesTableTests(unittest.TestCase):
    def test_a_table_is_read_from_json_and_from_gzip(self):
        rows = [('aaaa', 8, ['_one']), ('bbbb', 12, [])]
        plain = ReferenceNames.load(table(rows))
        packed = ReferenceNames.load(gzip.compress(table(rows), mtime=0))
        for loaded in (plain, packed):
            self.assertEqual(len(loaded.rows), 2)
            self.assertEqual(loaded.names_at(0), ('_one',))
            self.assertEqual(loaded.names_at(1), ())
            self.assertEqual(loaded.names_at(2), ())
            self.assertEqual(loaded.named_shapes[('aaaa', 8)], {'_one'})

    def test_a_foreign_format_is_refused(self):
        with self.assertRaises(ValueError):
            ReferenceNames.load(json.dumps({'format': 'something-else/9', 'functions': []}).encode())

    def test_a_shape_shared_by_two_rows_is_not_one_name(self):
        loaded = ReferenceNames.load(table([('aaaa', 8, ['_left']), ('aaaa', 8, ['_right'])]))
        self.assertEqual(loaded.named_shapes[('aaaa', 8)], {'_left', '_right'})

    def test_evidence_invalidates_a_cached_map(self):
        first = ReferenceNames.load(table([('aaaa', 8, ['_one'])]))
        second = ReferenceNames.load(table([('aaaa', 8, ['_two'])]))
        self.assertNotEqual(matcher_signature([('reference-names', first.sha256)]),
                            matcher_signature([('reference-names', second.sha256)]))
        payload = {'evidence': dict(first.evidence)}
        self.assertEqual(map_signature(payload), matcher_signature(first.evidence.items()))

    def test_the_reference_input_accepts_an_image_or_a_table(self):
        image, names = reference_input(b'\xcf\xfa\xed\xfe' + bytes(28))
        self.assertEqual(image[:4], b'\xcf\xfa\xed\xfe')
        self.assertIsNone(names)
        image, names = reference_input(gzip.compress(table([('aaaa', 8, ['_one'])]), mtime=0))
        self.assertIsNone(image)
        self.assertEqual(names.names_at(0), ('_one',))


class ReferenceNameAnchorTests(unittest.TestCase):
    def match(self, original, native):
        return Matcher(side(**original), side(**native))

    def test_a_unique_shape_names_a_function_without_a_position(self):
        matcher = self.match({'functions': {0x10: ('aaaa', 8)}}, {'functions': {0x2000: ('ffff', 8)},
                                                                  'names': {0x2000: ['_target']}})
        names = ReferenceNames.load(table([('ffff', 8, ['_unrelated']), ('aaaa', 8, ['_target'])]))
        stats = matcher.anchor_reference_names(names)
        self.assertEqual(matcher.pairs, {0x10: 0x2000})
        self.assertEqual(matcher.how[0x10], 'reference-name')
        self.assertEqual(stats['shape-bound'], 1)

    def test_a_repeated_shape_needs_the_address_order_row_and_its_shape(self):
        original = {'functions': {0x10: ('aaaa', 8), 0x20: ('aaaa', 8), 0x30: ('bbbb', 4), 0x40: ('bbbb', 4)}}
        native = {'functions': {0x2000: ('1111', 8), 0x2010: ('2222', 8), 0x2020: ('3333', 4), 0x2030: ('4444', 4)},
                  'names': {0x2000: ['_first'], 0x2010: ['_second'], 0x2020: ['_third'], 0x2030: ['_fourth']}}
        matcher = self.match(original, native)
        names = ReferenceNames.load(table([('aaaa', 8, ['_first']),
                                           ('cccc', 8, ['_second']),   # digest differs: refused
                                           ('bbbb', 4, ['_third']),
                                           ('bbbb', 4, ['_fourth'])]))
        stats = matcher.anchor_reference_names(names)
        self.assertEqual(matcher.pairs, {0x10: 0x2000, 0x30: 0x2020, 0x40: 0x2030})
        self.assertEqual(matcher.how[0x30], 'reference-name-order')
        self.assertEqual(stats['positions-digest-mismatch'], 1)

    def test_a_name_the_native_kernel_lacks_binds_nothing(self):
        matcher = self.match({'functions': {0x10: ('aaaa', 8)}},
                             {'functions': {0x2000: ('ffff', 8)}, 'names': {0x2000: ['_elsewhere']}})
        names = ReferenceNames.load(table([('aaaa', 8, ['_absent'])]))
        stats = matcher.anchor_reference_names(names)
        self.assertEqual(matcher.pairs, {})
        self.assertEqual(stats['shape-absent-in-native'], 1)

    def test_an_ambiguous_native_name_binds_nothing(self):
        native = {'functions': {0x2000: ('ffff', 8), 0x3000: ('eeee', 8)},
                  'names': {0x2000: ['_left'], 0x3000: ['_right']}}
        matcher = self.match({'functions': {0x10: ('aaaa', 8)}}, native)
        stats = matcher.anchor_reference_names(ReferenceNames.load(table([('aaaa', 8, ['_left', '_right'])])))
        self.assertEqual(matcher.pairs, {})
        self.assertEqual(stats['shape-ambiguous'], 1)


class DeclaredNameTests(unittest.TestCase):
    def test_the_entry_names_its_own_functions_and_data(self):
        original = {'functions': {0x10: ('aaaa', 8)}, 'names': {0x10: ['_function']},
                    'data_names': {0x9000: ['_variable']}}
        native = {'functions': {0x2000: ('ffff', 8)}, 'names': {0x2000: ['_function']},
                  'data_names': {0x8000: ['_variable']}}
        matcher = Matcher(side(**original), side(**native))
        declared = matcher.declared_names()
        self.assertEqual(declared, {0x10: 0x2000, 0x9000: 0x8000})
        self.assertEqual(matcher.anchor_declared_functions(declared), 1)
        known = {}
        self.assertEqual(matcher.anchor_declared_data(declared, known), 1)
        self.assertEqual(known, {0x9000: (0x8000, 'declared')})

    def test_a_name_the_native_kernel_lacks_or_duplicates_is_refused(self):
        original = {'functions': {0x10: ('aaaa', 8), 0x20: ('bbbb', 4), 0x30: ('cccc', 4)},
                    'names': {0x10: ['_absent'], 0x20: ['_twice'], 0x30: ['_one.cold.1']}}
        native = {'functions': {0x2000: ('1111', 8), 0x3000: ('2222', 4), 0x4000: ('3333', 4)},
                  'names': {0x2000: ['_other'], 0x3000: ['_twice'], 0x4000: ['_twice']}}
        matcher = Matcher(side(**original), side(**native))
        self.assertEqual(matcher.declared_names(), {})
        self.assertEqual(matcher.declared_stats['absent-in-native'], 1)   # _absent
        self.assertEqual(matcher.declared_stats['ambiguous'], 1)          # _twice, twice
        self.assertEqual(matcher.declared_stats['unusable-in-native'], 1)  # _one.cold.1


if __name__ == '__main__':
    unittest.main()
