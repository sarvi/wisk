'''
Created on Feb 20, 2017

@author: sarvi

@copyright:  2017 Cisco Inc. All rights reserved.
'''
# pylint: disable=locally-disabled, too-many-lines

from __future__ import print_function

import base64
import collections
import datetime
import hashlib
import logging.handlers
import os
import platform
import pprint
import pwd
import random
import stat
import subprocess
import sys
import threading
import time
import uuid
from configparser import SafeConfigParser, InterpolationMissingOptionError, InterpolationSyntaxError, ParsingError, Error
from io import StringIO

SIMPLE_FMT = '%(message)s'
SIMPLE_FMT_CORR_ID = '%(corr_id)s %(message)s'
VERBOSE_FMT_THREAD = '[%(asctime)s-%(thread)s] %(levelname)s/%(processName)s [%(name)s.%(funcName)s:%(lineno)d] %(message)s'
VERBOSE_FMT_CORR_ID = '[%(asctime)s-%(corr_id)s] %(levelname)s/%(processName)s [%(name)s.%(funcName)s:%(lineno)d] %(message)s'
VERBOSE_FMT_CORR_ID_TASK = '[%(asctime)s-%(corr_id)s] %(levelname)s/%(processName)s/%(task_name)s [%(name)s.%(funcName)s:%(lineno)d] %(message)s'

log = logging.getLogger(__name__)  # pylint: disable=locally-disabled, invalid-name
printlog = logging.getLogger('wisk.print')  # pylint: disable=locally-disabled, invalid-name

ENV_VARIABLE_PREFIX = 'WISK_'
ENV_INIT_DONE = False
LOGGING_INIT_DONE = False

MONITOR_INTERVAL = 3
MONITOR_LOW = 20*1024
MONITOR_RESIZE = 50*1024

MIN_CONFIG = '''
[common]
logdir = %(install_root)s/var/logs/%(toolrootname)s
savelogs = all
fileloglevel = DEBUG
consoleloglevel = WARNING
log_iso8601_dates = False
'''

INSTALL_TYPE_SEARCH = ['%(config_dir)s/wisk_install_type.cfg']

CLIENT_CFG_SEARCH = ['%(config_dir)s/wisk_common.cfg',
                     '%(config_dir)s/wisk_%(Site)s.cfg']

INSTALL_PKG_ROOT = os.path.abspath(os.path.normpath(os.path.join(os.path.dirname(__file__), '../')))
INSTALLED = True if os.path.basename(INSTALL_PKG_ROOT) != 'src' else False
LOCAL_ONLY_CONFIGS = []
if not INSTALLED:
    INSTALL_ROOT = os.path.abspath(os.path.join(INSTALL_PKG_ROOT, '../'))
    INSTANCE_NAME = os.path.basename(INSTALL_ROOT)
    CONFIG_DIR = os.path.abspath(os.path.join(INSTALL_PKG_ROOT, '../config'))
    INSTALL_BIN_DIR = os.path.join(INSTALL_ROOT, 'scripts')
    LATEST_INSTALL_ROOT = INSTALL_ROOT
    LATEST_BIN_DIR = INSTALL_BIN_DIR
    LATEST_LIB_DIR = os.path.abspath(os.path.join(INSTALL_PKG_ROOT, '../src'))
else:
    INSTALL_ROOT = os.path.normpath(os.path.join(INSTALL_PKG_ROOT, '../'))
    INSTANCE_NAME = None
    while os.path.basename(INSTALL_ROOT) != 'lib':
        INSTALL_ROOT = os.path.normpath(os.path.join(INSTALL_ROOT, '../'))
    INSTALL_ROOT = os.path.normpath(os.path.join(INSTALL_ROOT, '../'))
    CONFIG_DIR = os.path.abspath(os.path.join(INSTALL_ROOT, 'var/config'))
    INSTALL_BIN_DIR = os.path.join(INSTALL_ROOT, 'bin')
    LATEST_INSTALL_ROOT = os.path.normpath(os.path.join(INSTALL_ROOT, '../current'))
    LATEST_BIN_DIR = os.path.join(LATEST_INSTALL_ROOT, 'bin')
    LATEST_LIB_DIR = os.path.join(LATEST_INSTALL_ROOT, 'lib')

HOST_UNAME = platform.uname()
UMASK = 0o22  # Set default umask to file permissions of 0644 and directory permissions of 0755
os.umask(UMASK)

