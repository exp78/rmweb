#!/usr/bin/env python3
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

spec=importlib.util.spec_from_file_location('auth_recipe',Path(__file__).resolve().parents[1]/'scripts/build-auth-browser.py')
recipe=importlib.util.module_from_spec(spec);spec.loader.exec_module(recipe)

class RecipeTest(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name).resolve()
        self.repo=self.root/'repo';(self.repo/'engine/wpeqt').mkdir(parents=True)
        for name in ['CMakeLists.txt','auth-entry.cpp','qtfbclient.cpp','qtfbclient.h','auth-main.cpp']:
            (self.repo/'engine/wpeqt'/name).write_text(name)
        (self.repo/'scripts').mkdir();(self.repo/'scripts/build-auth-browser.py').write_text('recipe')
        (self.repo/'scripts/build-auth-engine.py').write_text('engine recipe')
        (self.repo/'device/auth').mkdir(parents=True);(self.repo/'device/auth/entry').write_text('launcher')
        (self.repo/'patches').mkdir();(self.repo/'patches/wpe-2.48.5-native-assertion-provider.patch').write_text('licenses/provider.patch')
        self.patch=self.repo/'engine/auth-passkey-helper/patches/libwebauthn-buffered-response.patch'
        self.patch.parent.mkdir(parents=True);self.patch.write_text('reviewed patch')
        self.engine=self.root/'engine';self.helper=self.root/'helper';self.cache=self.root/'cache'
        (self.engine/'runtime').mkdir(parents=True);(self.engine/'devel').mkdir()
        (self.helper/'licenses').mkdir(parents=True)
        self.repo_patch=patch.object(recipe,'REPOSITORY',self.repo);self.repo_patch.start()
        self.runtime={}
        for name in sorted(recipe.REQUIRED_RUNTIME|{'licenses/provider.patch'}):
            self.runtime[name]=self.file(self.engine/'runtime'/name,recipe.RUNTIME_ENVIRONMENT.encode() if name == 'rmweb-env.sh' else name.encode(),0o755 if name.startswith('libexec/') else 0o644)
        self.devel={'include/wpe/header.h':self.file(self.engine/'devel/include/wpe/header.h',b'header',0o644)}
        self.e={'schemaVersion':1,'purpose':'auth-engine','firmware':'3.28.0.172',
            'sdk':{'sha256':recipe.SDK_SHA,'qtVersion':'6.10.3'},
            'upstream':{'version':'2.48.5','sourceSha256':recipe.WPE_SHA,'runtimeSha256':recipe.RUNTIME_SHA},
            'patch':{'path':'runtime/licenses/provider.patch','sha256':self.runtime['licenses/provider.patch']['sha256']},
            'runtimeFiles':self.runtime,'develFiles':self.devel}
        self.elf(self.helper/'rmweb-auth-passkey')
        license_record=self.file(self.helper/'licenses/NOTICE.txt',b'license',0o644);license_record['mode']='0644'
        self.h={'schemaVersion':1,'SDKsha256':recipe.SDK_SHA,
            'sourceRevision':{'libwebauthn':recipe.HELPER_REVISION,'helperSourceSha256':recipe.helper_source_hash()},
            'patchSHA256':recipe.sha(self.patch),'binary':{'path':'rmweb-auth-passkey',**recipe.executable(self.helper/'rmweb-auth-passkey')},
            'licenses':{'licenses/NOTICE.txt':license_record}}
        self.save()
    def tearDown(self):self.repo_patch.stop();self.temp.cleanup()
    def file(self,path,data,mode):
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);path.chmod(mode)
        return {'sha256':recipe.sha(path),'bytes':len(data),'mode':mode}
    def elf(self,path):
        data=bytearray(64);data[:6]=b'\x7fELF\x02\x01';data[18:20]=(183).to_bytes(2,'little');self.file(path,data,0o755)
    def save(self):
        (self.engine/'manifest.json').write_text(json.dumps(self.e));(self.helper/'manifest.json').write_text(json.dumps(self.h))
    def image(self,*args,**kwargs):return json.dumps([{'Id':'sha256:'+'1'*64,'Architecture':'arm64','Os':'linux'}]).encode()
    def compile(self,cache,engine,image):
        (cache/'build-auth').mkdir()
        for name in recipe.TARGETS:self.elf(cache/'build-auth'/name)
    def build(self,compile=None):
        with patch.object(recipe.subprocess,'check_output',side_effect=self.image),patch.object(recipe,'build_cpp',side_effect=compile or self.compile):
            return recipe.build(self.cache,self.engine,self.helper,'sdk')
    def test_combines_only_auth_targets_and_exact_payload(self):
        output=self.build();m=json.loads((output/'manifest.json').read_text())
        self.assertEqual(set(m['artifacts']),{'rmweb-auth-browser','rmweb-auth-entry','rmweb-auth-passkey'})
        self.assertEqual(m['schemaVersion'],2);self.assertEqual(m['helper']['files']['NOTICE.txt']['mode'],0o644)
        self.assertTrue((self.cache/'stage/usr/include/wpe/header.h').is_file())
        self.assertFalse((output/'rmweb-wpeqt').exists());recipe.validate_published(output)
    def test_input_hash_mismatch_does_not_claim_cache(self):
        (self.engine/'runtime/rmweb-env.sh').write_text('changed')
        with self.assertRaisesRegex(ValueError,'differs'):self.build()
        self.assertFalse(self.cache.exists())
    def test_legacy_runtime_contract_is_rejected_even_with_matching_manifest(self):
        self.runtime['rmweb-env.sh']=self.file(self.engine/'runtime/rmweb-env.sh',b'export LD_LIBRARY_PATH=/old-app/runtime/lib\n',0o644)
        self.save()
        with self.assertRaisesRegex(ValueError,'launch contract'):self.build()
        self.assertFalse(self.cache.exists())
    def test_launcher_change_during_build_preserves_previous_artifacts(self):
        output=self.build();before=(output/'manifest.json').read_bytes()
        def mutate(cache,engine,image):
            self.compile(cache,engine,image);(self.repo/'device/auth/entry').write_text('changed launcher')
        with self.assertRaisesRegex(ValueError,'sources changed'):self.build(mutate)
        self.assertEqual((output/'manifest.json').read_bytes(),before)
    def test_input_provenance_mismatch_does_not_claim_cache(self):
        self.e['upstream']['sourceSha256']='0'*64;self.save()
        with self.assertRaisesRegex(ValueError,'provenance'):self.build()
        self.assertFalse(self.cache.exists())
    def test_stale_engine_patch_is_rejected(self):
        (self.repo/'patches/wpe-2.48.5-native-assertion-provider.patch').write_text('fixed provider')
        with self.assertRaisesRegex(ValueError,'current checkout'):self.build()
        self.assertFalse(self.cache.exists())
    def test_helper_source_change_is_rejected(self):
        self.patch.write_text('unbuilt edit')
        with self.assertRaisesRegex(ValueError,'source'):self.build()
        self.assertFalse(self.cache.exists())
    def test_source_change_during_build_preserves_previous_artifacts(self):
        output=self.build();before=(output/'manifest.json').read_bytes()
        def mutate(cache,engine,image):
            self.compile(cache,engine,image);(self.repo/'engine/wpeqt/auth-main.cpp').write_text('new edit')
        with self.assertRaisesRegex(ValueError,'sources changed'):self.build(mutate)
        self.assertEqual((output/'manifest.json').read_bytes(),before)
    def test_unrelated_unmarked_cache_is_preserved(self):
        self.cache.mkdir(mode=0o700);sentinel=self.cache/'unrelated';sentinel.write_text('preserve')
        with self.assertRaisesRegex(ValueError,'unmarked'):self.build()
        self.assertEqual(sentinel.read_text(),'preserve');self.assertFalse((self.cache/recipe.MARKER).exists())
    def test_symlink_cache_write_tree_is_rejected(self):
        recipe.claim_cache(self.cache);other=self.root/'other';other.mkdir();(other/'sentinel').write_text('keep')
        (self.cache/'stage').symlink_to(other)
        with self.assertRaisesRegex(ValueError,'redirected'):self.build()
        self.assertEqual((other/'sentinel').read_text(),'keep')
    def test_unowned_file_in_previous_artifacts_is_preserved(self):
        output=self.build();(output/'unrelated').write_text('preserve')
        with self.assertRaisesRegex(ValueError,'unowned'):self.build()
        self.assertEqual((output/'unrelated').read_text(),'preserve')
    def test_input_mutation_during_build_does_not_publish(self):
        def mutate(cache,engine,image):
            self.compile(cache,engine,image);(self.helper/'licenses/NOTICE.txt').write_text('changed')
        with self.assertRaisesRegex(ValueError,'differs'):self.build(mutate)
        self.assertFalse((self.cache/'artifacts').exists())
    def test_runtime_symlinks_are_not_followed(self):
        target=self.engine/'runtime/rmweb-env.sh';target.unlink();target.symlink_to(self.patch)
        with self.assertRaisesRegex(ValueError,'redirected'):self.build()
        self.assertFalse(self.cache.exists())
    def test_cache_inside_checkout_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'outside Git'):recipe.external(self.repo/'build')
    def test_cache_can_rebuild_after_supported_smoke_tests(self):
        self.build()
        for name in ('build-wpe-smoke','build-wpe-provider','build-passkey-browser'):
            directory=self.cache/name;directory.mkdir();(directory/'test-result').write_text('preserve')
        output=self.build();recipe.validate_published(output)
        for name in ('build-wpe-smoke','build-wpe-provider','build-passkey-browser'):
            self.assertEqual((self.cache/name/'test-result').read_text(),'preserve')
    def test_unrecognized_smoke_directory_is_not_adopted(self):
        self.build();directory=self.cache/'build-unrelated';directory.mkdir();(directory/'sentinel').write_text('keep')
        with self.assertRaisesRegex(ValueError,'unexpected file'):self.build()
        self.assertEqual((directory/'sentinel').read_text(),'keep')

if __name__=='__main__':unittest.main()
