'''
Created on Nov 3, 2016

@author: sarvi

@copyright:  2017 Cisco Inc. All rights reserved.
'''

from __future__ import print_function
import os
import sys
import errno
import signal
import json
import platform
import pwd
import time
import shutil
import shlex
import logging
import tempfile
import random
import subprocess
import traceback
import re
from copy import deepcopy
from datetime import datetime
from dateutil.relativedelta import relativedelta
from dateutil.parser import parse
from dccexporter.send_manager import Sender
from dccexporter.message_generator import MessageGenerator  # noqa: E402
from dccexporter.message_validator import is_valid_uuid  # noqa: E402
from dccexporter.sender_utils import find_exception_bucket  # noqa: E402
from common import env

log = logging.getLogger(__name__)  # pylint: disable=locally-disabled, invalid-name

ERROR_BUCKETS = {
    'Exception': 'System',
    'CmdSystemException': 'System',
    'CmdException': 'User',
}

send_2_lumens = Sender(error_buckets=ERROR_BUCKETS)


def lumens_wrap(python_callable):
    def wrapped_execute(*args):
        message_generator = MessageGenerator(env.LUMENS_DATA)
        message_object = message_generator.generate()
        send_2_lumens.logger = logging.getLogger(__name__)
        for arg in sys.argv:
            if arg.startswith('-vvv'):
                break
        else:
            logging.getLogger('kafka.metrics').setLevel(logging.WARNING)
            logging.getLogger('kafka.conn').setLevel(logging.WARNING)
            logging.getLogger('kafka.producer').setLevel(logging.WARNING)
            logging.getLogger('kafka.producer.sender').setLevel(logging.WARNING)
            logging.getLogger('kafka.protocol').setLevel(logging.WARNING)
            logging.getLogger('kafka.protocol.parser').setLevel(logging.WARNING)
#             if not is_valid_uuid(env.ENVIRONMENT['uniqueid']):
#                 log.debug('Invalid UUID: %s', env.ENVIRONMENT['uniqueid'])
#             send_2_lumens.service_id = env.ENVIRONMENT['uniqueid']
#             message_object.service_id = send_2_lumens.service_id
        message_object.group_name = 'wisk'
        message_object.submitter_id = env.ENVIRONMENT['username']

        # Update UUID
        if 'uuid' in env.LUMENS_DATA and is_valid_uuid(env.LUMENS_DATA['uuid']):
            send_2_lumens.service_id = env.LUMENS_DATA['uuid']
        message_object.uuid = send_2_lumens.service_id
        if not is_valid_uuid(message_object.uuid):
            send_2_lumens.logger.error('UUID: {} is not a valid UUID format'.
                                       format(message_object.uuid))

        # Excute the python callable and update state
        try:
            tstart = time.time()
            python_callable(*args)
            # Update full service name
            message_object.service_name = '{}.{}.{}'.format(
                                            message_object.service_name,
                                            python_callable.__module__,
                                            env.LUMENS_DATA.pop('service_name', python_callable.__name__))
            tend = time.time()
            message_object.metadata['runtime'] = tend - tstart
            message_object.state = 'SUCCESS'
        except Exception as e:
            t, v, tb = sys.exc_info()
            log.error('Exception: %s', ''.join(traceback.format_exception(t, v, tb)))
            # Update full service name
            message_object.service_name = '{}.{}.{}'.format(
                                            message_object.service_name,
                                            python_callable.__module__,
                                            env.LUMENS_DATA.pop('service_name', python_callable.__name__))
            tend = time.time()
            message_object.metadata['runtime'] = tend - tstart
            message_object.state = 'FAILURE'

            execption_class = str(e.__class__)
            bucket_exception = execption_class[8:len(execption_class)-2]  # noqa: F401, E501
            message_object.metadata['bucket_exception'] = bucket_exception  # noqa: F401, E501
            message_object.metadata['bucket_exception_message'] = str(e)  # noqa: F401, E501

            error_buckets = send_2_lumens.error_buckets
            if 'error_buckets' in env.LUMENS_DATA:
                error_buckets.update(**env.LUMENS_DATA['error_buckets'])

            message_object.metadata['bucket'] = find_exception_bucket(
                                                bucket_exception,
                                                error_buckets
                                                )

            try:
                send_2_lumens.generate_message_to_kafka(message_object, env.LUMENS_DATA)
            except Exception as kafkaex:
                log.debug('Lumens Kafka Exception: %s' % kafkaex)
            raise t, v, tb
        try:
            send_2_lumens.generate_message_to_kafka(message_object, env.LUMENS_DATA)
        except Exception as kafkaex:
            log.debug('Lumens Kafka Exception: %s' % kafkaex)
    return wrapped_execute

def formattimedelta(deltatime):
    ''' Format Delta time in hours:min:secs'''
    hours, rem = divmod(deltatime, 3600)
    minutes, seconds = divmod(rem, 60)
    return "%02d:%02d:%05.2f" % (int(hours), int(minutes), seconds)