ENVIRONMENT = {
    'start_time': datetime.datetime.now().strftime("_%Y%m%d%H%M%S"),
    'pid': str(os.getpid()),
    'username': os.environ['SUDO_USER'] if pwd.getpwuid(os.getuid())[0] == 'root' and 'SUDO_USER' in os.environ else pwd.getpwuid(os.getuid())[0],
    'installed': INSTALLED,
    'instance_name': INSTANCE_NAME,
    'install_pkg_root': INSTALL_PKG_ROOT,
    'install_root': INSTALL_ROOT,
    'config_dir': CONFIG_DIR,
    # 'Site': 'local',
    'OS': HOST_UNAME[0],
    'OS-Version': 'X.XX',
    'CPU': 'x86',
    'Bits': '64',
    'Host OS': HOST_UNAME[0],
    'Host-osver': HOST_UNAME[2],
    'Host Machine arch': HOST_UNAME[4],
    'Host CPU family': HOST_UNAME[5],
    'Host Name': HOST_UNAME[1].split('.')[0],
    'UMASK': UMASK,
    'log_iso8601_dates': False,
}

LUMENS_DATA = {
    'dry_run': False,
    'group_name': 'wisk',
    'data_source': 'cli',
    'submitter_id': ENVIRONMENT['username'],
#    'timestamp': None,
#    'uuid': 'valid UUID',
#    'state': 'SUCCESS/FAILURE/IN PROGRESS',
    'metadata': {}
}

LOCAL = threading.local()


def get_current_correlation_id():
    ''' Retrieve operation_id saved to thread '''
    try:
        return LOCAL.operation_id or ENVIRONMENT.get('uniqueid', None) or '{}.{}'.format(os.getpid(), threading.get_ident())
    except AttributeError:
        return ENVIRONMENT.get('uniqueid', None) or '{}.{}'.format(os.getpid(), threading.get_ident())


class CorrelationIdFilter(logging.Filter):
    ''' Correlation ID Filter '''

    def filter(self, record):
        record.corr_id = get_current_correlation_id()
        return True


class MicroFormatter(logging.Formatter):
    """ Microsecond precision for CLIP """

    def formatTime(self, record, datefmt=None):
        """
        Override date format for microseconds
        """
        converted = self.converter(record.created)
        if datefmt:
            s = time.strftime(datefmt, converted)
        else:
            t = time.strftime("%Y-%m-%d %H:%M:%S", converted)
            if ENVIRONMENT['log_iso8601_dates']:
                s = "%s,%03d" % (t, record.msecs)
            else:
                s = "%s.%06d" % (t, 1000 * record.msecs)
        return s


def env_siteinfo_update():
    ''' Get site information about where the client is running from '''
    if ENVIRONMENT['OS'] == 'Darwin':
        servinfo = {}
        servinfo['Site'] = 'sjc'
        servinfo['DC'] = os.environ.get('MY_DEFAULT_DC', 'sjc5c').lower()
        servinfo['OS-Version'] = '6.20'
    else:
        try:
            out = subprocess.check_output(['/router/bin/servinfo'])
        except subprocess.CalledProcessError as ex:
            log.error('Could not get servinfo for client: %s', ex)
            return {}
        log.debug(out)
        out = out.strip()
        out = [k.decode("utf-8").split(':') for k in out.splitlines()]
        servinfo = {k.strip(): v.strip() for k, v in out}
    # servinfo = {k: v for k, v in servinfo.items() if k not in ENVIRONMENT}
    ENVIRONMENT.update(servinfo)
    LUMENS_DATA['metadata'].update(servinfo)
    env_update = {k.replace(ENV_VARIABLE_PREFIX, ''): v for k, v in os.environ.items() if k.startswith(ENV_VARIABLE_PREFIX)}
    ENVIRONMENT.update(env_update)
    ENVIRONMENT['Host Name'] = ENVIRONMENT['Host Name'].split('.')[0]
    return servinfo


def get_unique_id():
    ''' generate a short unique id for client '''
#     return str(uuid.uuid4())
    intstr = hex(int(time.time() * 10000))[2:] + hex(random.randrange(0, 0xFFFF))[2:]
    return base64.b64encode(intstr.encode(), 'AB'.encode())[:-2].decode('utf-8')


