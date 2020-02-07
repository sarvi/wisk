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
import shutil
import pdb
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
WISK_DEBUGLOG='wisk_debug.log'
WISK_INSIGHT_FILENAME='wisk_insight.data'
WISK_INSIGHT_FILE=None
WISK_ARGS=None
WISK_EVENTFILTERS=['writes', 'reads', 'links', 'chmods','process']
UNRECOGNIZED_TOOLS_CXT = []
FIELDS_ALL = ['UUID', 'P-UUID', 'PID', 'PPID', 'WORKING_DIRECTORY', 'OPERATIONS', 'COMMAND', 'COMMAND_PATH', 
              'ENVIRONMENT', 'COMPLETE', 'command_type', 'children', 'WSROOT', 'invokes', 'mergedcommands']

CONFIG = None
dosplitlist = lambda x: [i.strip() for i in x.split()]
doregexlist = lambda x: [re.compile(i) for i in x]
doregexpatternlist = lambda x: [i.pattern for i in x]
dojoinlist = lambda x: '\n    '.join(x)

RE_RELPATH=re.compile(r'\.\.?\/.*')

CONFIGPARSER = None
CONFIG_DATATYPES = {
#     'DEFAULT': {
#         'filterfields': ((dosplitlist,), (lambda x: ' '.join(x),)),
#         },
    'command_type': {
        'filterfields': ((dosplitlist,), (lambda x: ' '.join(x),)),
        'buildtool_patterns': ((dosplitlist, doregexlist), (doregexpatternlist, dojoinlist)),
        'shelltool_patterns': ((dosplitlist, doregexlist), (doregexpatternlist, dojoinlist)),
        'hardtool_patterns':  ((dosplitlist, doregexlist), (doregexpatternlist, dojoinlist)),
        'interptool_patterns': ((dosplitlist, doregexlist), (doregexpatternlist, dojoinlist)),
        }
}

CONFIG_DEFAULTS = {
#    'filterfields': '',
    'filterfields': 'COMMAND_PATH COMMAND OPERATIONS WORKING_DIRECTORY ENVIRONMENT invokes mergedcommands command_type',
#    'shelltool_patterns': '',
#    'hardtool_patterns': '',
#    'buildtool_patterns': '',
}

CONFIG_TOOLS = ['buildtool', 'shelltool', 'hardtool']


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

def getuuidsabove(uuid, include=True):
    l = []
    if include:
        l.append(uuid)
    pn = ProgramNode.progtree[uuid].parent
    while pn:
        l.insert(0, pn.uuid)
        pn = pn.parent
    return l


def getuuidsbelow(uuid, include=True):
    l = []
    if include:
        l.append(uuid)
    pns = list(ProgramNode.progtree[uuid].children)
    while pns:
        pn = pns.pop(0)
        l.append(pn.uuid)
        pns.extend(pn.children)
    return l

def compactcommand(cmd, length=100, partlen=30):
    s=[]
    for c in cmd:
        partlen = min(partlen, length)
        size = len(c)
        if size > partlen:
            c = (c[:partlen]+'...') if c.startswith('-') else ('...' + c[-partlen:])
            length -= partlen+4 
        else:
            length -= size+1
        s.append(c)
        if length<=0:
            s.append('...')
            return ' '.join(s)
    return ' '.join(s)
    
    

