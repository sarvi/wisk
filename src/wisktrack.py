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
import time
import fcntl
import threading
import traceback
import logging
import random
import json
import subprocess
import re
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


WSROOT = None
WISK_TRACKER_PIPE='/tmp/wisk_tracker.pipe'
WISK_TRACKER_UUID='XXXXXXXX-XXXXXXXX-XXXXXXXX'

class ProgramNode(object):
    progtree = {}
    
    def __init__(self, uuid, parent=None, **kwargs):
        self.uuid = uuid
        if parent is None:
            self.parent = None
            self.command = None
            self.command_path = None
            self.environment = {}
        else:
            self.parent = ProgramNode.progtree[parent]
        self.children = []
        self.operations = []
        if parent:
            p = ProgramNode.progtree[parent]
            p.children.append(self)
        for k,v in kwargs.items():
            setattr(self, k,v)
        ProgramNode.progtree[uuid] = self

    def __str__(self):        
        l={
            'UUID': self.uuid,
            'P-UUID': self.parent.uuid if self.parent is not None else None,
            'OPERATIONS': self.operations,
            'COMMAND': self.command,
            'COMMAND_PATH': self.command_path,
            'ENVIRONMENT': self.environment,
            }
        wsroot= getattr(self, 'WSROOT', None)
        if wsroot:
            l['WSROOT'] = wsroot
        return json.dumps(l, indent=4, sort_keys=True)

    @classmethod
    def add_operation(cls, uuid, operation, data):
        pn = cls.progtree[uuid]
        if operation in ['COMMAND_PATH', 'READS', 'WRITES']:
            data = os.path.normpath(data).replace(WSROOT+'/', '')
        elif operation in ['LINKS']:
            data = [os.path.normpath(i).replace(WSROOT+'/', '') for i in data]
        if operation in ['COMMAND', 'COMMAND_PATH', 'COMPLETE', 'CALLS', 'ENVIRONMENT']:
            setattr(pn, operation.lower(), data)
        else:
            pn.operations.append((operation, data))

    @classmethod
    def clean(cls):
        programs = ProgramNode.progtree[WISK_TRACKER_UUID].children
        for p in programs:
            programs.extend(p.children)
            parent = p.parent
            p.environment = dict(p.environment.items() - parent.environment.items())
            

    @classmethod        
    def show_nodes(cls, node=None):
        if node is None:
            node = ProgramNode.progtree[WISK_TRACKER_UUID]
        l =[node]
        for n in l:
            print('%s' % n)
            l.extend(n.children)
        
        

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
        print(self.cmdenv)
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

def create_reciever():
    if os.path.exists(WISK_TRACKER_PIPE):
        os.unlink(WISK_TRACKER_PIPE)
    print("WISK PID: %d" % os.getpid())
    print('Creating Recieving FIFO Pipe: %s' % WISK_TRACKER_PIPE)
    os.mkfifo(WISK_TRACKER_PIPE)

def readlineswithwait(runner, ifilefd):
    ifile=os.fdopen(ifilefd)
    while runner.thread.is_alive():
        try:
            line = ifile.readline()
        except:
            continue
        if line:
            yield line
    flags = fcntl.fcntl(ifilefd, fcntl.F_GETFL)
    flags = flags & ~os.O_NONBLOCK
    fcntl.fcntl(ifilefd, fcntl.F_SETFL,  flags)
    for line in ifile.readlines():
        yield line 

def read_reciever(runner, args):
    ofile= open(args.trackfile, 'w') if args.trackfile else sys.stdout
#     ifilefd = os.open(WISK_TRACKER_PIPE, os.O_RDONLY | os.O_NONBLOCK)
    ifilefd = os.open(WISK_TRACKER_PIPE, os.O_RDONLY)
    for l in readlineswithwait(runner, ifilefd):
        parts = l.split(' ',2)
        uuid = parts[0].strip(':')
        operation = parts[1].strip()
        data = parts[2]
        data = json.loads(data)
        ofile.write('{} {} {}\n'.format(uuid, operation, data))
        if operation=='CALLS':
            ProgramNode(data, uuid)
        elif operation == 'ENVIRONMENT':
            data = [i for i in data if not (i.startswith('WISK_') or i.startswith('LD_PRELOAD'))]
            data = dict([i.split('=',1) for i in data])
        ProgramNode.add_operation(uuid, operation, data)

def delete_reciever():    
    print('\nDeleting Recieving FIFO Pipe: %s' % (WISK_TRACKER_PIPE))
    os.unlink(WISK_TRACKER_PIPE)

def dotrack(args):
    ''' do wisktrack of a command'''
    global WSROOT
    WSROOT = args.wsroot
    ProgramNode(WISK_TRACKER_UUID,wsroot=args.wsroot)
    create_reciever()
    runner = TrackedRunner(args)
    read_reciever(runner, args)
    delete_reciever()
    if runner.thread.is_alive():
        runner.waitforcompletion()
    print('\nTracking Complete,', end='')
    if args.trackfile:
        print(' Trackfile at %s' % args.trackfile)
    ProgramNode.clean()
    if args.show:
        ProgramNode.show_nodes()
    return (runner.retval.returncode if runner.retval is not None else 0)


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
        parser.add_argument('-show', '--show', action='store_true', default=False, help="Show Tree")
        parser.add_argument('-wsroot', '--wsroot', type=str, default=os.getcwd(), help="Workspace Root")
        parser.add_argument('-trackfile', '--trackfile', type=str, default=None, help="Where to output the tracking data")

        args, command = parser.parse_known_args()
        args.command = command

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
