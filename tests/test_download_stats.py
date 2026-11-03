"""Counting invariants: alias attribution, full transfers, privacy, restart safety."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('download_stats', Path(__file__).resolve().parents[1] / 'tools/download_stats.py')
stats = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stats)


def release(version, size):
    return {'version':version, 'published_at':'2026-10-24T08:00:00Z', 'assets':[
        {'name':'firmware-app-c6.bin','size':size,'downloads':3},
        {'name':f'firmware-app-c6-{version}.bin','size':size,'downloads':2},
        {'name':'firmware-app.bin','size':size,'downloads':1},
        {'name':'firmware-c6.bin','size':4194304,'downloads':4},
        {'name':'bootloader-c6.bin','size':20000,'downloads':9}]}


RELEASES = [release('1.35',1295104),release('1.34',1284000)]


def line(path='/firmware-app-c6.bin', agent='ESP32HTTPClient', size=1295104, method='GET', status=200, ref='-', tick=0):
    return f'2026-10-25T06:00:{tick:02d}.123456789Z 192.0.2.1 - - [25/Oct/2026:06:00:{tick:02d} +0000] "{method} {path} HTTP/1.1" {status} {size} "{ref}" "{agent}" "192.0.2.77"'


class DownloadStatsTests(unittest.TestCase):
    def test_s31_ota_manual_and_github_are_counted_separately_from_s3(self):
        release = {'version':'2.06','published_at':'2026-11-03T17:00:00Z','assets':[
            {'name':'firmware-app-s31.bin','size':1449264,'downloads':3},
            {'name':'firmware-app-s31-2.06.bin','size':1449264,'downloads':2},
            {'name':'firmware-s31.bin','size':16777216,'downloads':4}]}
        with stats.connect(':memory:') as db:
            stats.ingest(db,[line(path='/updates/firmware-app-s31.bin',size=1449264),
                             line(path='/firmware-app-s31-2.06.bin',size=1449264,agent='Mozilla/5.0',tick=1)], [release])
            result=stats.summarize(db,{'updated_at':'2026-11-03T17:00:00Z','available':True,'releases':[release]})
            row=result['versions'][0]
            self.assertEqual(row['boards']['s31'],dict(web_flash=0,ota=1,manual=1,github_app=5,github_full=4))
            self.assertEqual(row['total'],11)
            self.assertEqual(sum(row['boards']['s3'].values()),0)

    def test_historical_alias_uses_size_not_current_release(self):
        self.assertEqual(stats.classify(stats.parse_line(line(size=1284000)),RELEASES),('1.34','c6','ota'))

    def test_legacy_recovery_version_is_actual_payload_version(self):
        self.assertEqual(stats.classify(stats.parse_line(line(path='/firmware-app-c6-1.11.bin')),RELEASES),('1.35','c6','ota'))

    def test_types_aliases_queries_and_bridge(self):
        for path in ['/updates/firmware-app-c6.bin','/firmware-app.bin','/firmware-app-c6-bridge.bin','/firmware-app-c6-1.35.bin?x=1']:
            self.assertEqual(stats.classify(stats.parse_line(line(path=path)),RELEASES),('1.35','c6','ota'))
        browser = stats.parse_line(line(agent='Mozilla/5.0',ref='https://esp32mic.msmeteo.cz/?utm_source=test'))
        self.assertEqual(stats.classify(browser,RELEASES),('1.35','c6','web_flash'))
        browser['referer']='-'
        self.assertEqual(stats.classify(browser,RELEASES),('1.35','c6','manual'))

    def test_exclude_checks_failed_partial_and_non_firmware(self):
        for args in [dict(agent='curl/8'),dict(agent='BirdNETMic-release-verifier'),dict(agent='Googlebot'),dict(method='HEAD'),dict(status=404),dict(status=206),dict(size=1000),dict(path='/bootloader-c6.bin'),dict(path='/partitions-c6.bin'),dict(path='/ota-version.txt')]:
            self.assertIsNone(stats.classify(stats.parse_line(line(**args)), RELEASES),args)

    def test_ambiguous_historical_full_image_remains_unassigned(self):
        event=stats.parse_line(line(path='/firmware-c6.bin',size=4194304,agent='Mozilla/5.0'))
        self.assertEqual(stats.classify(event,RELEASES),('unknown','c6','manual'))
        event['release']='1.34'
        self.assertEqual(stats.classify(event,RELEASES),('1.34','c6','manual'))

    def test_new_log_attribution_and_escaping(self):
        payload={'firmware_stats':1,'time':'2026-10-25T06:00:00+00:00','method':'GET','uri':'/firmware-app-c6-1.11.bin','status':200,'bytes':1295104,'referer':'','agent':'ESP32HTTPClient','release':'1.36','file':'/usr/share/nginx/html/firmware-app-c6-bridge.bin'}
        event=stats.parse_line('2026-10-25T06:00:00.123456789Z '+json.dumps(payload,separators=(',',':')))
        self.assertEqual(stats.classify(event,RELEASES),('1.35','c6','ota'))
        self.assertIsNone(stats.parse_line('2026-10-25T06:00:00Z nginx notice'))

    def test_restart_overlap_durable_totals_and_no_client_data(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'stats.sqlite3'
            with stats.connect(path) as db:
                stats.ingest(db,[line(),line(tick=1),line()],RELEASES)
            with stats.connect(path) as db:
                stats.ingest(db,[line(),line(tick=1)],RELEASES)
                github={'updated_at':'2026-10-25T06:00:00Z','available':True,'releases':RELEASES}
                result=stats.summarize(db,github)
                self.assertEqual(result['total'],22)  # 10 GitHub firmware assets per release + 2 OTA.
                self.assertEqual(result['versions'][0]['boards']['c6']['ota'],2)
                self.assertEqual(result['versions'][0]['total'],12)
                self.assertNotIn('192.0.2',json.dumps(list(db.iterdump())))
                self.assertEqual(result['web_since'],'2026-10-25T06:00:00+00:00')

    def test_download_before_metadata_refresh_is_recovered_without_private_data(self):
        with stats.connect(':memory:') as db:
            stats.ingest(db,[line(size=1300000,agent='Mozilla/5.0 secret-agent',ref='https://esp32mic.msmeteo.cz/?private-query=secret')],RELEASES)
            self.assertEqual(db.execute('SELECT count(*) FROM downloads').fetchone()[0],0)
            dump=json.dumps(list(db.iterdump()))
            self.assertNotIn('secret',dump)
            self.assertNotIn('192.0.2',dump)
            stats.ingest(db,[],[release('1.36',1300000),*RELEASES])
            self.assertEqual(db.execute('SELECT version,kind FROM downloads').fetchone(),('1.36','web_flash'))
            self.assertEqual(db.execute('SELECT count(*) FROM pending').fetchone()[0],0)


if __name__=='__main__':
    unittest.main()
