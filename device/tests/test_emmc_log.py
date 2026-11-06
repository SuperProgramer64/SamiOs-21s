"""Recover surviving records after wrapping, mixed boots and partial writes."""
import importlib.util,pathlib,struct,zlib,unittest
path=pathlib.Path(__file__).parents[1]/'tools/emmc_log.py'
class RecoveryTests(unittest.TestCase):
 def setUp(self):
  self.assertTrue(path.exists(),'missing eMMC log recovery tool')
  spec=importlib.util.spec_from_file_location('emmc_log',path);self.m=importlib.util.module_from_spec(spec);spec.loader.exec_module(self.m)
 def record(self,seq,run,pos,data):
  b=bytearray(4096);b[:8]=b'HAKLOG01';struct.pack_into('<QQQII',b,8,seq,run,pos,len(data),0);b[64:64+len(data)]=data;struct.pack_into('<I',b,36,zlib.crc32(b));return b
 def test_latest_boot_sorted_with_lost_span_reported(self):
  records=[self.record(40,38,8,b'later\n'),self.record(37,1,0,b'old\n'),self.record(39,38,0,b'first\n')]
  text,meta=self.m.decode(b''.join(records))
  self.assertIn('first\n',text);self.assertIn('later\n',text);self.assertNotIn('old\n',text);self.assertIn('2 bytes unavailable',text)
  self.assertEqual(meta['run'],38);self.assertEqual(meta['records'],2)
 def test_torn_record_ignored(self):
  torn=self.record(2,1,5,b'bad\n');torn[90]^=1
  text,meta=self.m.decode(self.record(1,1,0,b'good\n')+torn)
  self.assertEqual(text,'good\n');self.assertEqual(meta['invalid'],1)
 def test_empty_region_and_bad_length_rejected(self):
  with self.assertRaisesRegex(ValueError,'no valid'):self.m.decode(bytes(8192))
  with self.assertRaisesRegex(ValueError,'4096'):self.m.decode(bytes(4095))
 def test_payload_reserves_log_tail(self):
  import sys,tempfile
  sys.path.insert(0,str(path.parent));import xnusys
  self.assertTrue(callable(getattr(xnusys,'payload_room',None)),'missing protected payload room')
  with tempfile.NamedTemporaryFile() as f:
   f.truncate(60*1024*1024+512)
   with self.assertRaises(SystemExit):xnusys.image_info(f.name,{},xnusys.payload_room())
 def test_plan(self):
  self.assertEqual(self.m.log_layout({'super_start':737280,'first':11479040,'sectors':8075264}),{'lba':20283392,'sectors':8192})
if __name__=='__main__':unittest.main()
