#!/usr/bin/env python3
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SOURCE=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("helper_package",SOURCE/"package.py")
package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)

class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name).resolve()
        self.stage=self.root/"stage";self.stage.mkdir()
        for name in ["README.md","Cargo.lock","vendor/libwebauthn-COPYING","vendor/LICENSE-rmweb","patches/libwebauthn-buffered-response.patch","src/main.rs"]:
            path=self.stage/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(name+"\n")
        files={str(p.relative_to(self.stage)):package.sha(p) for p in self.stage.rglob("*") if p.is_file()}
        (self.stage/"source-receipt.json").write_text(json.dumps({"libraryCommit":package.PIN,"files":files,"helperSourceSha256":"1"*64}))
        self.binary=self.root/"helper";header=bytearray(64);header[:6]=b"\x7fELF\x02\x01";header[18:20]=(183).to_bytes(2,"little");self.binary.write_bytes(header);self.binary.chmod(0o755)
        self.output=self.root/"artifacts"
    def tearDown(self):self.temp.cleanup()
    def vendor(self,*args,**kwargs):
        (self.stage/"dependencies").mkdir();(self.stage/"dependencies/LICENSE").write_text("dependency notice\n")
        return f'[source.vendored-sources]\ndirectory = "{self.stage}/dependencies"\n'
    def export(self):
        package.export(self.stage,self.binary,self.output,"sha256:"+"2"*64,"3"*40,"cargo")
    def test_complete_sources_and_exact_receipt(self):
        with patch.object(package.subprocess,"check_output",side_effect=self.vendor):self.export()
        manifest=json.loads((self.output/"manifest.json").read_text())
        self.assertEqual(manifest["binary"]["sha256"],package.sha(self.binary))
        self.assertEqual(manifest["binary"]["mode"],"0755")
        with tarfile.open(self.output/"licenses/helper-source.tar.gz") as archive:
            self.assertIn("auth-passkey-helper/src/main.rs",archive.getnames())
            self.assertIn("auth-passkey-helper/dependencies/LICENSE",archive.getnames())
            self.assertIn("auth-passkey-helper/.cargo/config.toml",archive.getnames())
        for name,record in manifest["licenses"].items():self.assertEqual(record["sha256"],package.sha(self.output/name))
    def test_source_change_fails_before_output(self):
        (self.stage/"src/main.rs").write_text("changed")
        with self.assertRaises(ValueError):self.export()
        self.assertFalse(self.output.exists())
    def test_existing_output_is_preserved(self):
        self.output.mkdir();(self.output/"unrelated").write_text("preserve")
        with self.assertRaises(ValueError):self.export()
        self.assertEqual((self.output/"unrelated").read_text(),"preserve")
    def test_wrong_architecture_fails(self):
        self.binary.write_bytes(b"not an executable")
        with self.assertRaises(ValueError):self.export()
        self.assertFalse(self.output.exists())
    def test_binary_change_during_export_fails(self):
        def mutate(*args,**kwargs):
            self.binary.write_bytes(self.binary.read_bytes()+b"changed")
            return self.vendor()
        with patch.object(package.subprocess,"check_output",side_effect=mutate),self.assertRaises(ValueError):self.export()
        self.assertFalse(self.output.exists())
    def test_reused_vendor_tree_is_rejected(self):
        (self.stage/"dependencies").mkdir()
        with self.assertRaises(ValueError):self.export()
        self.assertFalse(self.output.exists())

if __name__ == "__main__":unittest.main()