class ProgramNode(object):
    progtree = {}
    count = 0
    
    def __init__(self, uuid, parent=None, **kwargs):
        self.uuid = uuid
        assert uuid not in ProgramNode.progtree, "Conflicting UUID values"
        if parent is None:
            self.parent = None
        else:
            self.parent = ProgramNode.progtree[parent]
        self.command = None
        self.command_path = None
        self.scriptlang = None
        self.working_directory = None
        self.complete = False
        self.environment = {}
        self.children = []
        self.operations = {}
        self.pid = None
        self.ppid = None
        self.command_type = None
        self.filteredout = False
        self.mergedcommands=[]
        if parent:
            p = ProgramNode.progtree[parent]
            p.children.append(self)
        for k,v in kwargs.items():
            setattr(self, k,v)
        ProgramNode.count += 1
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


    def match_command_type(self):
        command = self.command[0]
        for tool_type in CONFIG_TOOLS:
            if any(i.match(command) for i in CONFIG['command_type']['%s_patterns'%tool_type]):
                log.debug('Command Type: %s(pattern)', command)
                log.debug([i.match(command) for i in CONFIG['command_type']['%s_patterns'%tool_type]])
                self.command_type = tool_type
                return self.command_type
        n=self
        tab=''
        outdata=''
        cmdcxt=[]
        while n.uuid != WISK_TRACKER_UUID:
            cmdcxt.append(command)
            outdata = outdata + ('{!s} {!s} {!s} {!s:<.20} [{!r:<.100}]\n'.format(tab, n.command_type, n.uuid, os.path.basename(n.command_path), compactcommand(n.command)))
            tab=tab+'    '
            n = n.parent
        print('\n%sUnrecognized Tool: %r\n[i]-InterpTool, [b] - Buildtool, [s] - ShellTool, [Enter] - hardtool, [e] - Save/Exit, [q] - Quit' % (outdata, compactcommand(self.command)))
        while True:
            c = utils.getch()
            print('"[%d]"' % ord(c))
            if c in ['i', 'I']:
                self.command_type = 'interptool'
            elif c in ['b', 'B']:
                self.command_type = 'buildtool'
            elif c in ['s', 'S']:
                self.command_type = 'shelltool'
            elif ord(c) in [13]:
                self.command_type = 'hardtool'
            elif c in ['e', 'E']:
                configwrite(WISK_ARGS.config)
                sys.exit('Saving and Exiting...')
            elif c in ['q', 'Q']:
                sys.exit('Terminating...')
            elif c in ['g', 'G']:
                pdb.set_trace()
            else:
                continue
            break
        print('%s for Tool: %r' % (self.command_type, compactcommand(self.command)))
        CONFIG['command_type']['%s_patterns' % self.command_type].append(re.compile(re.escape(command)))
        return self.command_type


    def generate_insight(self, insight_type):
        if self.uuid == WISK_TRACKER_UUID:
            return
        command = self.command[0] or []
        n=self
        tab=''
        outdata=''
        cmdcxt=[]
        while n.uuid != WISK_TRACKER_UUID:
            cmdcxt.append(command)
            outdata = outdata + ('{!s} {!s} {!s} {!s:<.20} [{!r:<.150}]\n'.format(tab, n.command_type, n.uuid, os.path.basename(n.command_path), ' '.join(n.command)))
            tab=tab+'    '
            n = n.parent
        if cmdcxt not in UNRECOGNIZED_TOOLS_CXT:
            UNRECOGNIZED_TOOLS_CXT.append(cmdcxt)
            WISK_INSIGHT_FILE.write('UUID: %s : %s\n' % (self.uuid, insight_type))
            WISK_INSIGHT_FILE.write(outdata)
            log.info('Unrecognized Program: %r', compactcommand(self.command))
        self.command_type = 'hardtool'
        return self.command_type

    def checkfortooltypeinherit(self):
        if self.command_type == 'shelltool' and all((i.command_type == 'hardtool') for i in self.mergedcommands+self.children):
            log.debug('Shell-inherits All HardTool: %s', self.command_path)
            self.command_type = 'hardtool'
        elif self.command_type == 'shelltool' and all((i.command_type == 'buildtool') for i in self.mergedcommands+self.children):
            log.debug('Shell-inherits All BuildTool: %s', self.command_path)
            self.command_type = 'buildtool'
    
    def checkformerge(self):
        if self.uuid == WISK_TRACKER_UUID:
            log.debug('Root: Skip Merging')
            return False
        if self.children:
            return False
        if self.parent.uuid == WISK_TRACKER_UUID:
            log.debug('Top-Tool: Skip Merging %r', compactcommand(self.command))
            return False
        if self.command_type is None:
            log.error('Not-Tool: Merging %.50r', compactcommand(self.command))
            self.generate_insight('Command Type None')
            return True
        if self.command_type in ['hardtool'] and self.parent.command_type in ['hardtool']:
            log.debug('HardTool-by-HardTool:\n\t Merging %r\n\t child-of %r', compactcommand(self.command), compactcommand(self.parent.command))
            return True
        if self.command_type in ['hardtool'] and self.parent.command_type in ['shelltool']:
            log.debug('ShellTool-by-HardTool:\n\t Merging %r\n\t child-of %r', compactcommand(self.command), compactcommand(self.parent.command))
            return True
