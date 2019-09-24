'''
Created on Dec 21, 2017

@author: sarvi
'''

from __future__ import print_function

import os
import logging
from common import env

log = logging.getLogger(__name__)  # pylint: disable=locally-disabled, invalid-name


def env_init(*args, **kwargs):
    ''' Client Init '''
    ospaths = os.environ['PATH'].split(':')
    if '/sbin' not in ospaths:
        ospaths.insert(0, '/sbin')
    if '/usr/sbin' not in ospaths:
        ospaths.insert(0, '/usr/sbin')
    os.environ['PATH'] = ':'.join(ospaths)
    return env.env_init(*args, **kwargs)