def config_read(cfg_search, doclientcfg=False):  # pylint: disable=locally-disabled, too-many-statements, too-many-branches, too-many-locals
    ''' Read configuration files in a certain order of precedence '''
    config = SafeConfigParser()
    # Consider using initial values as defaults, so are accessible outside common
    # config = SafeConfigParser(ENVIRONMENT)

    ENVIRONMENT['config'] = config
    config.add_section('common')
    config.set('common', 'home_dir', os.path.expanduser('~/'))
    config.set('common', 'installed', str(INSTALLED))
    config.set('common', 'install_pkg_root', INSTALL_PKG_ROOT)
    config.set('common', 'install_root', INSTALL_ROOT)
    config.set('common', 'config_dir', CONFIG_DIR)
    config.set('common', 'instance_name', str(INSTANCE_NAME))
    config.set('common', 'username', ENVIRONMENT['username'])
    config.set('common', 'hostname', ENVIRONMENT['Host Name'])
    config.set('common', 'toolname', ENVIRONMENT['toolname'])
    toolrootname = ENVIRONMENT['toolrootname']
    config.set('common', 'toolrootname', toolrootname)
    config.set('common', 'buildtag', os.environ.get('BUILD_TAG', ''))
    config.set('common', 'buildid', os.environ.get('BUILD_ID', ''))
    config.set('common', 'pid', '%d' % os.getpid())
    config.set('common', 'worker_id', '0')
    config.set('common', 'site', ENVIRONMENT['Site'])
    config.set('common', 'datacenter', ENVIRONMENT['DC'])

    config.add_section('wisk')
    config.set('wisk', 'monitor_interval', str(MONITOR_INTERVAL))
    config.set('wisk', 'monitor_low', str(MONITOR_LOW))
    config.set('wisk', 'monitor_resize', str(MONITOR_RESIZE))

    # Exceptions
    if toolrootname in ['uwsgi']:
        config.set('common', 'widsuffix', '{worker_id}')
    else:
        config.set('common', 'widsuffix', '')

    if ENVIRONMENT['username'] != 'flxclone':
        doclientcfg = True

    ENVIRONMENT['doclientcfg'] = doclientcfg
    if doclientcfg:
#         ENVIRONMENT['uniqueid'] = '%s' % hex(int(time.time() * 10000))[2:]
        ENVIRONMENT['uniqueid'] = get_unique_id()
        LUMENS_DATA['metadata']['uniqueid'] = ENVIRONMENT['uniqueid']
        LOCAL.operation_id = ENVIRONMENT['uniqueid']
        config.set('common', 'uniqueid', ENVIRONMENT['uniqueid'])
        config.set('common', 'log_root', '%(client_log_root)s')
        ENVIRONMENT['logfile'] = '%(logdir)s/%(toolname)s_%(username)s_%(hostname)s_%(uniqueid)s.log'
        config.set('common', 'logfilename', ENVIRONMENT['logfile'])
    else:
        config.set('common', 'uniqueid', '')
        config.set('common', 'log_root', '%(server_log_root)s')
        if toolrootname in ['uwsgi']:
            ENVIRONMENT['logfile'] = '%(logdir)s/%(toolname)s_%(hostname)s_%(widsuffix)s.log'
        else:
            ENVIRONMENT['logfile'] = '%(logdir)s/%(toolname)s_%(hostname)s.log'
        config.set('common', 'logfilename', ENVIRONMENT['logfile'])

    if not ENVIRONMENT['installed']:
        config.set('common', 'logdir', '%(install_root)s/logs/%(toolrootname)s')
    # Read system defaults
    cfgfiles = list(cfg_search)
    found = []
    try:
        # Read the minimum configuration
        config.readfp(StringIO(MIN_CONFIG))

        # read the tool specific list of config files
        cfgfiles = [os.path.expanduser(p) % ENVIRONMENT for p in cfgfiles]
        found.extend(config.read(cfgfiles))

        # Search for install_type config files
        installtypecfg = [os.path.expanduser(p) % ENVIRONMENT for p in INSTALL_TYPE_SEARCH]
        foundinstalltype = config.read(installtypecfg)
        if not foundinstalltype:
            sys.exit('Error: install_type config files not found: %s' % (installtypecfg))
        found.extend(foundinstalltype)
        cfgfiles.extend(installtypecfg)
        if doclientcfg:
            clientcfg = [os.path.join(get_tool_dir(), 'wisk.cfg')]
            found.extend(config.read(clientcfg))
            cfgfiles.extend(clientcfg)
        else:
            servercfgfiles = [os.path.join(get_tool_dir(), 'wisk_server.cfg')]
            if config.get('common', 'install_type', vars={'install_type': None}) != 'local':
                servercfgfiles.append(os.path.join(get_tool_dir(), 'wisk_server_%s.cfg' % config.get('common', 'install_type', None)))
            servercfgfiles = [os.path.expanduser(i) for i in servercfgfiles]
            cfgfiles.extend(servercfgfiles)
            found.extend(config.read(servercfgfiles))

        if config.get('common', 'install_type', vars={'install_type': None}) == 'local':
            localcfgfiles = [os.path.join(get_tool_dir(), 'wisk_local.cfg'),
                             os.path.join(get_tool_dir(), 'wisk_local_%(instance_name)s.cfg')]
            localcfgfiles = [os.path.expanduser(i) for i in localcfgfiles]
            cfgfiles.extend(localcfgfiles)
            found.extend(config.read(localcfgfiles))

        # read the config files specified in WISK_CFG environment variable, used for trouble shooting
        env_cfg = None if INSTALLED else os.environ.get('WISK`_CFG', None)
        if env_cfg is not None:
            cfgfiles.append(env_cfg)
            found.extend(config.read([env_cfg]))

        # Temp code: Remove after CLIP configured
        try:
            ENVIRONMENT['log_iso8601_dates'] = config.getboolean('common', 'log_iso8601_dates')
        except Error:
            ENVIRONMENT['log_iso8601_dates'] = False

    except (ParsingError, OSError) as ex:
        sys.exit('Error reading/parsing Confg files %s : %s' % (cfgfiles, ex))
    ENVIRONMENT['install_type'] = config.get('common', 'install_type', vars={'install_type': None})
    not_found = set(cfgfiles) - set(found)
    ENVIRONMENT['cfg_found'] = found
    ENVIRONMENT['cfg_notfound'] = not_found
    return found, not_found