#        if self.command_type in ['buildtool'] and self.parent.command_type in ['buildtool']:
#            log.debug('BuildTool-by-BuildTool:\n\t Merging %r\n\t child-of %r', compactcommand(self.command), compactcommand(self.parent.command))
#            return True
#        if self.command_type in ['buildtool'] and self.parent.command_type in ['shelltool']:
#            log.debug('Shell-by-BuildTool:\n\t Merging %r\n\t child-of %r', compactcommand(self.command), compactcommand(self.parent.command))
#            return True
        return False

    def command_clean(self):
        interptool = CONFIG['command_type']['interptool_patterns']
        command = ' '.join(self.command)
        for p in interptool:
            m = p.match(command)
            if m and m.lastindex is not None:
                log.info('InterpTool: %s, %s', p.pattern, m.groups())
                log.info('            %r -> %r' % (compactcommand(self.command), compactcommand(self.command[m.lastindex:])))
                scriptlang = []
                for i in m.groups():
                    if i:
                        scriptlang.append(self.command.pop(0))
                self.scriptlang = ' '.join(scriptlang)
                break
        nenv = self.parent.environment or self.environment or os.environ
        pth = ':'.join([nenv.get('PATH', ''), self.working_directory])
        if RE_RELPATH.match(self.command[0]):
            cmd = os.path.join(self.working_directory, self.command[0])
        else:
            cmd = shutil.which(self.command[0], path=pth) 
        if cmd is None:
            log.warn('Cannot find program: %s in %s', self.command[0], pth)
        else:
            self.command[0] = os.path.normpath(cmd)
        self.match_command_type()
         

    def domerge(self):
        assert self.uuid != WISK_TRACKER_UUID and self.parent.uuid != WISK_TRACKER_UUID
        log.info('Merging: [%s] %r\n   with: [%s] %r', self.uuid, compactcommand(self.command), self.uuid, compactcommand(self.parent.command))
        for k,v in self.operations.items():
            self.parent.operations.setdefault(k, [])
            for i in v:
                if i not in self.parent.operations[k]:
                    self.parent.operations[k].append(i)
        self.operations=[]
        for cn in self.children:
            cn.parent = self.parent
            cn.parent.children.append(cn)
        self.parent.children.remove(self)
        self.parent.mergedcommands.append(self)
        self.parent = None
        self.children = []
        self.filteredout = True

    def node_complete(self):
        for operation in ['COMMAND', 'ENVIRONMENT', 'COMPLETE']:
            buffer_name = '_'+operation.lower()+'_buffer'
            opbuffer = getattr(self, buffer_name, '')
            assert not opbuffer, "Data left in the buffer on COMPLETE\n%s" % (opbuffer)
        self.complete = True
        # Also completes parents  with the same PID/PPID as this one. Usually this is
        # the sub process was by exec without forking a new process.
        sublist = [self]
        while sublist:
            p=sublist.pop(0)
            for i in p.children:
                if i.pid == self.pid and i.ppid == self.ppid:
                    if not p.complete:
#                        log.error("Completing a non-forked exec parent-process: %s: %s [%s]", p.uuid, p.command_path, ' '.join(p.command))
                        i.complete = True
                    sublist.extend(i.children)
                    break
        p=self.parent
        while p and p.pid==self.pid and p.ppid==self.ppid:
            # log.error("Completing a non-forked exec subprocess: %s: %s [%s]", p.uuid, p.command_path, ' '.join(p.command))
            p.complete = True
            p=p.parent

    @classmethod
    def getorcreate(cls, uuid, parent=None, **kwargs):
        if uuid not in cls.progtree:
            prog = cls(uuid, parent, **kwargs)
        else:
            prog = cls.progtree[uuid]
        if parent:
            prog.parent = cls.getorcreate(parent)
            prog.parent.children.append(prog)
        return prog


    @classmethod
    def add_operation(cls, uuid, operation, data, buffer=False):
        if uuid not in cls.progtree:
            cls(uuid)
        node = cls.progtree[uuid]
        buffer_name = '_'+operation.lower()+'_buffer'
        opbuffer = getattr(node, buffer_name, '')
        if opbuffer:
            opbuffer = opbuffer[:-1] + data[1:]
        else:
            opbuffer = opbuffer + data
        data = opbuffer
        setattr(node, buffer_name, '')
        try:
            data = json.loads(opbuffer)
        except json.decoder.JSONDecodeError as e:
            setattr(node, buffer_name, opbuffer)
            return
        if operation in ['COMMAND_PATH', 'READS', 'WRITES', 'UNLINK']:
            data = os.path.normpath(data).replace(WSROOT+'/', '')
        elif operation in ['LINKS']:
            data = [os.path.normpath(i).replace(WSROOT+'/', '') for i in data]
        if operation in ['ENVIRONMENT']:
            data = [i for i in data if not (i.startswith('WISK_') or i.startswith('LD_PRELOAD'))]
            data = [i.split('=',1) for i in data]
            data = dict([(i if len(i)==2 else (i[0], '')) for i in data])
            getattr(node, operation.lower()).update(data)
            node.command_clean()
        elif operation in ['COMMAND', 'CALLS', 'PID', 'PPID', 'WORKING_DIRECTORY']:
            setattr(node, operation.lower(), data)
        elif operation in ['COMMAND_PATH',]:
            setattr(node, operation.lower(), data)
        elif operation in ['COMPLETE']:
            node.node_complete()
        else:
            node.operations.setdefault(operation, [])
            if data not in node.operations[operation]:
                node.operations[operation].append(data)


    @classmethod
    def prune_tree(cls, program=None):
        if program is None:
            program = cls.progtree[WISK_TRACKER_UUID]
        for p in list(program.children):
            if p.uuid == WISK_TRACKER_UUID:
                continue
            if p.command is None:
                p.parent.children.remove(p)
                cls.progtree.pop(p.uuid)
                continue
            cls.prune_tree(p)
        

    @classmethod
    def merge_node(cls, node=None):
        if node is None:
            node = cls.progtree[WISK_TRACKER_UUID]
#         if not node.complete:
#             log.error('Incomplete Command: %s %s', node.uuid, node.command_path)
#             WISK_INSIGHT_FILE.write('UUIDS: %s\n' % (','.join(getuuidsabove(node.uuid))))
#             WISK_INSIGHT_FILE.write('Incomplete Command: %s %s [%s]\n' % (node.uuid, node.command_path, ' '.join(node.command)))
#             for k, v in node.environment.items():
#                 if k in ['LD_PRELOAD'] or k.startswith('WISK_'):
#                     continue
#                 WISK_INSIGHT_FILE.write('%s="%s"\n' % (k, v))
#             WISK_INSIGHT_FILE.write('\n%s\n\n' % (' '.join(node.command)))
        for cn in list(node.children):
            cls.merge_node(cn)
        node.checkfortooltypeinherit()
        if node.checkformerge():
            node.domerge()
            

    @classmethod        
    def show_nodes(cls, ofile=sys.stdout, node=None, fields=None):
        if node is None:
            node = cls.progtree[WISK_TRACKER_UUID].children[0]
        fields = fields or CONFIG['DEFAULT']['filterfields']
        if isinstance(ofile, str):
            ofile = open(ofile, 'w')
        json.dump(node, ofile, default=partial(tojson, fields=fields), indent=2, sort_keys=True)

      
    @classmethod        
    def compact_environment(cls, program=None):
        if program is None:
            program = cls.progtree[WISK_TRACKER_UUID]
        for p in program.children:
            cls.compact_environment(p)
            parent = p.parent
            p.environment = {k:v for k,v in p.environment.items() if k not in parent.environment}            


    @classmethod        
    def expand_environment(cls, program=None):
        if program is None:
            program = cls.progtree[WISK_TRACKER_UUID]
        for p in program.children:
            parent = p.parent
            p.environment = {**p.environment, **parent.environment}            
            cls.expand_environment(p)
        


