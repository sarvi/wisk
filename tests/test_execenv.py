'''
Created on Sep 25, 2019

@author: sarvi
'''
import os
import sys
import stat
import argparse
import subprocess
import shutil
import unittest
import logging
from parameterized import parameterized_class
import wisktrack
from testrunner import TrackedRunner


log=logging.getLogger('tests.test_open')
wisktrack.WISK_TRACKER_VERBOSITY=4
WSROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.realpath(__file__)), '../'))
LD_PRELOAD = os.path.join(WSROOT, 'src/lib64/libwisktrack.so')
sys.path.insert(0, os.path.join(WSROOT, 'src/'))

if False:
    from ctypes import *
    cdll.LoadLibrary(LD_PRELOAD)
    ld_preload = CDLL(LD_PRELOAD)
     
    def arrayofstr(L):
        arr = (c_char_p * len(L))()
        arr[:] = [(c_char_p(i) if i else i) for i in L]
        return arr
     
    ld_preload.execvp(c_char_p(b'/bin/touch'), arrayofstr([b'/bin/touch', b'/tmp/file1', None]))


TEMPLATE_EXESCRIPT='#!{PYTHON}\n'.format(PYTHON=sys.executable)

TEMPLATE_COMMON = TEMPLATE_EXESCRIPT + '''
import os
import sys
from ctypes import *
LD_PRELOAD='{LD_PRELOAD}'
cdll.LoadLibrary(LD_PRELOAD)
ld_preload = CDLL(LD_PRELOAD)

def arrayofstr(L):
    arr = (c_char_p * len(L))()
    arr[:] = [(c_char_p(i) if i else i) for i in L]
    return arr

'''.format(LD_PRELOAD=LD_PRELOAD)


testcases = [
    [0, ('"LARGEVAR=", "ZLASTVAR=END"', ),
     TEMPLATE_COMMON+'''
os.environ.clear()
os.environ["LARGEVAR"] = ''
os.environ["ZLASTVAR"] = "END"

sys.exit(ld_preload.execv(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'--help', None])))
     '''],
    [0, ('"LARGEVAR=1", "ZLASTVAR=END"', ),
     TEMPLATE_COMMON+'''
os.environ.clear()
os.environ["LARGEVAR"] = '1'
os.environ["ZLASTVAR"] = "END"

sys.exit(ld_preload.execv(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'--help', None])))
     '''],
    [0, ('11111X', 
         'ENVIRONMENT *", "ZLASTVAR=END"'),
     TEMPLATE_COMMON+'''
os.environ.clear()
os.environ["LARGEVAR"] = '1'*3775+'X'
os.environ["ZLASTVAR"] = "END"

sys.exit(ld_preload.execv(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'--help', None])))
     '''],
    [0, ('"LARGEVAR=111111111',
         '11111X',
         '*1", "ZLASTVAR=END"'),
     TEMPLATE_COMMON+'''
os.environ.clear()
os.environ["LARGEVAR"] = '1'*3775+'X'+'1'
os.environ["ZLASTVAR"] = "END"

sys.exit(ld_preload.execv(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'--help', None])))
     '''],
    [0, ('"LARGEVAR=111111111',
         'ENVIRONMENT *111111111111111111',
         'ENVIRONMENT *1", "ZLASTVAR=END"',
         ),
     TEMPLATE_COMMON+'''
os.environ.clear()
os.environ["LARGEVAR"] = '1'*(3775 + 4039)
os.environ["ZLASTVAR"] = "END"

sys.exit(ld_preload.execv(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'--help', None])))
     '''],
    [0, ('"QUOTEDVALUE=something \\"quoted value\\" "',
         '"ZLASTVAR=END"',
         ),
     TEMPLATE_COMMON+'''
os.environ.clear()
os.environ["QUOTEDVALUE"] = 'something "quoted value" '
os.environ["ZLASTVAR"] = "END"

sys.exit(ld_preload.execv(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'--help', None])))
     '''],
]

@parameterized_class(('returncode', 'tracks', 'code'), testcases)
class TestExecEnv(unittest.TestCase):

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

    def test_exec(self):
        print(self.code)
        args = argparse.Namespace(command=[self.testscript], verbose=4, trackfile=None)
        wisktrack.create_reciever()
        runner = TrackedRunner(args)
        lines = [' '.join(i.split()[1:]).strip() for i in open(wisktrack.WISK_TRACKER_PIPE).readlines()]
        print('Tracked Operations:\n\t%s' % ('\n\t'.join(lines)))
        print('Expected Operations:\n\t%s' % ('\n\t'.join(self.tracks)))
        for i in self.tracks:
            print('Asserting: %s' % i)
            self.assertTrue([j for j in lines if i in j])
        wisktrack.delete_reciever(runner)
        # runner.waitforcompletion()
        self.assertEqual(runner.retval.returncode, self.returncode)


if __name__ == "__main__":
    #import sys;sys.argv = ['', 'Test.testName']
    unittest.main()
