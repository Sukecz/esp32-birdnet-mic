"""A skipped-release update must still traverse the small, permanent bridge."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('bridge',ROOT/'tools/package_ota_bridge.py')
bridge=importlib.util.module_from_spec(spec);spec.loader.exec_module(bridge)


class BridgePackagingTests(unittest.TestCase):
    def test_bridge_frozen_for_skipped_versions_and_even_same_version_rebuilds(self):
        with tempfile.TemporaryDirectory() as temp:
            web=Path(temp)
            for board in bridge.BOARDS:
                (web/f'firmware-app-{board}.bin').write_bytes(('bridge-'+board).encode())
            bridge.package(web,'1.35',False)
            frozen={board:(web/f'firmware-app-{board}-bridge.bin').read_bytes() for board in bridge.BOARDS}
            for board in bridge.BOARDS:
                (web/f'firmware-app-{board}.bin').write_bytes(b'new firmware'+b'x'*1310720)
            bridge.package(web,'1.40',False)
            bridge.package(web,'1.35',True)
            for board in bridge.BOARDS:
                self.assertEqual((web/f'firmware-app-{board}-bridge.bin').read_bytes(),frozen[board])
            (web/'firmware-app-c6-bridge.bin').write_bytes(b'changed')
            with self.assertRaises(ValueError): bridge.package(web,'1.40',False)

    def test_oversized_bridge_is_never_packaged(self):
        with tempfile.TemporaryDirectory() as temp:
            web=Path(temp)
            (web/'firmware-app-c3.bin').write_bytes(b'x'*1310721)
            with self.assertRaises(ValueError): bridge.package(web,'1.35',False)


class RealNginxBridgeTests(unittest.TestCase):
    def test_old_client_1_34_gets_1_35_before_current_1_40(self):
        binary=os.environ.get('NGINX_TEST_BIN') or shutil.which('nginx')
        if not binary: self.skipTest('nginx binary required for actual routing test')
        with tempfile.TemporaryDirectory() as temp:
            base=Path(temp);web=base/'web';web.mkdir();(base/'logs').mkdir()
            (web/'ota-version.txt').write_bytes(b'1.40\n')
            (web/'ota-bridge-version.txt').write_bytes(b'1.35\n')
            (web/'ota-bridge.json').write_text('{}')
            for board in bridge.BOARDS:
                (web/f'firmware-app-{board}.bin').write_bytes(('new-'+board).encode()+b'x'*1310720)
                (web/f'firmware-app-{board}-bridge.bin').write_bytes(('bridge-'+board).encode())
                (web/f'firmware-app-{board}-1.40.bin').write_bytes((web/f'firmware-app-{board}.bin').read_bytes())
            (web/'firmware-app-s31.bin').write_bytes(b'new-s31')
            with socket.socket() as sock:
                sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            server=(ROOT/'deployment/esp32mic-web-default.conf').read_text()
            server=server.replace('listen 80;',f'listen 127.0.0.1:{port};').replace('/usr/share/nginx/html',str(web))
            server=server.replace('include /etc/nginx/mime.types;','types { application/octet-stream bin; application/json json; text/plain txt; }')
            server=server.replace('/var/log/nginx/access.log',str(base/'logs/access.log'))
            server=server.replace('__FIRMWARE_VERSION__','1.40')
            config=base/'nginx.conf'
            config.write_text(f'daemon off; master_process off; pid {base}/nginx.pid; error_log stderr; events {{}} http {{ access_log off; client_body_temp_path {base}/body; proxy_temp_path {base}/proxy; fastcgi_temp_path {base}/fastcgi; uwsgi_temp_path {base}/uwsgi; scgi_temp_path {base}/scgi;\n'+server+'\n}')
            command=[str(binary),'-p',str(base),'-c',str(config)]
            subprocess.run(command+['-t'],check=True,capture_output=True)
            process=subprocess.Popen(command,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            try:
                url=f'http://127.0.0.1:{port}'
                for attempt in range(50):
                    try:
                        with urllib.request.urlopen(url+'/ota-version.txt',timeout=1) as response:
                            self.assertEqual(response.read(),b'1.35\n')
                        break
                    except OSError:
                        if process.poll() is not None: self.fail(process.stderr.read().decode())
                        time.sleep(.05)
                else: self.fail('nginx did not start')
                with urllib.request.urlopen(url+'/updates/ota-version.txt',timeout=2) as r:
                    self.assertEqual(r.read(),b'1.40\n')
                for board in bridge.BOARDS:
                    for path in [f'/firmware-app-{board}.bin',*[f'/firmware-app-{board}-{old}.bin' for old in ('1.10.0','1.10.1','1.11')]]:
                        with urllib.request.urlopen(url+path,timeout=2) as r:
                            self.assertEqual(r.status,200)
                            self.assertEqual(r.read(),('bridge-'+board).encode())
                            self.assertIsNone(r.headers.get('Location'))
                    with urllib.request.urlopen(url+f'/updates/firmware-app-{board}.bin',timeout=2) as r:
                        self.assertEqual(r.headers.get_content_type(),'application/octet-stream')
                        self.assertGreater(int(r.headers['Content-Length']),1310720)
                        self.assertEqual(r.read(),(web/f'firmware-app-{board}.bin').read_bytes())
                with urllib.request.urlopen(url+'/firmware-app.bin',timeout=2) as r:
                    self.assertEqual(r.read(),b'bridge-c6')
                self.assertEqual(urllib.request.urlopen(url+'/updates/firmware-app-s31.bin').read(),b'new-s31')
                self.assertEqual(urllib.request.urlopen(url+'/firmware-app-s31.bin').read(),b'new-s31')
                records=[json.loads(line) for line in (base/'logs/access.log').read_text().splitlines()]
                self.assertTrue(all(record['release']=='1.40' for record in records))
                self.assertTrue(any(record['file'].endswith('firmware-app-c6-bridge.bin') for record in records))
                self.assertTrue(any(record['file'].endswith('firmware-app-c6.bin') for record in records))
            finally:
                process.terminate()
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired: process.kill();process.wait()
                process.stderr.close()
