import sys
sys.path.insert(0,str(__import__("pathlib").Path(__file__).resolve().parents[1]/"tools"))
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("release", Path(__file__).resolve().parents[1] / "tools/verify_published_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseVerificationTest(unittest.TestCase):
    def setUp(self):
        self.files = {name: b"firmware" for name in release.asset_names("1.23")}
        self.files["ota-version.txt"] = b"1.23\n"
        self.files["manifest.json"] = json.dumps({"version": "1.23", "builds": [
            {"chipFamily": "ESP32-" + board.upper(), "parts": [{"path": f"firmware-app-{board}-1.23.bin", "offset": 65536}]}
            for board in ("c3", "s3", "c5", "c6")]}).encode()
        self.metadata = {"tag_name": "v1.23", "draft": False, "prerelease": False, "assets": [
            {"name": name, "digest": "sha256:" + hashlib.sha256(data).hexdigest(), "size": len(data),
             "browser_download_url": "https://download/" + name} for name, data in self.files.items()]}

    def check(self):
        def read(url):
            if url == "http://public/": return b'Install BirdNETMic from your browser. <span id="fw-version-chip"></span>'
            if "api.github.com" in url:
                return json.dumps(self.metadata).encode()
            return self.files[url.rsplit("/", 1)[1]]
        with patch.object(release, "read", side_effect=read):
            release.verify("example/repo", "1.23", "http://public")

    def test_matching_release(self):
        self.check()

    def test_stale_public_feed(self):
        self.files["ota-version.txt"] = b"1.22\n"
        with self.assertRaisesRegex(ValueError, "mismatch: ota-version"):
            self.check()

    def test_one_wrong_board(self):
        self.files["firmware-app-s3.bin"] = b"wrong board"
        with self.assertRaisesRegex(ValueError, "mismatch: firmware-app-s3"):
            self.check()

    def test_missing_asset(self):
        self.metadata["assets"].pop()
        with self.assertRaisesRegex(ValueError, "Missing GitHub assets"):
            self.check()

    def test_draft_rejected(self):
        self.metadata["draft"] = True
        with self.assertRaisesRegex(ValueError, "published stable"):
            self.check()

    def test_older_github_without_digest(self):
        for asset in self.metadata["assets"]:
            del asset["digest"]
        self.check()

class BridgedReleaseVerificationTest(unittest.TestCase):
    def check(self, corrupt_legacy=False):
        version='1.40'
        files={name:b'part' for name in release.asset_names(version)}
        files['ota-version.txt']=b'1.40\n'
        bridge={'version':'1.35','boards':{}}
        for board in ('c3','s3','c5','c6'):
            files[f'firmware-app-{board}.bin']=('latest-'+board).encode()
            files[f'firmware-app-{board}-{version}.bin']=files[f'firmware-app-{board}.bin']
            image=('bridge-'+board).encode()
            files[f'firmware-app-{board}-bridge.bin']=image
            bridge['boards'][board]={'size':len(image),'sha256':hashlib.sha256(image).hexdigest()}
        files['firmware-app.bin']=files['firmware-app-c6.bin']
        files['ota-bridge-version.txt']=b'1.35\n'
        files['ota-bridge.json']=json.dumps(bridge).encode()
        files['manifest.json']=json.dumps({'version':version,'builds':[
            {'chipFamily':'ESP32-'+board.upper(),'parts':[{'path':f'firmware-app-{board}-{version}.bin','offset':65536}]}
            for board in ('c3','s3','c5','c6')]}).encode()
        metadata={'tag_name':'v'+version,'draft':False,'prerelease':False,'assets':[
            {'name':name,'size':len(data),'digest':'sha256:'+hashlib.sha256(data).hexdigest()}
            for name,data in files.items()]}
        def read(url):
            if url == "http://public/": return b'Install BirdNETMic from your browser. <span id="fw-version-chip"></span>'
            if 'api.github.com' in url: return json.dumps(metadata).encode()
            path=url.removeprefix('http://public/')
            if path.startswith('updates/'): return files[path.removeprefix('updates/')]
            if path=='ota-version.txt': return files['ota-bridge-version.txt']
            if path=='firmware-app.bin': return files['firmware-app-c6-bridge.bin']
            for board in ('c3','s3','c5','c6'):
                if path==f'firmware-app-{board}.bin':
                    return files[f'firmware-app-{board}.bin' if corrupt_legacy else f'firmware-app-{board}-bridge.bin']
            return files[path]
        with patch.object(release,'read',side_effect=read):
            release.verify('example/repo',version,'http://public')

    def test_current_release_and_permanent_bridge_both_verified(self): self.check()
    def test_rejects_old_clients_receiving_the_large_current_image(self):
        with self.assertRaisesRegex(ValueError,'Legacy URL skipped the bridge'): self.check(True)


if __name__ == "__main__":
    unittest.main()
