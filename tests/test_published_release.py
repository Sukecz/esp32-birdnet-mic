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


if __name__ == "__main__":
    unittest.main()
