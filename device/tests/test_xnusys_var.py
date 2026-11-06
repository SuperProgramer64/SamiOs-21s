"""Catch overlap with the root image, payload and Android when planning var."""
import importlib.util
import pathlib
import struct
import unittest

spec = importlib.util.spec_from_file_location('xnusys', pathlib.Path(__file__).parents[1] / 'tools/xnusys.py')
x = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x)

class VarLayoutTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(callable(getattr(x, "var_layout", None)), "missing validated var layout")
        self.region = dict(super_start=737280, first=11479040, sectors=8075264)
        self.header = bytearray(4096)
        self.header[32:36] = b'NXSB'
        struct.pack_into('<IQ', self.header, 36, 4096, 606033)

    def test_real_image_leaves_disjoint_aligned_var_and_payload(self):
        got = x.var_layout(self.region, self.header)
        self.assertEqual(got['root_sectors'], 4848264)
        self.assertEqual(got['lba'], 17068032)
        self.assertEqual(got['sectors'], 3092480)
        self.assertEqual(got['payload_lba'], 20160512)

    def test_unrecognized_root_is_refused(self):
        self.header[32:36] = b'NOPE'
        with self.assertRaisesRegex(ValueError, 'APFS'):
            x.var_layout(self.region, self.header)

    def test_oversized_root_is_refused(self):
        struct.pack_into('<Q', self.header, 40, 2000000)
        with self.assertRaisesRegex(ValueError, 'room'):
            x.var_layout(self.region, self.header)

    def test_overflowing_block_count_is_refused(self):
        struct.pack_into('<Q', self.header, 40, 2**64 - 1)
        with self.assertRaises(ValueError):
            x.var_layout(self.region, self.header)

    def test_invalid_apfs_geometry_is_refused(self):
        struct.pack_into('<I', self.header, 36, 1000)
        with self.assertRaises(ValueError):
            x.var_layout(self.region, self.header)


class HfsImageTests(unittest.TestCase):
    def test_rejects_nonjournaled_and_wrong_sized_images(self):
        import tempfile
        self.assertTrue(callable(getattr(x, 'hfs_image_check', None)), 'missing HFS image validation')
        with tempfile.NamedTemporaryFile() as f:
            f.truncate(64 * 1024 * 1024)
            h = bytearray(512)
            h[:2] = b'H+'
            struct.pack_into('>II', h, 4, 1 << 13, 0)
            struct.pack_into('>I', h, 12, 8)
            struct.pack_into('>II', h, 40, 4096, 16384)
            f.seek(1024); f.write(h); f.flush()
            x.hfs_image_check(f.name, 131072)
            with self.assertRaisesRegex(ValueError, 'size'):
                x.hfs_image_check(f.name, 133120)
            struct.pack_into('>I', h, 4, 0)
            f.seek(1024); f.write(h); f.flush()
            with self.assertRaisesRegex(ValueError, 'journal'):
                x.hfs_image_check(f.name, 131072)


class VarWriteSafetyTests(unittest.TestCase):
    def test_existing_hfs_is_never_reformatted(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        a = SimpleNamespace(serial='test-device', cmd='var-write', image='unused', i_understand=True)
        r = dict(super_start=737280, first=11479040, sectors=8075264)
        h = bytearray(4096); h[32:36] = b'NXSB'; struct.pack_into('<IQ', h, 36, 4096, 606033)
        writes = []
        def transport(serial, cmd, binary=False):
            if 'count=8 ' in cmd:
                return h
            if 'count=1 ' in cmd:
                return b'H+' + bytes(510)
            writes.append(cmd)
            raise AssertionError('unexpected storage mutation: ' + cmd)
        with patch.object(x, 'sh', side_effect=transport), patch.object(x, 'hfs_image_check'), patch.object(x, 'image_info', return_value=(1583349760, 'unused')):
            with self.assertRaisesRegex(SystemExit, 'already exists'):
                x.var_command(a, r)
        self.assertEqual(writes, [])

    def test_layout_change_prevents_first_write(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        a = SimpleNamespace(serial='test-device', cmd='var-write', image='unused', i_understand=True)
        r = dict(super_start=737280, first=11479040, sectors=8075264)
        h = bytearray(4096); h[32:36] = b'NXSB'; struct.pack_into('<IQ', h, 36, 4096, 606033)
        writes = []
        def transport(serial, cmd, binary=False):
            if 'count=8 ' in cmd:
                return h
            if 'count=1 ' in cmd:
                return bytes(512)
            writes.append(cmd)
            raise AssertionError('unexpected storage mutation: ' + cmd)
        with patch.object(x, 'sh', side_effect=transport), patch.object(x, 'hfs_image_check'), patch.object(x, 'image_info', return_value=(1583349760, 'unused')), patch.object(x, 'adb'), patch.object(x, 'region', return_value=dict(r, first=11481088)):
            with self.assertRaisesRegex(SystemExit, 'changed'):
                x.var_command(a, r)
        self.assertEqual(writes, [])



class RootUpdateSafetyTests(unittest.TestCase):
    def test_existing_var_prevents_relocating_it_by_changing_root_size(self):
        from unittest.mock import patch
        self.assertTrue(callable(getattr(x, 'protect_root_update', None)), 'missing persistent-volume update guard')
        r = dict(super_start=737280, first=11479040, sectors=8075264)
        h = bytearray(4096); h[32:36] = b'NXSB'; struct.pack_into('<IQ', h, 36, 4096, 606033)
        def transport(serial, cmd, binary=False):
            return h if 'count=8 ' in cmd else b'H+' + bytes(510)
        with patch.object(x, 'sh', side_effect=transport):
            x.protect_root_update('test-device', r, 2482311168)
            with self.assertRaisesRegex(SystemExit, 'size'):
                x.protect_root_update('test-device', r, 2483359744)



class DamagedPrimaryHeaderTests(unittest.TestCase):
    def test_backup_hfs_header_prevents_accidental_reformat(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        a = SimpleNamespace(serial='test-device', cmd='var-write', image='unused', i_understand=True)
        r = dict(super_start=737280, first=11479040, sectors=8075264)
        h = bytearray(4096); h[32:36] = b'NXSB'; struct.pack_into('<IQ', h, 36, 4096, 606033)
        def transport(serial, cmd, binary=False):
            if 'count=8 ' in cmd:
                return h
            if 'skip=16330754 ' in cmd:  # primary header: lba 17068032 - super 737280 + 2
                return bytes(512)
            if 'count=1 ' in cmd:
                return b'H+' + bytes(510)
            raise AssertionError('must not write with a recoverable data volume: ' + cmd)
        with patch.object(x, 'sh', side_effect=transport), patch.object(x, 'hfs_image_check'), patch.object(x, 'image_info', return_value=(1583349760, 'unused')), patch.object(x, 'adb'), patch.object(x, 'region', return_value=r):
            with self.assertRaisesRegex(SystemExit, 'already exists'):
                x.var_command(a, r)

if __name__ == '__main__':
    unittest.main()