def uuid_list_complete(args, root):
    rv = True 
    for i in list(args.extract): 
        for j in getuuidsabove(i):
            if j not in args.extract:
#                print('Missing: %s' % j)
                args.extract.append(j)
                rv = False
    return rv


@utils.timethis
def read_raw_data(args, debug=False):
    print('Reading Raw Data: %s' %(args.trackfile + '.raw'))
    ifile = open(args.trackfile + '.raw')
    extractfile=None
    if args.extract:
        print('Extracting Filtered Data: %s, UUIDs: %s' % (args.trackfile+'.ext.raw', args.extract))
        extractfile = open(args.trackfile+'.ext.raw', 'w')
    root = ProgramNode(WISK_TRACKER_UUID).complete=True
    count = 0
    line = 0
#    for l in ifile.readlines():
    while True:
        l = ifile.readline()
        if not l:
            break
        line += 1
        if not debug:
            print("\rReading %d ..." % (line), end='')
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
        if debug and operation in ['CALLS', 'COMMAND', 'COMPLETE']:
            print(l.rstrip())
        if extractfile and uuid in args.extract:
            log.debug('Extracting: [%s]', l)
            extractfile.write(l)
    if args.extract and not uuid_list_complete(args, root):
        extractfile.close()
        root=None
        ProgramNode.progtree.clear()
        rv = read_raw_data(args, debug=True) 
        print('\nSuggested Full Extract Option: -extract=%s\n' % (','.join(args.extract)))
        return
    return

@utils.timethis
def clean_data(args, root=None):
    if root is None:
        root = ProgramNode.progtree[WISK_TRACKER_UUID]
    print('Cleaning Data')
    ProgramNode.prune_tree()
    ProgramNode.compact_environment()
    print('Writing cleanedup full dependency data to %s' % (args.trackfile+'.dep'))
    ProgramNode.show_nodes(args.trackfile+'.dep', fields=FIELDS_ALL)
    ProgramNode.expand_environment()

@utils.timethis
def extract_commands(args, root=None):
    if root is None:
        root = ProgramNode.progtree[WISK_TRACKER_UUID]
    print('Extracting Top level commands ...')
    ProgramNode.merge_node()
    ProgramNode.compact_environment()
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

def getfiltermask(args):
    if not args.filter:
        return 0xFFFFFFFF
    print(args.filter)
    mask = 0x0
    for k in args.filter.split(','):
        try:
            i = WISK_EVENTFILTERS.index(k)
            mask = mask | (1 << i)
        except ValueError:
            log.error("Unrecognized event filter value: %s. Supported values are %s", k, ', '.join(WISK_EVENTFILTERS))
            exit(1)
    return mask