def config_dict(section='common', options=None):
    """
    Safely return options from config section as dictionary
    """
    cdict = {}
    config = ENVIRONMENT.get('config', None)
    if config and config.has_section(section):
        if options is None:
            cdict = {option: value for option, value in config.items(section)}
        else:
            cdict = {key: config.get(section, key) for key in options if config.has_option(section, key)}

    return cdict


def loglevelint2str(llevel):
    ''' Translate a loglevel string to a loglevel integer '''
    loglevels = {
        logging.DEBUG: 'debug',
        logging.INFO: 'info',
        logging.WARNING: 'warning',
        logging.ERROR: 'error',
        logging.CRITICAL: 'critical'}
    return loglevels.get(llevel, 'notset')


def loglevelstr2int(llevel):
    ''' Translate a loglevel string to a loglevel integer '''
    loglevels = {
        'debug': logging.DEBUG,
        'info': logging.INFO,
        'warn': logging.WARNING,
        'warning': logging.WARNING,
        'error': logging.ERROR,
        'critical': logging.CRITICAL}
    return loglevels.get(llevel, logging.NOTSET)


def logverbosity2str(verbosity):
    ''' Return loglevel as a string '''
    if verbosity > 3:
        verbosity = 3
    return ['ERROR', 'WARNING', 'INFO', 'DEBUG'][verbosity]


def loglevel(verbosity):
    ''' Change log levels if needed '''
    if verbosity == 0:
        llevel = logging.ERROR
    elif verbosity == 1:
        llevel = logging.WARNING
    elif verbosity == 2:
        llevel = logging.INFO
    elif verbosity >= 3:
        llevel = logging.DEBUG
    else:
        llevel = logging.DEBUG
    ENVIRONMENT['consoleloghandler'].setLevel(llevel)


if hasattr(sys, '_getframe'):
    def currentframe():
        ''' Return Frame '''
        # noinspection PyProtectedMember
        return sys._getframe(3)  # pylint: disable=locally-disabled, protected-access
else:
    def currentframe():
        """Return the frame object for the caller's stack frame."""
        try:
            raise Exception
        except Exception:  # pylint: disable=locally-disabled, broad-except
            return sys.exc_info()[2].tb_frame.f_back


_SRCFILE = os.path.normcase(currentframe.__code__.co_filename)  # pylint: disable=locally-disabled, no-member


