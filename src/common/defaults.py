'''
Created on Jan 13, 2017

@author: sarvi
'''
import os.path
from common import env

DEPLOYMENT_TYPES_MAP = {
    'LOCAL': ('', 'Test'),
    'DEV': ('-dev', 'Dev'),
    'TEST': ('test-dev', 'Test'),
    'STAGE': ('-stg', 'Stage'),
    'PROD': ('', 'Prod'),
}

__programname__ = 'django-wit-server'
__programpath__ = 'undefined'
__verbosity__ = 0

INSTALL_TYPE = env.ENVIRONMENT.get('install_type', 'local')
DEPLOYMENT_TYPE = INSTALL_TYPE.upper()

DEPLOYMENT_TYPE_MAP = DEPLOYMENT_TYPES_MAP[DEPLOYMENT_TYPE]

MB = 1
GB = 1024 * MB
TB = 1024 * GB

def human_size(qbytes, qunit=2, units=None):
    """ Returns a human readable string reprentation of bytes"""
    if units is None:
        units = [' bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB'][qunit:]
    return str(qbytes) + units[0] if qbytes < 1024 else human_size(qbytes >> 10, 1, units[1:])
