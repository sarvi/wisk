'''
CmdException -- cmd specific exception


@author:     ldu

@copyright:  2017 Cisco Inc. All rights reserved.

@license:    license

@contact:    ldu@cisco.com
@deffield    updated: Updated
'''


class CmdException(Exception):
    ''' Raise for cmd specific exception '''
    def __init__(self, message, *args):
        self.message = 'ERROR: ' + message
        super(CmdException, self).__init__(message, *args)

class CmdSystemException(Exception):
    ''' Raise for cmd specific exception '''
    def __init__(self, message, *args):
        self.message = 'ERROR: ' + message
        super(CmdSystemException, self).__init__(message, *args)