def timethis(method):
    ''' Decorator to measure time of a function '''
    def timethis_wrapper(*args, **kw):
        ''' Time this decorator support wrapper '''
        tstart = time.time()
        result = method(*args, **kw)
        tend = time.time()
        if '-json' not in sys.argv and '--json' not in sys.argv:
            print("Run time({}) {} {}    ({:05.2f} seconds)".format(
                env.ENVIRONMENT['toolname'], 'SUCCESS' if result == 0 else 'FAILURE', formattimedelta(tend - tstart), tend - tstart))
        return result
    return timethis_wrapper


def timethisapi(method):
    ''' Decorator to measure time of a function '''
    def timethisapi_wrapper(*args, **kw):
        ''' Time this decorator support wrapper '''
        tstart = time.time()
        result = method(*args, **kw)
        tend = time.time()
        # In order for cluster connect to log the
        # cluster_name(cluster api call doesn't have a vserver name)

        log.debug("ONTAP API Run time({}:{}) {}    ({:05.2f} seconds)".format(args[0].config.get('vserver',
                                                                                                 args[0].config.get('cluster_name')),
                                                                              args[1],
                                                                              formattimedelta(tend - tstart), tend - tstart))
        return result
    return timethisapi_wrapper




SHELL_RESERVED = ['basecommand', 'command', 'runcmd', 'sshshell', 'chdir', 'shell', 'getoutput']


class Shell(object):  # pylint: disable=locally-disabled, too-many-instance-attributes
    ''' Shell class '''

    def __init__(self, *args, **kwargs):
        self._check_output = kwargs.pop('check_output', None)
        self._interactive = kwargs.pop('interactive', not self._check_output)
        self._shell = kwargs.pop('shell', None)
        self._donotprint = kwargs.pop('donotprint', False)
        self._cmds = []
        self._args = {}
        self._stack = [(list(args), kwargs)]
        self._script = []
        self._last_returncode = 0
        self._last_cmd = None
        self._last_output = None

    def __getattribute__(self, name):
        if name.startswith('_') or name in SHELL_RESERVED:
            return object.__getattribute__(self, name)
        else:
            self._cmds.append(name)
            return self

    def __enter__(self):
        log.debug('Entering Context')
        cmdcxt, argcxt = self._stack[-1]
        cmdcxt, argcxt = deepcopy(cmdcxt), deepcopy(argcxt)
        cmdcxt.extend(self._cmds)
        argcxt.update(self._args)
        self._stack.append((cmdcxt, argcxt))
        self._cmds = []
        self._args = {}
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        log.debug('Exiting context')
        _, argcxt = self._stack.pop()
        shell = argcxt.get('shell', False)
        endquote = argcxt.get('_quote', None)
        collect = argcxt.get('_collect', False)
        _, newargcxt = self._stack[-1]
        if collect and not newargcxt.get('_collect', False):
            if endquote:
                self._cmds.append(endquote)
            self._args['shell'] = shell
            return self()

    def __call__(self, *args, **kwargs):  # pylint: disable=locally-disabled, too-many-branches, too-many-statements, too-many-locals
        cmdcxt, argcxt = self._stack[-1]
