'''
Created on Sep 25, 2019

@author: sarvi
'''
import os
import sys
import stat
import shutil
import argparse
import subprocess
import unittest
import logging
from parameterized import parameterized_class
import wisktrack

log=logging.getLogger('tests.test_open')
wisktrack.WISK_TRACKER_VERBOSITY=4
WSROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.realpath(__file__)), '../'))
LD_PRELOAD = os.path.join(WSROOT, 'src/libwisktrack.so')
sys.path.insert(0, os.path.join(WSROOT, 'src/'))

TEMPLATE_EXESCRIPT='#!{PYTHON}\n'.format(PYTHON=sys.executable)

TEMPLATE_COMMON = TEMPLATE_EXESCRIPT + '''
import os
'''.format(LD_PRELOAD=LD_PRELOAD)


testcases = [
    [0, ('WRITES "/tmp/{testname}/file1"',
     'LINKS ["/tmp/{testname}/file1", "/tmp/{testname}/fileH1"]',
     'LINKS ["/tmp/{testname}/file1", "/tmp/{testname}/fileS1"]'),
     TEMPLATE_COMMON+     '''
open('/tmp/{testname}/file1', 'w').close()
os.link('/tmp/{testname}/file1', '/tmp/{testname}/fileH1')
os.symlink('/tmp/{testname}/file1', '/tmp/{testname}/fileS1')
print('Complette')
     '''],
]

@parameterized_class(('returncode', 'tracks', 'code'), testcases)
class TestLink(unittest.TestCase):

    def setUp(self):
        if os.path.exists('/tmp/{}/'.format(self.id())):
            shutil.rmtree('/tmp/{}/'.format(self.id()))
        os.makedirs('/tmp/{}/'.format(self.id()))
        self.code = self.code.format(testname=self.id(), wsroot=WSROOT)
        self.tracks = tuple([i.format(testname=self.id(), wsroot=WSROOT) for i in self.tracks])
        self.testscript = '/tmp/{}/testscript'.format(self.id())
        open(self.testscript, 'w').write(self.code)
        print(self.testscript)
        os.chmod(self.testscript, os.stat(self.testscript).st_mode | stat.S_IEXEC)


    def tearDown(self):
        if os.path.exists('/tmp/{}/'.format(self.id())):
            shutil.rmtree('/tmp/{}/'.format(self.id()))

    def test_open(self):
        print(self.code)
        args = argparse.Namespace(command=[self.testscript], verbose=4, trackfile=None)
        wisktrack.create_reciever()
        runner = wisktrack.TrackedRunner(args)
        lines = [' '.join(i.split()[1:]).strip() for i in open(wisktrack.WISK_TRACKER_PIPE).readlines()]
        print('Tracked Operations:\n %s' % ('\n\t'.join(lines)))
        print('Expected Operations:\n %s' % ('\n\t'.join(self.tracks)))
        for i in self.tracks:
            self.assertIn(i, lines)
        wisktrack.delete_reciever()
        runner.waitforcompletion()
        self.assertEqual(runner.retval.returncode, self.returncode)


if __name__ == "__main__":
    #import sys;sys.argv = ['', 'Test.testName']
    unittest.main()