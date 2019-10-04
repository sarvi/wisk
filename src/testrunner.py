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
            'LD_PRELOAD': os.path.join(env.LATEST_LIB_DIR, 'libwisktrack.so'),
            'WISK_TRACKER_PIPE': WISK_TRACKER_PIPE,
            'WISK_TRACKER_UUID': WISK_TRACKER_UUID,
            'WISK_TRACKER_DEBUGLEVEL': ('%d' % (self.wisk_verbosity))})
        self.thread = threading.Thread(target=self.run, args=())
        self.thread.daemon = True
        self.thread.start()
        return

    def run(self):
        log.debug('Environment:\n%s', self.cmdenv)
        log.debug('Command:%s', ' '.join(self.args.command))
        print('Running: %s'  % self.args.command)
        try:
            self.retval = subprocess.run(self.args.command, env=self.cmdenv)
        except FileNotFoundError as e:
            print(e)
            return 255
        return self.retval.returncode

    def waitforcompletion(self):
        self.thread.join()