#        cmd = [pipes.quote(i) for i in cmdcxt + self._cmds + list(args)]
        cmd = [i for i in cmdcxt + self._cmds + list(args)]
        for k, v in kwargs.iteritems():
            if k.startswith('_'):
                continue
            cmd.append(('-%s' % k) if len(k) == 1 else ('--%s' % k))
            cmd.append('%s' % v)
        popenargs = dict(argcxt)
        popenargs.update(self._args)
        if popenargs.get('_collect', False):
            cmd = ' '.join(cmd) if isinstance(cmd, list) else cmd
            self._script.append(cmd)
            self._cmds = []
            self._args = {}
            return cmd
        script = self._script
        self._script = []
        self._cmds = []
        self._args = {}
        ignore_exception = popenargs.pop('_ignore_exception', False)
        ignore_returncode = popenargs.pop('_ignore_returncode', [0])
        popenargs.setdefault('shell', self._shell)
        if popenargs['shell']:
            cmds = ['\n'.join(script + [' '.join(cmd)])]
            dispcmds = '\n'.join(cmds)
        else:
            cmds = script + cmd
            dispcmds = ' '.join(cmds)
        print('Executing: %s' % (dispcmds))
        log.debug('%s', popenargs)
        if self._check_output and not self._interactive:
            self._last_returncode = 0
            try:
                self._last_output = subprocess.check_output(cmds, **popenargs)
            except subprocess.CalledProcessError as e:
                self._last_cmd = e.cmd
                self._last_returncode = e.returncode
                self._last_output = e.output
                print('Error(ReturnCode: %d): %s' % (e.returncode, e.cmd))
                if self._check_output and not self._donotprint:
                    log.debug(e.output)
                if not ignore_exception and e.returncode not in ignore_returncode:
                    raise
            if not self._donotprint:
                log.debug(self._last_output)
            return self._last_output
        elif not self._check_output and self._interactive:
            try:
                self._last_returncode = subprocess.check_call(cmds, **popenargs)  # pylint: disable=locally-disabled, R0204
            except subprocess.CalledProcessError as e:
                self._last_cmd = e.cmd
                self._last_returncode = e.returncode
                print('Error(ReturnCode: %d): %s' % (e.returncode, e.cmd))
                if not ignore_exception and e.returncode not in ignore_returncode:
                    raise
            return self._last_returncode
        elif popenargs['shell']:
            tmpfile = tempfile.NamedTemporaryFile('w')
            cmds = [i.strip("'") for i in cmds]
            cmds[0] += ' |& /usr/bin/tee %s ; exit ${PIPESTATUS[0]}' % tmpfile.name
            # cmds[0] = ('exec &> >(/usr/bin/tee "%s") \n ' % tmpfile.name) + cmds[0]
            self._last_returncode = 0
            try:
                self._last_returncode = subprocess.check_call(*cmds, **popenargs)  # pylint: disable=locally-disabled, R0204
            except subprocess.CalledProcessError as e:
                self._last_cmd = e.cmd
                self._last_returncode = e.returncode
                print('Error(ReturnCode: %d): %s' % (e.returncode, e.cmd))
                if not ignore_exception and e.returncode not in ignore_returncode:
                    self._last_output = open(tmpfile.name).read()
                    e.output = self._last_output
                    log.error('%s', e.output[-1000:])
                    raise
            self._last_output = open(tmpfile.name).read()
            log.debug('%s', self._last_output[-500:])
            return self._last_output
        else:
            raise Exception('Invalid argument combination for check_output/interactive/shell, shell MUST be true')

    def _bakecmd(self, *args, **kwargs):
        ''' Command '''
        self._cmds.extend(args if not kwargs.get('shell', False) else shlex.split(args[0]))
        self._args.update(kwargs)
        return self

    def _chdir(self, dirpath):
        return self.chdir(dirpath)

#     def basecommand(self, *args, **kwargs):
#         ''' Command '''
#         self._cmdbase.extend(args if not kwargs.get('shell', False) else shlex.split(args[0]))
#         self._argbase.update(kwargs)
#         return self

    def basecommand(self, *args, **kwargs):
        ''' Command '''
        self._cmds.extend(args if not kwargs.get('shell', False) else shlex.split(args[0]))
        self._args.update(kwargs)
        cmdcxt, argcxt = self._stack[-1]
        cmdcxt, argcxt = deepcopy(cmdcxt), deepcopy(argcxt)
        cmdcxt.extend(self._cmds)
        argcxt.update(self._args)
        self._stack.append((cmdcxt, argcxt))
        self._cmds = []
        self._args = {}
        return self

    def command(self, *args, **kwargs):
        ''' Command '''
        self._cmds.extend(args if not kwargs.get('shell', False) else [args[0]])
        self._args.update(kwargs)
        return self

    def runcmd(self, *args, **kwargs):
        ''' RunCmd '''
        return self.command(*args, **kwargs)()

    def getoutput(self):
        ''' Command '''
        return self._last_output

    def chdir(self, dirpath):
        ''' Command '''
        _, argcxt = self._stack[-1]
        if argcxt.get('_collect', False):
            self._script.append('cd %s' % dirpath)
        else:
            self._args['cwd'] = dirpath
        return self

    def shell(self):
        ''' Command '''
        self._args['_collect'] = True
        self._args['shell'] = True
        return self

    def sshshell(self, username, hostname, quote='EOF'):
        ''' Command '''
        self._script.append('ssh %s@%s<< %s' % (username, hostname, quote))
        self._args['_quote'] = quote
        self._args['_collect'] = True
        self._args['shell'] = True
        return self


if 'HOME' not in os.environ:
    os.environ['HOME'] = os.path.join('/users', env.ENVIRONMENT['username'])

OSHELL = Shell(env={'PATH': '/bin:/usr/bin:/sbin:/usr/sbin:/usr/cisco/bin:/router/bin',
                    'HOME': os.environ['HOME']}, check_output=True)
ISHELL = Shell(env={'PATH': '/bin:/usr/bin:/sbin:/usr/sbin:/usr/cisco/bin:/router/bin',
                    'HOME': os.environ['HOME']}, interactive=True)
IOSHELL = Shell(env={'PATH': '/bin:/usr/bin:/sbin:/usr/sbin:/usr/cisco/bin:/router/bin',
                     'HOME': os.environ['HOME']}, check_output=True, interactive=True, shell=True)

# Depracated. For backward compatibility
SHELL = Shell(env={'PATH': '/bin:/usr/bin:/sbin:/usr/sbin:/usr/cisco/bin:/router/bin',
                   'HOME': os.environ['HOME']}, check_output=True)
