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
import json
import subprocess
import re
import configparser
import itertools
from functools import partial
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
WISK_DEPDATA='wisk_depdata'
WISK_PARSER_CFG='wisk_parser.cfg'
WISK_INSIGHT_FILENAME='wisk_insight.data'
WISK_INSIGHT_FILE=None
UNRECOGNIZED_TOOLS_CXT = []
FIELDS_ALL = ['UUID', 'P-UUID', 'PID', 'PPID', 'WORKING_DIRECTORY', 'OPERATIONS', 'COMMAND', 'COMMAND_PATH', 
              'ENVIRONMENT', 'COMPLETE', 'command_type', 'children', 'WSROOT', 'invokes', 'mergedcommands']

CONFIG = None
dosplitlist = lambda x: [i.strip() for i in x.split()]
doregexlist = lambda x: [re.compile(i) for i in x]
CONFIG_DATATYPES = {
#     'DEFAULT': {
#         'filterfields': (dosplitlist,),
#         },
    'command_type': {
        'filterfields': (dosplitlist,),
        'buildtool_patterns': (dosplitlist, doregexlist),
        'shelltool_patterns': (dosplitlist, doregexlist),
        'hardtool_patterns': (dosplitlist, doregexlist),
        }
}

CONFIG_DEFAULTS = {
#    'filterfields': '',
    'filterfields': 'COMMAND_PATH COMMAND OPERATIONS WORKING_DIRECTORY ENVIRONMENT invokes mergedcommands command_type',
#    'shelltool_patterns': '',
#    'hardtool_patterns': '',
#    'buildtool_patterns': '',
}

CONFIG_TOOLS = ['hardtool', 'buildtool', 'shelltool']

def match_command_type(node):
    for tool_type in CONFIG_TOOLS:
        if any(i.match(node.command_path) for i in CONFIG['command_type']['%s_patterns'%tool_type]):
            log.debug('Command Type: %s(pattern)', node.command_path)
            log.debug([i.match(node.command_path) for i in CONFIG['command_type']['%s_patterns'%tool_type]])
            return tool_type
    n=node
    tab=''
    outdata=''
    cmdcxt=[]
    while n.uuid != WISK_TRACKER_UUID:
        cmdcxt.append(n.command_path)
        outdata = outdata + '%s%s [%s]\n' % (tab, n.command_path, ' '.join(n.command))
        tab=tab+'    '
        n = n.parent
    if cmdcxt not in UNRECOGNIZED_TOOLS_CXT:
        UNRECOGNIZED_TOOLS_CXT.append(cmdcxt)
        WISK_INSIGHT_FILE.write(outdata)
        log.error('Unrecognized Program: %s', node.command_path)
    return None

def checkfortooltypeinherit(node):
    if node.command_type == 'shelltool' and any((i.command_type == 'hardtool') for i in node.children):
        log.debug('Shell-inherits HardTool: %s', node.command_path)
        node.command_type = 'hardtool'
    if node.command_type == 'shelltool' and any((i.command_type == 'buildtool') for i in node.children):
        log.debug('Shell-inherits BuildTool: %s', node.command_path)
        node.command_type = 'buildtool'

def checkformerge(node):
    if node.uuid == WISK_TRACKER_UUID:
        log.debug('Root: Skip Merging')
        return False
    if node.parent.uuid == WISK_TRACKER_UUID:
        log.debug('Top-Tool: Skip Merging %s', node.command)
        return False
    if node.command_type is None:
        log.debug('Not-Tool: Merging %s', node.command)
        return True
    if node.command_type in ['hardtool'] and node.parent.command_type in ['hardtool']:
        log.debug('HardTool-by-HardTool: Merging %s child-of %s', node.command, node.parent.command)
        return True
    if node.command_type in ['hardtool'] and node.parent.command_type in ['shelltool']:
        log.debug('ShellTool-by-HardTool: Merging %s child-of %s', node.command, node.parent.command)
        return True
    if node.command_type in ['buildtool'] and node.parent.command_type in ['buildtool']:
        log.debug('BuildTool-by-BuildTool: Merging %s child-of %s', node.command, node.parent.command)
        return True
    if node.command_type in ['buildtool'] and node.parent.command_type in ['shelltool']:
        log.debug('Shell-by-BuildTool: Merging %s child-of %s', node.command, node.parent.command)
        return True
    return False

def domerge(node):
    assert node.uuid != WISK_TRACKER_UUID and node.parent.uuid != WISK_TRACKER_UUID
    for cn in node.children:
        cn.parent = node.parent
        cn.parent.children.append(cn)
    node.parent.children.remove(node)
    node.parent.mergedcommands.append(node)
    node.parent = None
    node.children = []
    node.filteredout = True