def findcaller():
    """
    Find the stack frame of the caller so that we can note the source
    file name, line number and function name.
    """
    f = currentframe()
    # On some versions of IronPython, currentframe() returns None if
    # IronPython isn't run with -X:Frames.
    if f is not None:
        f = f.f_back
    rv = "(unknown file)", 0, "(unknown function)"
    while hasattr(f, "f_code"):
        code = f.f_code
        filename = os.path.normcase(code.co_filename)
        if filename != _SRCFILE:
            f = f.f_back
            continue
        if f.f_back is None or not hasattr(f.f_back, "f_code"):
            rv = (code.co_filename, f.f_lineno, '*%s*' % code.co_name)
        else:
            f = f.f_back
            code = f.f_code
            rv = (code.co_filename, f.f_lineno, code.co_name)
        break
    return rv


def genemitmethod(console, origemit):
    ''' generate emit method for handlers '''

    def emitmethod(self, record):
        ''' emit method for handlers '''
        try:
            thr = self.threads.setdefault(record.thread, dict(isprint=False, levelno=None, record=None))
            tisprint = thr.get('isprint')
            tlevelno = thr.get('levelno')
            trecord = thr.get('record')
            isprint = getattr(record, 'isprint', False)
            if tlevelno != record.levelno or tisprint != isprint:
                trecord = thr.get('record')
                if trecord:
                    origemit(self, trecord)
                    thr['record'] = None
            thr['isprint'] = isprint
            thr['levelno'] = record.levelno
            if not isprint:
                return origemit(self, record)
            if console:
                return
            trecord = thr.get('record')
            if trecord is not None:
                trecord.msg += record.msg
            else:
                thr['record'] = record
                record.pathname, record.lineno, record.funcName = findcaller()
            if record.msg.endswith('\n'):
                thr['record'].msg = thr['record'].msg[:-1]
                origemit(self, thr['record'])
                thr['record'] = None
        except (KeyboardInterrupt, SystemExit):
            raise
        except Exception:  # pylint: disable=locally-disabled, broad-except
            self.handleError(record)
    return emitmethod


class StreamHandler(logging.StreamHandler):
    ''' Stream Handler '''
    threads = {}
    emit = genemitmethod(console=True, origemit=logging.StreamHandler.emit)

    def __init__(self, *args, **kwargs):
        if isinstance(kwargs.setdefault('stream', sys.stdout), OutputRedirector):
            kwargs['stream'] = kwargs['stream'].filep

        super(StreamHandler, self).__init__(*args, **kwargs)


class FileHandler(logging.FileHandler):
    ''' File Handler '''
    threads = {}
    emit = genemitmethod(console=False, origemit=logging.FileHandler.emit)


class RotatingFileHandler(logging.handlers.RotatingFileHandler):
    ''' Rotating Filehandler '''
    threads = {}
    emit = genemitmethod(console=False, origemit=logging.handlers.RotatingFileHandler.emit)


class OutputRedirector(object):
    """ Wrapper to redirect stdout or stderr """

    def __init__(self, filep, logmethod):
        ''' Output Redirector init '''
        self.filep = filep
        self.logmethod = logmethod

    def write(self, s):
        ''' Write '''
        self.logmethod(s, extra={'isprint': True})
        self.filep.write(s)

    def origwrite(self, s):
        ''' Write data to stream '''
        self.filep.write(s)

    def writelines(self, lines):
        ''' Writelines '''
        self.logmethod('\n'.join(lines), extra={'isprint': True})
        self.filep.writelines(lines)

    def origwritelines(self, lines):
        ''' Write data to stream '''
        self.filep.writelines(lines)

    def flush(self):
        ''' Flush '''
        self.filep.flush()

    def isatty(self, *args, **kwargs):  # pylint: disable=locally-disabled, unused-argument, no-self-use
        ''' isatty is False when in redirection '''
        return False


