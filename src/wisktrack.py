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
COMMAND_SEPARATOR='---'
WISK_TRACKER_PIPE=None
WISK_TRACKER_UUID='XXXXXXXX-XXXXXXXX-XXXXXXXX'
WISK_DEPDATA_RAW='wisk_depdata.raw'
WISK_DEPDATA='wisk_depdata.dep'

class ProgramNode(object):
    progtree = {}
    count = 0
    
    def __init__(self, uuid, parent=None, **kwargs):
        self.uuid = uuid
        if parent is None:
            self.parent = None
        else:
            self.parent = ProgramNode.progtree[parent]
        self.command = None
        self.command_path = None
        self.environment = {}
        self.children = []
        self.operations = []
        self.pid = None
        self.ppid = None
        if parent:
            p = ProgramNode.progtree[parent]
            p.children.append(self)
        for k,v in kwargs.items():
            setattr(self, k,v)
        ProgramNode.count += 1
        if uuid in ProgramNode.progtree:
            print(str(ProgramNode.progtree[uuid]), str(self))
        ProgramNode.progtree[uuid] = self

    def __str__(self):        
        l={
            'UUID': self.uuid,
            'P-UUID': self.parent.uuid if self.parent is not None else None,
            'PID': self.pid,
            'PPID': self.ppid,
            'OPERATIONS': self.operations,
            'COMMAND': self.command,
            'COMMAND_PATH': self.command_path,
            'ENVIRONMENT': self.environment,
            'children': len(self.children),
            }
        wsroot= getattr(self, 'WSROOT', None)
        if wsroot:
            l['WSROOT'] = wsroot
        return json.dumps(l, indent=4, sort_keys=True)

    @classmethod
    def getorcreate(cls, uuid, parent=None, **kwargs):
        if uuid not in cls.progtree:
            prog = ProgramNode(uuid, parent, **kwargs)
        else:
            prog = ProgramNode.progtree[uuid]
        if parent:
            prog.parent = ProgramNode.getorcreate(parent)
            prog.parent.children.append(prog)
        return prog

    @classmethod
    def add_operation(cls, uuid, operation, data):
        if uuid not in cls.progtree:
            ProgramNode(uuid)
        pn = cls.progtree[uuid]
        if operation in ['COMMAND_PATH', 'READS', 'WRITES']:
            data = os.path.normpath(data).replace(WSROOT+'/', '')
        elif operation in ['LINKS']:
            data = [os.path.normpath(i).replace(WSROOT+'/', '') for i in data]
        if operation in ['ENVIRONMENT']:
            getattr(pn, operation.lower()).update(data)
        elif operation in ['COMMAND', 'COMMAND_PATH', 'COMPLETE', 'CALLS', 'PID', 'PPID']:
            setattr(pn, operation.lower(), data)
        else:
            pn.operations.append((operation, data))

#     @classmethod
#     def clean(cls):
#         programs = list(ProgramNode.progtree[WISK_TRACKER_UUID].children)
#         while programs:
#             programs.extend(p.children)
#         while programs:
#             p =programs.pop()
#             parent = p.parent
#             p.environment = dict(p.environment.items() - parent.environment.items())            

    @classmethod        
    def show_nodes(cls, ofile, node=None):
        if node is None:
            node = ProgramNode.progtree[WISK_TRACKER_UUID]
        l =[node]
        ofile.write('Objects: %d' % ProgramNode.count)
        for n in l:
            ofile.write("%s\n" % (n))
            l.extend(n.children)
        ofile.write('Objects: %d' % len(l))
        assert len(l) == ProgramNode.count
        
def readenoughlines(ifile):
    buffer = []
    for line in ifile.readlines():
        if buffer:
            if line.endswith(']\n'):
                buffer.append(line)
                yield ''.join(buffer)
                buffer = []
            else:
                buffer.append(line)
        elif line.split(' ', 2)[1] == "COMMAND" and not line.endswith(']\n'):
            buffer.append(line)
        else:
            yield line
    if buffer:
        yield ''.join(buffer)
                
def clean_environment(program=None):
    if program is None:
        program = ProgramNode.progtree[WISK_TRACKER_UUID]
    for p in program.children:
        clean_environment(p)
        parent = p.parent
        p.environment = dict(p.environment.items() - parent.environment.items())            

def clean_data(args):
    print('Extracting and Cleaning Data')
    ProgramNode(WISK_TRACKER_UUID)
    ifile = open(args.rawfile)
    count = 0
    for l in readenoughlines(ifile):
        parts = l.split(' ',2)
        uuid = parts[0]
        operation = parts[1].strip()
        data = parts[2]
        if operation not in "COMMAND":
            try:
                data = json.loads(data)
            except json.decoder.JSONDecodeError as e:
                log.error('Error Decoding: %s%s', l, e)
                raise
        if operation=='CALLS':
            ProgramNode(data, uuid)
            count += 1
        elif operation == 'COMMAND':
            ProgramNode.add_operation(uuid, operation, data)
        elif operation == 'ENVIRONMENT':
            data = [i for i in data if not (i.startswith('WISK_') or i.startswith('LD_PRELOAD'))]
            data = [i.split('=',1) for i in data]
            data = dict([(i if len(i)==2 else (i[0], '')) for i in data])
            ProgramNode.add_operation(uuid, operation, data)
        else:
            ProgramNode.add_operation(uuid, operation, data)
    clean_environment()
    ProgramNode.show_nodes(open(args.trackfile, 'w'))
    


        

