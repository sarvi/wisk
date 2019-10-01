'''
wisktrack -- A program to do wisktrack client storage and reize automatically


@author:     sarvi

@copyright:  2016 Cisco Inc. All rights reserved.

@license:    license

@contact:    sarvi@cisco.com
@deffield    updated: Updated
'''

from __future__ import print_function
import os
import sys
import uuid
import threading
import traceback
import logging
import subprocess
from argparse import ArgumentParser
from argparse import RawDescriptionHelpFormatter
from html2text import html2text
from common import env
from common import clientenv
from common import utils
from common.cmd_exception import CmdException

log = logging.getLogger(__name__)  # pylint: disable=locally-disabled, invalid-name

__all__ = []
__programname__ = 'wisktrack'
__programpath__ = os.path.join(env.INSTALL_BIN_DIR, __programname__)
__version__ = env.get_relversion(client=True)
__date__ = env.get_reldatetime()
__updated__ = env.get_reldatetime()


WISK_TRACKER_VERBOSITY = 0
WISK_TRACKER_PIPE='/tmp/wisk_tracker.pipe'
WISK_TRACKER_UUID=str(uuid.uuid4())

class TrackedRunner(object):
    def __init__(self, args):
        self.args = args
        log.info('Verbosity: %d', WISK_TRACKER_VERBOSITY)
        self.cmdenv = {
            'LD_PRELOAD': os.path.join(env.INSTALL_PKG_ROOT, 'libwisktrack.so'),
            'WISK_TRACKER_PIPE': WISK_TRACKER_PIPE,
            'WISK_TRACKER_UUID': WISK_TRACKER_UUID,
            'WISK_TRACKER_DEBUGLEVEL': ('%d' % (WISK_TRACKER_VERBOSITY))}
        print(self.cmdenv)
        self.thread = threading.Thread(target=self.run, args=())
        self.thread.daemon = True
        self.thread.start()
        return

    def run(self):
        global WISK_RV
        log.debug('Environment & Command: %s, %s', self.cmdenv, ' '.join(self.args.command))
        self.retval = subprocess.run(self.args.command, env=self.cmdenv)
        return self.retval.returncode
    
    def waitforcompletion(self):
        self.thread.join()

def create_reciever():
    if os.path.exists(WISK_TRACKER_PIPE):
        os.unlink(WISK_TRACKER_PIPE)
    print("WISK PID: %d" % os.getpid())
    print('Creating Recieving FIFO Pipe: %s' % WISK_TRACKER_PIPE)
    os.mkfifo(WISK_TRACKER_PIPE)

def read_reciever(args):
    file = open(args.trackfile, 'w') if args.trackfile else sys.stdout
    for l in open(WISK_TRACKER_PIPE).readlines():
        file.write(l.strip())

def delete_reciever():    
    print('\nDeleting Recieving FIFO Pipe: %s' % (WISK_TRACKER_PIPE))
    os.unlink(WISK_TRACKER_PIPE)

def dotrack(args):
    ''' do wisktrack of a command'''
    global WISK_TRACKER_VERBOSITY
    global WISK_RV
    WISK_TRACKER_VERBOSITY = args.verbose + 1
    create_reciever()
    runner = TrackedRunner(args)
    read_reciever(args)
    delete_reciever()
    runner.waitforcompletion()
    print('\nTracking Complete')
    return runner.retval.returncode


class CLIError(Exception):
    '''Generic exception to raise and log different fatal errors.'''

    def __init__(self, msg):
        super(CLIError).__init__(type(self))
        self.msg = "E: %s" % msg

    def __str__(self):
        return self.msg

    def __unicode__(self):
        return self.msg


@utils.timethis
def main(argv=None, testing=False):  # pylint: disable=locally-disabled, too-many-locals, R0915, too-many-branches
    '''Command line options.'''

    if argv is None:
        argv = sys.argv
    elif not testing:
        sys.argv.extend(argv)
    else:
        sys.argv = argv
    program_version = "%s" % __version__
    program_build_date = str(__updated__)
    program_version_message = '%%(prog)s %s (%s)' % (program_version, program_build_date)
    program_shortdesc = __doc__.split("\n")[1]
    program_license = '''%s

  Created by Sarvi Shanmugham on %s.
  Copyright 2016 Cisco Inc. All rights reserved.

USAGE
''' % (program_shortdesc, str(__date__))
    program_epilog = '''
wisktrack - track depencencies

Example:
    wisktrack
'''

    init = False
    try:
        init = clientenv.env_init(env.gettoolname(__programname__, subcommands=0), env.CLIENT_CFG_SEARCH, doclientcfg=True)

        parser = ArgumentParser(description=program_license, epilog=program_epilog,
                                formatter_class=RawDescriptionHelpFormatter)

        parser.epilog = program_epilog
        parser.formatter_class = RawDescriptionHelpFormatter
        parser.add_argument("-v", "--verbose", dest="verbose", action="count", default=0,
                            help="Set verbosity level [default: %(default)s]")
        parser.add_argument('-V', '--version', action='version', version=program_version_message)
        parser.add_argument('-dry', '--dryrun', action='store_true', default=False, help="A dry on of the operations")
        parser.add_argument('-trackfile', '--trackfile', type=str, default=None, help="Where to output the tracking data")

        parser.add_argument("command", type=str, nargs="+", help="Command an args to track")

        args = parser.parse_args()

        # Setup verbose
        env.logging_setup(args.verbose)

        return dotrack(args)
    except KeyboardInterrupt:
        # handle keyboard interrupt
        return 0
    except CmdException as ex:
        print(ex.message)
        print("\nFor Assistance: please open a ticket at http://devxsupport.cisco.com/, BU:DevX Tools, OS:Common Tools(non-OS specific), Tool:WIT")
        return 1
    except Exception as ex:  # pylint: disable=locally-disabled, broad-except
        logerror = log.error if init else print
        logdebug = log.debug if init else print
        logdebug(traceback.format_exc())
        if not args or not args.verbose:
            err_property = None
            if hasattr(ex, 'message'):
                err_property = getattr(ex, 'message', None)
            if (err_property is None or err_property == '') and hasattr(ex, 'error'):
                err_property = getattr(ex, 'error', None)
            else:
                err_property = getattr(ex, '', None)
            logerror('Error %s' % html2text(str(err_property)))
        print('Commmand: %s' % __programpath__)
        print("  for help use --help")
        print("\nFor Assistance: please open a ticket at http://devxsupport.cisco.com/, BU:DevX Tools, OS:Common Tools(non-OS specific), Tool:WIT")
        return 2