def logging_setup(verbosity, corridfilter=None, onlyerrorlogs=False):  # pylint: disable=locally-disabled, too-many-statements, too-many-branches
    ''' Logging Setup '''
    global LOGGING_INIT_DONE  # pylint: disable=locally-disabled, global-statement
    if LOGGING_INIT_DONE:
        return
    LOGGING_INIT_DONE = True
    config = ENVIRONMENT['config']
    if onlyerrorlogs:
        config.set('common', 'savelogs', 'error')

    # All logging is done in UTC for CLIP
    os.environ['TZ'] = 'UTC'
    time.tzset()

    # create file handler which logs with log level specified in config
    ENVIRONMENT['logfile'] = config.get('common', 'logfilename')
    if not os.path.exists(os.path.dirname(ENVIRONMENT['logfile'])):
        dname = os.path.dirname(ENVIRONMENT['logfile'])
        try:
            os.makedirs(dname)
            os.chmod(dname,
                     os.stat(dname).st_mode |
                     stat.S_IRGRP | stat.S_IWGRP | stat.S_IROTH | stat.S_IWOTH)
        except OSError as e:
            sys.exit('Error creating log directory: %s' % e)
    logger = logging.getLogger()
    logger.setLevel(logging.NOTSET)

    verbosity = verbosity if verbosity <= 4 else 4
    clidebuglevel = (5 - verbosity) * 10
    ENVIRONMENT['verbosity'] = verbosity

    # create console handler with a default log level from config and increased by verbosity
    consoleloglevel = loglevelstr2int(config.get('common', 'consoleloglevel').lower())
    if clidebuglevel < consoleloglevel:
        consoleloglevel = clidebuglevel
    ENVIRONMENT['consoleloglevel'] = consoleloglevel

    doclientcfg = ENVIRONMENT.get('doclientcfg', False)
    toolname = ENVIRONMENT.get('toolname', '')

    if not corridfilter:
        corridfilter = CorrelationIdFilter()

    logging.getLogger('').addFilter(corridfilter)
    if logger.handlers:
        ENVIRONMENT['consoleloghandler'] = logger.handlers[0]
    else:
        if toolname.startswith('eventlistener'):
            ENVIRONMENT['consoleloghandler'] = StreamHandler(stream=sys.stderr)
        else:
            ENVIRONMENT['consoleloghandler'] = StreamHandler()
    ENVIRONMENT['consoleloghandler'].addFilter(corridfilter)
    ENVIRONMENT['consoleloghandler'].setLevel(consoleloglevel)
    if doclientcfg:
        if verbosity >= 3:
            cformat = MicroFormatter(VERBOSE_FMT_CORR_ID)
        else:
            cformat = MicroFormatter(SIMPLE_FMT)
    else:
        cformat = MicroFormatter(VERBOSE_FMT_CORR_ID)  # Server

    ENVIRONMENT['consoleloghandler'].setFormatter(cformat)
    logger.addHandler(ENVIRONMENT['consoleloghandler'])

    if doclientcfg:
        fileloglevel = config.get('common', 'fileloglevel').lower()
        ENVIRONMENT['fileloglevel'] = logging.DEBUG if fileloglevel == 'debug' else logging.INFO
        try:
            ENVIRONMENT['fileloghandler'] = FileHandler(ENVIRONMENT['logfile'])
        except OSError as e:
            sys.exit('Error setting up file logging handler: %s, %s' % (ENVIRONMENT['logfile'], e))
        ENVIRONMENT['fileloghandler'].addFilter(corridfilter)
        logger.addHandler(ENVIRONMENT['fileloghandler'])
        ENVIRONMENT['fileloghandler'].setLevel(ENVIRONMENT['fileloglevel'])

        fformat = MicroFormatter(VERBOSE_FMT_CORR_ID)
        ENVIRONMENT['fileloghandler'].setFormatter(fformat)

        sys.stdout = OutputRedirector(sys.stdout, printlog.info)
        sys.stderr = OutputRedirector(sys.stderr, printlog.error)
    elif toolname.startswith('eventlistener') or toolname.startswith('celery-flower'):
        sys.stderr = OutputRedirector(sys.stderr, printlog.info)
        handler = FileHandler(ENVIRONMENT['logfile'])
        handler.addFilter(corridfilter)
        handler.setLevel(logging.DEBUG)
        handler.setFormatter(MicroFormatter(VERBOSE_FMT_CORR_ID))
        logger.addHandler(handler)
        ENVIRONMENT['fileloghandler'] = handler
        ENVIRONMENT['fileloglevel'] = logging.DEBUG

    loglevel(verbosity)

    log.debug('Incoming Environment: %s', pprint.pformat(dict(os.environ), indent=4))
    log.debug('Command line: "%s"', '" "'.join(sys.argv))
    log.debug('Workspace- ID: %s', ENVIRONMENT['workspace_id'])
    log.debug('Config files read from search path: %s', ENVIRONMENT['cfg_found'])
    log.debug('Config files not found in search path: %s', ENVIRONMENT['cfg_notfound'])
    log.debug('Environment: %s', pprint.pformat(ENVIRONMENT, indent=4))