@utils.timethis
def tracked_run(args):
    global WISK_DEBUGLOG
    retval = None
    log.info('WISK Verbosity: %d', args.verbose-1)
    cmdenv = {k:v for k,v in os.environ.items() if k in args.environ}
    log.debug('LATEST_LIB_DIR: %s', env.LATEST_LIB_DIR)
    WISK_DEBUGLOG = os.path.join(args.wsroot, WISK_DEBUGLOG)
    open(WISK_DEBUGLOG, 'w').close()
    cmdenv.update({
        'LD_LIBRARY_PATH': ':'.join(['', os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib32'),
                                     os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib64')]),
#        'LD_LIBRARY_PATH': ':'.join([os.path.join(os.path.dirname(env.INSTALL_LIB_DIR), 'lib32')]),
        'LD_PRELOAD': 'libwisktrack.so',
        'WISK_TRACKER_PIPE': WISK_TRACKER_PIPE,
        'WISK_TRACKER_PIPE_FD': '-1',
        'WISK_TRACKER_UUID': WISK_TRACKER_UUID,
#         'WISK_TRACKER_DEBUGLOG_FD': '-1',
        'WISK_TRACKER_DEBUGLEVEL': ('%d' % (args.verbose)),
        'WISK_TRACKER_EVENTFILTER': '%d'%(getfiltermask(args))})
    
    if args.trace:
        cmdenv['WISK_TRACKER_DEBUGLOG'] = WISK_DEBUGLOG
        cmdenv['WISK_TRACKER_DEBUGLOG_FD'] = '-1'
    else:
        cmdenv['WISK_TRACKER_DEBUGLOG_FD'] = '2'
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
    if args.extract or args.clean:
        read_raw_data(args)
    if args.clean:
        clean_data(args)
        extract_commands(args)
    if args.show:
        filetoshow = args.trackfile + ('.cmds' if args.clean else '.raw')
        for line in open(filetoshow):
            print(line, end='')
    configwrite(args.config)
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

def configwrite(configfile):
    global CONFIGPARSER
    for secname, secproxy in CONFIGPARSER.items():
        for k, v in secproxy.items():
            dtype = CONFIG_DATATYPES.get(secname, {}).get(k, None)
            v = CONFIG[secname][k]
            if dtype is not None:
                for do_op in dtype[1]:
                    v = do_op(v)
            CONFIGPARSER[secname][k] = v
    print('Saving current config to %s' % (configfile+'.saved'))
    print('Updating config %s' % (configfile))
    if os.path.exists(configfile):
        shutil.copy(configfile, configfile+'.saved')
    with open(configfile, 'w') as configfile:
        CONFIGPARSER.write(configfile)

def configparse(configfile):
    ''' Configuration File '''
    global CONFIG
    global CONFIGPARSER
    CONFIGPARSER = configparser.ConfigParser(defaults=CONFIG_DEFAULTS, interpolation=configparser.BasicInterpolation())
    print('Reading Config: %s' % (configfile))
    CONFIGPARSER.read(configfile if os.path.exists(configfile) else os.path.join(env.CONFIG_DIR, 'wisk_parser.cfg'))
    CONFIG = {}
    for secname, secproxy in CONFIGPARSER.items():
        CONFIG[secname] = {}
        for k, v in secproxy.items():
            CONFIG[secname][k] = v.strip()
            dtype = CONFIG_DATATYPES.get(secname, {}).get(k, None)
            if dtype is None:
                continue                
            for do_op in dtype[0]:
                CONFIG[secname][k] = do_op(CONFIG[secname][k])

def insight_init(args):
    global WISK_INSIGHT_FILE
    WISK_INSIGHT_FILE = open(os.path.join(args.wsroot, WISK_INSIGHT_FILENAME) if os.path.exists(args.wsroot) else WISK_INSIGHT_FILENAME, 'w')

def partialparse(parser):
    ''' Parse args until first unknown '''
    global WISK_TRACKER_PIPE
    global WISK_ARGS

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
    if not isinstance(args.extract, list):
        args.extract = args.extract.split(',')
    if args.extract and WISK_TRACKER_UUID not in args.extract:
        args.extract.insert(0, WISK_TRACKER_UUID)
    WISK_ARGS = args
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
        parser.add_argument('-trace', '--trace', action='store_true', default=False, help="Trace to file")
        parser.add_argument('-wsroot', '--wsroot', type=str, default=os.getcwd(), help="Workspace Root")
        parser.add_argument('-trackfile', '--trackfile', type=str, default=os.path.join(os.getcwd(), WISK_DEPDATA), help="Where to output the tracking data")
        parser.add_argument('-config', '--config', type=str, default=WISK_PARSER_CFG, help="WISK Parser Configration")
        parser.add_argument('-extract', '--extract', type=str, default=[], help="Extract specific nodes Parser Configration")
        parser.add_argument('-environ', '--environ', type=str, action='append',
                            default=['PATH', 'LOGNAME', 'LANGUAGE', 'HOME', 'USER', 'SHELL'],
                            help='Environment variables for carry forward')
        parser.add_argument('-filter', '--filter', type=str, default=None, help='Filtered list of events to track')

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
        logwarn = log.warn if init else print
        logwarn(traceback.format_exc())
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
