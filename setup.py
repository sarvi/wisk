#!/usr/bin/env python

'''
Created on Nov 14, 2016

@author: sarvi
'''

from distutils.core import setup

setup(name='WISK Tracker',
      version='1.0',
      description='WISK Tracker(WISK)',
      author='WISK Team',
      author_email='toothless-dev@cisco.com',
      url='http://wwwin-wisk.cisco.com/doc/',
      package_dir={'': 'src'},
      packages=['common',],
      py_modules = ['wisktrack',],
      scripts=['scripts/wisktrack'],
      data_files=[('var/config', ['config/wisk_install_type.cfg',
                                  'config/wisk_common.cfg']),
                  ('lib', ['src/libwisktrack.so']),
#                   ('man/man1', ['doc/_build/man/wisk.1']),
#                   ('man/man2', ['doc/_build/man/wisk.2']),
#                   ('man/man3', ['doc/_build/man/wisk.3']),
#                   ('man/man4', ['doc/_build/man/wisk.4']),
#                   ('man/man7', ['doc/_build/man/wisk.7'])
                  ]
)