def gettoolname(programname, subcommands=0):
    ''' Get toolname from program name and subcommand '''

    nodashargs = [i for i in sys.argv if not i.startswith('-')]
    for i, v in enumerate(nodashargs):
        if programname in v:
            return '-'.join([programname] + nodashargs[i + 1:i + subcommands + 1])

    return programname


def env_init(toolname, cfgsearch, doclientcfg=False, **kwargs):
    ''' Initialize the environment. Read platform information. Read configuration. Setup logging '''
    global ENV_INIT_DONE  # pylint: disable=locally-disabled, global-statement
    if not ENV_INIT_DONE:
        ENV_INIT_DONE = True
        ENVIRONMENT['toolname'] = toolname
        ENVIRONMENT['toolrootname'] = toolname.split('-')[0]

        ENVIRONMENT.update(kwargs)
        env_siteinfo_update()
        ENVIRONMENT['workspace_path'] = workspace_path()
        ENVIRONMENT['workspace_id'] = workspace_id()
        ENVIRONMENT['workspace_guid'] = workspace_guid()
        config_read(cfgsearch, doclientcfg)
        celmajor, _ = ENVIRONMENT['OS-Version'].strip().split('.')
        if int(celmajor) < 6:
            sys.exit('ERROR: Tooling requires CEL 6 or above')

    return ENV_INIT_DONE


def exit_clean(err):
    ''' Cleanup and save logs if error or needed before exiting '''
    if err is None:
        err = 0
    try:
        logfile = ENVIRONMENT['logfile']
        config = ENVIRONMENT['config']
        tlogdir = os.path.expanduser(config.get('common', 'logdir'))
        savelogs = config.get('common', 'savelogs').lower()
    except (InterpolationMissingOptionError, InterpolationSyntaxError) as ex:
        log.info(ex)
        return err
    except KeyError as ex:
        return err
    if err != 0 or savelogs == 'all':
        if not os.path.exists(logfile):
            log.error('Log file does not exist: %s', logfile)
            return err
        logfilename = os.path.basename(logfile)
        tlogfilename = list(os.path.splitext(logfilename))
        if err:
            tlogfilename.insert(-1, '_error')
            tlogdir0 = tlogdir
            tlogdir = '%s_error' % tlogdir
            if not os.path.exists(tlogdir):
                # Don't create if does not exist, would be wrong ID
                log.error('Error Log dir does not exist: %s', tlogdir)
                tlogdir = tlogdir0
        tlogfile = os.path.join(tlogdir, ''.join(tlogfilename))
        logmsg = log.info if err == 0 else print
        if logfile != tlogfile:
            try:
                # Try to create hardlink
                os.link(logfile, tlogfile)
            except OSError as e:
                log.warning('Error with hard link of %s to %s: %s', logfile, tlogfile, e)
                try:
                    # Try to create symlink
                    os.symlink(logfile, tlogfile)
                except OSError as e:
                    log.error('Error creating symlink of %s to %s: %s', logfile, tlogfile, e)
                    return err
        try:
            os.chmod(tlogfile,
                     os.stat(tlogfile).st_mode |
                     stat.S_IRGRP | stat.S_IWGRP | stat.S_IROTH | stat.S_IWOTH)
        except OSError as e:
            log.error('Error updating permissions on log files: %s', e)
        logmsg('Detailed Logs at %s' % tlogfile)
    else:
        log.debug('Save Logs on Error is Enabled. Removing success Logfile: %s', logfile)
        if os.path.exists(logfile):
            os.remove(logfile)
    return err


def get_relversion(client):
    ''' Get the release version of the current software instance '''
    if 'rel-version' in ENVIRONMENT:
        return ENVIRONMENT['rel-version']
    if INSTALLED:
        ENVIRONMENT['rel-version'] = os.path.basename(os.path.realpath(INSTALL_ROOT))
        return ENVIRONMENT['rel-version']
    try:
        prefix = 'WISK_CLIENT' if client else 'WISK_SERVER'
        ENVIRONMENT['rel-version'] = subprocess.check_output(
            ['git', 'describe', '--tags', '--match', '%s_[--9A-Z]*' % prefix, '--always', '--abbrev=4', 'HEAD'],
            stderr=subprocess.STDOUT).decode('utf-8').strip().replace('_', '.')
    except subprocess.CalledProcessError as ex:
        log.debug('Could not get servinfo for client: %s', ex)
        ENVIRONMENT['rel-version'] = 'unknown-development-version'
    return ENVIRONMENT['rel-version']