class TrackerReciever(object):
    def __init__(self, args):
        self.rawfile = args.rawfile
        self.thread = threading.Thread(target=self.run, args=())
        self.thread.daemon = True
        self.thread.start()
        return
    
    def run(self):
        print('Reading RAW Dependency from: %s'  % (self.rawfile))
        command = '/bin/cat %s > %s' % (WISK_TRACKER_PIPE, self.rawfile)
        return subprocess.run(command, shell=True)
    
    def waitforcompletion(self):
        self.thread.join()

def create_reciever():
    if os.path.exists(WISK_TRACKER_PIPE):
        os.unlink(WISK_TRACKER_PIPE)
    log.info("WISK PID: %d", os.getpid())
    log.info('Creating Recieving FIFO Pipe: %s', WISK_TRACKER_PIPE)
    os.mkfifo(WISK_TRACKER_PIPE)

def tracked_run(args):
    retval = None
    log.info('WISK Verbosity: %d', args.verbose-1)
    cmdenv = dict(os.environ)
    log.debug('LATEST_LIB_DIR: %s', env.LATEST_LIB_DIR)
    cmdenv.update({
        'LD_LIBRARY_PATH': ':'.join(['', os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib32'),
                                     os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib64')]),
#        'LD_LIBRARY_PATH': ':'.join([os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib32')]),
        'LD_PRELOAD': 'libwisktrack.so',
        'WISK_TRACKER_PIPE': WISK_TRACKER_PIPE,
        'WISK_TRACKER_UUID': WISK_TRACKER_UUID,
        'WISK_TRACKER_DEBUGLEVEL': ('%d' % (args.verbose))})
    if args.verbose > 4:
        cmdenv.update({'LD_DEBUG': 'all'})
    log.debug('Environment:\n%s', cmdenv)
    log.debug('Command:%s', ' '.join(args.command))
    print('Running: %s'  % (' '.join(args.command)))
    try:
        retval = subprocess.run(args.command, env=cmdenv)
#         retval = subprocess.run(args.command, env=cmdenv, stdout=open('stdout.log', 'w'), stderr=open('stderr.log', 'w'))
    except FileNotFoundError as e:
        print(e)
        return None
    return retval

def delete_reciever(reciever):    
    reciever.waitforcompletion()
    log.info('\nDeleting Recieving FIFO Pipe: %s', WISK_TRACKER_PIPE)
    os.unlink(WISK_TRACKER_PIPE)

def doinit(args):
    global WSROOT
    WSROOT = args.wsroot

def dotrack(args):
    ''' do wisktrack of a command'''
    result = None
    doinit(args)
    if args.command:
        create_reciever()
        reciever = TrackerReciever(args)
        result = tracked_run(args)
        delete_reciever(reciever)
        print(result)
    if args.clean:
        clean_data(args)
    if args.show:
        filetoshow = args.trackfile if args.clean else args.rawfile
        for line in open(filetoshow):
            print(line, end='')
    
    return (result.returncode if result else 0)


class CLIError(Exception):
    '''Generic exception to raise and log different fatal errors.'''

    def __init__(self, msg):
        super(CLIError).__init__(type(self))
        self.msg = "E: %s" % msg

    def __str__(self):
        return self.msg

    def __unicode__(self):
        return self.msg

def partialparse(parser):
    ''' Parse args until first unknown '''
    global WISK_TRACKER_PIPE
    if COMMAND_SEPARATOR in sys.argv:
        idx = sys.argv.index(COMMAND_SEPARATOR)
    else:
        idx = len(sys.argv)
    args = parser.parse_args(sys.argv[1:idx])
    args.command = sys.argv[idx+1:]
    log.debug('Args & Command: %s', args)
    WISK_TRACKER_PIPE=("%s/wisk_tracker.pipe" % args.wsroot)
    return args
        

    

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
        parser.add_argument('-show', '--show', action='store_true', default=False, help="Show Depenency Data")
        parser.add_argument('-clean', '--clean', action='store_true', default=False, help="Show Cleaned Command Tree ")
        parser.add_argument('-wsroot', '--wsroot', type=str, default=os.getcwd(), help="Workspace Root")
        parser.add_argument('-rawfile', '--rawfile', type=str, default=os.path.join(os.getcwd(), WISK_DEPDATA_RAW), help="RAW Data File")
        parser.add_argument('-trackfile', '--trackfile', type=str, default=os.path.join(os.getcwd(), WISK_DEPDATA), help="Where to output the tracking data")

        args = partialparse(parser)

        # Setup verbose
        env.logging_setup(args.verbose)
        # env.ENVIRONMENT['verbosity'] = 0
        # init = 0

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
