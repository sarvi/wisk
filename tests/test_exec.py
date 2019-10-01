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


log=logging.getLogger('tests.test_open')
wisktrack.WISK_TRACKER_VERBOSITY=4
WSROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.realpath(__file__)), '../'))
LD_PRELOAD = os.path.join(WSROOT, 'src/libwisktrack.so')
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



TEMPLATE_COMMON = '''
#!{PYTHON}
import sys
from ctypes import *
LD_PRELOAD='{LD_PRELOAD}'
cdll.LoadLibrary(LD_PRELOAD)
ld_preload = CDLL(LD_PRELOAD)

def arrayofstr(L):
    arr = (c_char_p * len(L))()
    arr[:] = [(c_char_p(i) if i else i) for i in L]
    return arr

'''.format(LD_PRELOAD=LD_PRELOAD, PYTHON=sys.executable)


testcases = [
    [0, tuple(),
     TEMPLATE_COMMON+'''
sys.exit(ld_preload.execlpe(c_char_p(b'cat'), c_char_p(b'cat'), c_char_p(b'--help'), None, arrayofstr([None])))
     '''],
    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
ld_preload.execlpe(c_char_p(b'/bin/cat'), c_char_p(b'cat'), c_char_p(b'{wsroot}/tests/fixtures/testcat.data'), None, arrayofstr([None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
ld_preload.execlpe(c_char_p(b'/bin/touch'), c_char_p(b'touch'), c_char_p(b'/tmp/{testname}/file1'), None, arrayofstr([None]))
     '''],

    [0, tuple(),
     TEMPLATE_COMMON+'''
sys.exit(ld_preload.execlp(c_char_p(b'cat'), c_char_p(b'cat'), c_char_p(b'--help'), None))
     '''],
    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
ld_preload.execlp(c_char_p(b'/bin/cat'), c_char_p(b'/bin/cat'), c_char_p(b'{wsroot}/tests/fixtures/testcat.data'), None)
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
ld_preload.execlp(c_char_p(b'/bin/touch'), c_char_p(b'/bin/touch'), c_char_p(b'/tmp/{testname}/file1'), None)
     '''],

    [255, tuple(),
     TEMPLATE_COMMON+'''
sys.exit(ld_preload.execl(c_char_p(b'cat'), c_char_p(b'cat'), c_char_p(b'--help'), None))
     '''],
    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
ld_preload.execl(c_char_p(b'/bin/cat'), c_char_p(b'/bin/cat'), c_char_p(b'{wsroot}/tests/fixtures/testcat.data'), None)
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
ld_preload.execl(c_char_p(b'/bin/touch'), c_char_p(b'/bin/touch'), c_char_p(b'/tmp/{testname}/file1'), None)
     '''],

    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
ld_preload.execve(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'{wsroot}/tests/fixtures/testcat.data', None]), arrayofstr([None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
ld_preload.execve(c_char_p(b'/bin/touch'), arrayofstr([b'/bin/touch', b'/tmp/{testname}/file1', None]), arrayofstr([None]))
     '''],

    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
ld_preload.execvpe(c_char_p(b'cat'), arrayofstr([b'cat', b'{wsroot}/tests/fixtures/testcat.data', None]), arrayofstr([None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
ld_preload.execvpe(c_char_p(b'touch'), arrayofstr([b'touch', b'/tmp/{testname}/file1', None]), arrayofstr([None]))
     '''],

    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
ld_preload.execvp(c_char_p(b'cat'), arrayofstr([b'cat', b'{wsroot}/tests/fixtures/testcat.data', None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
ld_preload.execvp(c_char_p(b'touch'), arrayofstr([b'touch', b'/tmp/{testname}/file1', None]))
     '''],

    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
ld_preload.execv(c_char_p(b'/bin/cat'), arrayofstr([b'/bin/cat', b'{wsroot}/tests/fixtures/testcat.data', None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
ld_preload.execv(c_char_p(b'/bin/touch'), arrayofstr([b'/bin/touch', b'/tmp/{testname}/file1', None]))
     '''],

    [0, ('Writes /tmp/{testname}/file1',),
     '''
import os
os.system('/bin/touch /tmp/{testname}/file1')
     '''],

    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
intptr = c_int()
ld_preload.posix_spawn(intptr, c_char_p(b'/bin/cat'), None, None, arrayofstr([b'cat', b'{wsroot}/tests/fixtures/testcat.data', None]), arrayofstr([None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
intptr = c_int()
ld_preload.posix_spawn(intptr, c_char_p(b'/bin/touch'), None, None, arrayofstr([b'touch', b'/tmp/{testname}/file1', None]), arrayofstr([None]))
     '''],

    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
intptr = c_int()
ld_preload.posix_spawnp(intptr, c_char_p(b'cat'), None, None, arrayofstr([b'cat', b'{wsroot}/tests/fixtures/testcat.data', None]), arrayofstr([None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
intptr = c_int()
ld_preload.posix_spawnp(intptr, c_char_p(b'touch'), None, None, arrayofstr([b'touch', b'/tmp/{testname}/file1', None]), arrayofstr([None]))
     '''],

    [0, ('Reads {wsroot}/tests/fixtures/testcat.data',),
     TEMPLATE_COMMON+'''
intptr = c_int()
ld_preload.posix_spawnp(intptr, c_char_p(b'/bin/cat'), None, None, arrayofstr([b'cat', b'{wsroot}/tests/fixtures/testcat.data', None]), arrayofstr([None]))
     '''],
    [0, ('Writes /tmp/{testname}/file1',),
     TEMPLATE_COMMON+'''
intptr = c_int()
ld_preload.posix_spawnp(intptr, c_char_p(b'/bin/touch'), None, None, arrayofstr([b'touch', b'/tmp/{testname}/file1', None]), arrayofstr([None]))
     '''],
]

@parameterized_class(('returncode', 'tracks', 'code'), testcases)
class TestExec(unittest.TestCase):

    def setUp(self):
        if os.path.exists('/tmp/{}/'.format(self.id())):
            shutil.rmtree('/tmp/{}/'.format(self.id()))
        os.makedirs('/tmp/{}/'.format(self.id()))
        self.code = self.code.format(testname=self.id(), wsroot=WSROOT)
        self.tracks = tuple([i.format(testname=self.id(), wsroot=WSROOT) for i in self.tracks])
        self.testscript = '/tmp/{}/testscript'.format(self.id())
        open(self.testscript, 'w').write(self.code)
        

    def tearDown(self):
        if os.path.exists('/tmp/{}/'.format(self.id())):
            shutil.rmtree('/tmp/{}/'.format(self.id()))

    def test_exec(self):
#        args = argparse.Namespace(command=["/bin/cat", "%s/tests/test_open1.c" % WSROOT], verbose=3)
#         args = argparse.Namespace(command=['tests/code.py'], verbose=4)
        print(self.code)
        args = argparse.Namespace(command=[sys.executable, self.testscript], verbose=4, trackfile=None)
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