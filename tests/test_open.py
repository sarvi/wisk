'''
Created on Sep 25, 2019

@author: sarvi
'''
import os
import sys
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

testcases = [
    [('Writes /tmp/file1',
     'Writes /tmp/file2',
     'Reads /tmp/file1',
     'Reads /tmp/file2'),
     '''
open('/tmp/file1', 'w').close()
open('/tmp/file2', 'w').close()
open('/tmp/file1', 'r').close()
open('/tmp/file2', 'r').close()
print('Complette')
     '''],
]

@parameterized_class(('tracks', 'code'), testcases)
class TestOpen(unittest.TestCase):

    def setUp(self):
        pass


    def test_open1(self):
#        args = argparse.Namespace(command=["/bin/cat", "%s/tests/test_open1.c" % WSROOT], verbose=3)
        args = argparse.Namespace(command=['tests/code.py'], verbose=4)
#        args = argparse.Namespace(command=[sys.executable, '-c', self.code], verbose=4)
        wisktrack.create_reciever()
        print(wisktrack.TrackedRunner(args))
        lines = [' '.join(i.split()[1:]).strip() for i in open(wisktrack.WISK_TRACKER_PIPE).readlines()]
        print('Tracked Operations:\n %s' % ('\n\t'.join(lines)))
        print('Expectred Operations:\n %s' % ('\n\t'.join(self.tracks)))
        for i in self.tracks:
            self.assertIn(i, lines)
        wisktrack.delete_reciever()


if __name__ == "__main__":
    #import sys;sys.argv = ['', 'Test.testName']
    unittest.main()