def filterlistpaths(paths):
    r=[]
    for i in paths:
        if i not in r and ((isinstance(i, list) and not any([j for j in i if j.startswith('/')])) or (not isinstance(i, list) and not i.startswith('/'))):
            r.append(i)
    return r

def tojson(o, fields=None):
    if fields:
        return dict((k,v) for k,v in o if k in fields)
    else:
        return dict(o)

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
        self.working_directory = None
        self.complete = False
        self.environment = {}
        self.children = []
        self.operations = []
        self.pid = None
        self.ppid = None
        self.command_type = None
        self.filteredout = False
        self.tobemerged = False
        self.mergedcommands=[]
        if parent:
            p = ProgramNode.progtree[parent]
            p.children.append(self)
        for k,v in kwargs.items():
            setattr(self, k,v)
        ProgramNode.count += 1
        if uuid in ProgramNode.progtree:
            print(str(ProgramNode.progtree[uuid]), str(self))
        ProgramNode.progtree[uuid] = self


    def remove(self):
        log.debug('Removing: %s', self.command_path) 
        self.operations = None
        self.parent.children.remove(self)
        self.parent = None
        ProgramNode.progtree.pop(self.uuid)
        ProgramNode.count -= 1

    def __repr__(self):
        return str(self.command)
    
    def __str__(self):
        l = dict(self)
        del l['invokes']
        return json.dumps(dict(l), indent=4, sort_keys=True)
    
    def __iter__(self):
        yield 'UUID', self.uuid
        yield 'P-UUID', self.parent.uuid if self.parent is not None else None
        yield 'command_type', self.command_type
        yield 'WORKING_DIRECTORY', self.working_directory
        yield 'COMMAND', ' '.join(self.command) if self.command else self.command
        yield 'COMMAND_PATH', self.command_path
        yield 'ENVIRONMENT', self.environment
        yield 'PID', self.pid
        yield 'PPID', self.ppid
        yield 'COMPLETE', self.complete
        wsroot= getattr(self, 'WSROOT', None)
        if wsroot:
            yield 'WSROOT', wsroot
        yield 'OPERATIONS', self.operations
        yield 'mergedcommands', [' '.join(i.command) for i in self.mergedcommands]
        yield 'children', len(self.children)
        yield 'invokes', self.children

    def node_complete(self):
        self.complete = True

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
    def add_operation(cls, uuid, operation, data, buffer=False):
        if uuid not in cls.progtree:
            ProgramNode(uuid)
        pn = cls.progtree[uuid]
        buffer_name = '_'+operation.lower()+'_buffer'
        opbuffer = getattr(pn, buffer_name, '')
        if opbuffer:
            opbuffer = opbuffer[:-1] + data[1:]
        else:
            opbuffer = opbuffer + data
        if (operation in ['ENVIRONMENT', 'COMMAND', 'LINKS', 'COMPLETE'] and not data.endswith(']\n')) \
            or (operation not in ['ENVIRONMENT', 'COMMAND', 'LINKS', 'COMPLETE'] and not data.endswith('"\n')):
            setattr(pn, buffer_name, opbuffer)
            return
        data = opbuffer
        setattr(pn, buffer_name, '')
        try:
            data = json.loads(opbuffer)
        except json.decoder.JSONDecodeError as e:
            log.error('Error Decoding: %s%s', data, e)
            column = re.search(re.compile(r'line 1 column (?P<column>[0-9]+) '), str(e)).group('column')
            log.error('Column: %s', column)
            column = int(column)
            log.error("Error at string: %s'%s'%s", data[column-10:column-1], data[column-1], data[column:])
            raise
        if operation in ['COMMAND_PATH', 'READS', 'WRITES', 'UNLINK']:
            data = os.path.normpath(data).replace(WSROOT+'/', '')
        elif operation in ['LINKS']:
            data = [os.path.normpath(i).replace(WSROOT+'/', '') for i in data]
        if operation in ['ENVIRONMENT']:
            data = [i for i in data if not (i.startswith('WISK_') or i.startswith('LD_PRELOAD'))]
            data = [i.split('=',1) for i in data]
            data = dict([(i if len(i)==2 else (i[0], '')) for i in data])
            getattr(pn, operation.lower()).update(data)
        elif operation in ['COMMAND', 'CALLS', 'PID', 'PPID', 'WORKING_DIRECTORY']:
            setattr(pn, operation.lower(), data)
        elif operation in ['COMMAND_PATH',]:
            setattr(pn, operation.lower(), data)
        elif operation in ['COMPLETE']:
            pn.command_type = match_command_type(pn)
            pn.node_complete()
        else:
            pn.operations.append((operation, data))


    @classmethod
    def merge_node(cls, node=None):
        if node is None:
            node = ProgramNode.progtree[WISK_TRACKER_UUID]
        if not node.complete:
            log.error('Incomplete Command: %s [%s]', node.command_path, ' '.join(node.command))
            WISK_INSIGHT_FILE.write('Incomplete Command: %s [%s]' % (node.command_path, ' '.join(node.command)))
        checkfortooltypeinherit(node)
        for cn in list(node.children):
            ProgramNode.merge_node(cn)
        if checkformerge(node):
            node.parent.operations.extend(node.operations)
            node.operations=[]
            domerge(node)
            node.tobemerged = True
        if not node.children:
            node.operations={k:filterlistpaths([i[1] for i in g]) for k, g in itertools.groupby(sorted(node.operations, key=lambda i: i[0]), lambda x: x[0])}
            

    @classmethod        
    def show_nodes(cls, ofile, node=None, fields=None):
        if node is None:
            node = ProgramNode.progtree[WISK_TRACKER_UUID].children[0]
        fields = fields or CONFIG['DEFAULT']['filterfields']
        json.dump(node, open(ofile, 'w'), default=partial(tojson, fields=fields), indent=2, sort_keys=True)

      