def get_reldatetime():
    ''' Get the release date of the current software instance '''
    if 'rel-datetime' in ENVIRONMENT:
        return ENVIRONMENT['rel-datetime']
    ENVIRONMENT['rel-datetime'] = time.ctime(os.path.getmtime(os.path.realpath(INSTALL_ROOT)))
    return ENVIRONMENT['rel-datetime']


def get_verbosity(default=None):
    ''' Get verbosity from the command line '''
    if 'verbosity' in ENVIRONMENT:
        return ENVIRONMENT['verbosity']
    for v in ['-v', '-verbose', '-verbosity', '--verbose', '--verbosity']:
        if v in sys.argv:
            i = sys.argv.index(v)
            try:
                return ENVIRONMENT.setdefault('verbosity', int(sys.argv[i + 1]))
            except (ValueError, IndexError):
                return ENVIRONMENT.setdefault('verbosity', 1)
        for i, o in enumerate(sys.argv):
            if o.startswith(v):
                if '=' in o:
                    _, rv = o.split('=')
                    return ENVIRONMENT.setdefault('verbosity', int(rv))
                elif o.startswith('-vv'):
                    return ENVIRONMENT.setdefault('verbosity', int(len(o[1:])))
                else:
                    rv = o.replace(v, '')
                    return ENVIRONMENT.setdefault('verbosity', int(rv))
    return ENVIRONMENT.setdefault('verbosity', default)


def workspace_path():
    ''' Recognize the root of the workspace if you are in one '''
    wspath = os.path.normpath(os.getcwd())
    try:
        while '.git' not in os.listdir(wspath):
            wspath = os.path.split(wspath)[0]
            if wspath == '/':
                return None
    except OSError:
        return '/router/bin'
    return wspath


def workspace_id():
    ''' Generate a workspace id unique to the username, host and workspace path'''
    wid = '%(username)s_%(Host Name)s_%(workspace_path)s' % (ENVIRONMENT)
    wid = wid.replace('/', '.')
    wid = wid.replace('_.', '_')
    return wid


def workspace_guid():
    ''' Generate a workspace guid that can be used for Oracle user name '''

    # Cleaning up DB after another user will be challenging if workspace_id varies with user calling this routine
    ws_id = '%(Host Name)s_%(workspace_path)s' % ENVIRONMENT  # Should be unique across cisco

    # Add the owner of the workspace so more easily identified
    ws_stat = os.stat(INSTALL_ROOT)
    ws_uid = ws_stat.st_uid
    ws_owner = pwd.getpwuid(ws_uid).pw_name

    slug = hashlib.sha1(ws_id.encode('utf-8')).hexdigest()[:10]
    guid = '%s_%s' % (ws_owner, slug)

    return guid


def get_tool_dir():
    ''' Get .wisk dir '''

    return os.environ.get('TOOL_DIR', os.path.join(os.path.expanduser('~'), '.wisk'))


def get_correlation_header():
    """ Retrieve uniqueid / workspace_guid from config """
    uniqueid = ENVIRONMENT.get('uniqueid', '')
    if not uniqueid:
        uniqueid = workspace_guid()
        log.error('Missing uniqueid - using workspace_guid: %s', uniqueid)

    log.info('Setting HTTP_X_OPERATION_ID header: %s', uniqueid)
    correlation_header = {'X-Operation-ID': uniqueid}

    return correlation_header


def get_team_from_local():
    ''' Get default team from home dir ~/.wisk/wisk.cfg '''

    config = ENVIRONMENT['config']
    if config.has_option('common', 'team'):
        return config.get('common', 'team', '')

    return None


def cliprint(obj, cindent=4, indent=4):
    ''' Display Dictionaries, Lists and Scalars in an indented fashion for CLI display'''
    retval = ''
    if isinstance(obj, dict) or isinstance(obj, collections.OrderedDict):
        width = max([len(i) for i in obj.keys()]) + 1
        for k in sorted(obj.keys()):
            retval = retval + '\n{}{:<{}} '.format(' ' * cindent, k + ':', width) + cliprint(obj[k], cindent + indent)
        return retval
    elif isinstance(obj, list):
        retval = retval + '['
        sep = ''
        for v in obj:
            retval = retval + sep + '{0}'.format(' ' * cindent) + cliprint(v, cindent)
            sep = ','
        retval = retval + ']'
        return retval
    else:
        retval = ' %s' % obj
        retval = retval.replace('\n', '\n' + ' ' * cindent)
        return retval
