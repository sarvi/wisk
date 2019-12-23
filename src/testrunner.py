import logging
import os
import subprocess
import threading
from common import env

log = logging.getLogger(__name__)  # pylint: disable=locally-disabled, invalid-name

WISK_TRACKER_PIPE='/tmp/wisk_tracker.pipe'
WISK_TRACKER_UUID='XXXXXXXX-XXXXXXXX-XXXXXXXX'


class TrackedRunner(object):
    def __init__(self, args):
        self.args = args
        self.retval = None
        self.wisk_verbosity = args.verbose + 1
        log.info('Verbosity: %d', self.wisk_verbosity)
        self.cmdenv = dict(os.environ)
        self.cmdenv.update({
            'LD_LIBRARY_PATH': ':'.join(['', os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib32'),
                                     os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib64')]),
#            'LD_LIBRARY_PATH': ':'.join([os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib32')]),
            'LD_PRELOAD': 'libwisktrack.so',
            'WISK_TRACKER_PIPE_FD': '-1',
            'WISK_TRACKER_DEBUGLOG': "%s.log" % args.test_id,
            'WISK_TRACKER_DEBUGLOG_FD': "-1",
            'WISK_TRACKER_PIPE': WISK_TRACKER_PIPE,
            'WISK_TRACKER_UUID': WISK_TRACKER_UUID,
            'WISK_TRACKER_DEBUGLEVEL': ('%d' % (self.wisk_verbosity))})
        open(self.cmdenv['WISK_TRACKER_DEBUGLOG'], 'w').close()
        print(self.cmdenv)
        print(self.args.command)
        self.thread = threading.Thread(target=self.run, args=())
        self.thread.daemon = True
        self.thread.start()
        print('Finished')
        return

    def run(self):
        log.info('Environment:\n%s', self.cmdenv)
        log.debug('Command:%s', ' '.join(self.args.command))
        print('Running        : %s'  % self.args.command)
        try:
            self.retval = subprocess.run(self.args.command, env=self.cmdenv)
            print('Completed        :')
        except FileNotFoundError as e:
            print(e)
            return 255
        return self.retval.returncode

    def waitforcompletion(self):
        self.thread.join()