def compact_environment(program=None):
    if program is None:
        program = ProgramNode.progtree[WISK_TRACKER_UUID]
    for p in program.children:
        compact_environment(p)
        parent = p.parent
        p.environment = {k:v for k,v in p.environment.items() if k not in parent.environment}            

def expand_environment(program=None):
    if program is None:
        program = ProgramNode.progtree[WISK_TRACKER_UUID]
    for p in program.children:
        parent = p.parent
        p.environment = {**p.environment, **parent.environment}            
        expand_environment(p)
        

def clean_data(args):
    print('Extracting and Cleaning Data')
    ProgramNode(WISK_TRACKER_UUID).complete=True
    ifile = open(args.trackfile + '.raw')
    count = 0
    for l in ifile.readlines():
        log.debug('(%s)', l)
        parts = l.split(' ',2)
        uuid = parts[0]
        operation = parts[1].strip()
        data = parts[2]
        if operation=='CALLS':
            ProgramNode(json.loads(data), uuid)
            count += 1
        else:
            ProgramNode.add_operation(uuid, operation, data)
    compact_environment()
    print('Writing cleanedup full dependency data to %s' % (args.trackfile+'.dep'))
    ProgramNode.show_nodes(args.trackfile+'.dep', fields=FIELDS_ALL)
    expand_environment()
    print('Extracting Top level commands ...')
    ProgramNode.merge_node()
    compact_environment()
    print('Writing Top level Commands to %s' % (args.trackfile+'.cmds'))
    ProgramNode.show_nodes(args.trackfile+'.cmds')


class TrackerReciever(object):
    def __init__(self, args):
        self.rawfile = args.trackfile + '.raw'
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
    global WISK_TRACKER_PIPE
    if WISK_TRACKER_PIPE is None:
        WISK_TRACKER_PIPE="/tmp/wisk_tracker.pipe"
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
        filetoshow = args.trackfile + ('.cmds' if args.clean else '.raw')
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


def configparse(configfile):
    ''' Configuration File '''
    global CONFIG
    config = configparser.ConfigParser(defaults=CONFIG_DEFAULTS, interpolation=configparser.BasicInterpolation())
    print('Reading Config: %s' % (configfile))
    config.read(configfile)
    CONFIG = {}
    for secname, secproxy in config.items():
        CONFIG[secname] = {}
        for k, v in secproxy.items():
            CONFIG[secname][k] = v.strip()
            dtype = CONFIG_DATATYPES.get(secname, {}).get(k, None)
            if dtype is None:
                continue                
            for do_op in dtype:
                CONFIG[secname][k] = do_op(CONFIG[secname][k])

def insight_init(args):
    global WISK_INSIGHT_FILE
    WISK_INSIGHT_FILE = open(os.path.join(args.wsroot, WISK_INSIGHT_FILENAME) if os.path.exists(args.wsroot) else WISK_INSIGHT_FILENAME, 'w')

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
    for k,v in vars(args).items():
        if k == 'command': continue
        if isinstance(v, list):
            v= ' '.join(str(i) for i in v)
        else:
            v = str(v)
        CONFIG_DEFAULTS[k]=v
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
        parser.add_argument('-trackfile', '--trackfile', type=str, default=os.path.join(os.getcwd(), WISK_DEPDATA), help="Where to output the tracking data")
        parser.add_argument('-config', '--config', type=str, default=WISK_PARSER_CFG, help="WISK Parser Configration")

        args = partialparse(parser)

        env.logging_setup(args.verbose)
        configparse(args.config)
        insight_init(args)

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
