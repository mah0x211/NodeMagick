import Options
import Environment
import sys, os, shutil, glob
from os import unlink, symlink, popen
from os.path import join, dirname, abspath, normpath

srcdir = '.'
blddir = 'build'
VERSION = '0.5.0'

def set_options(opt):
	opt.tool_options('compiler_cxx')
	opt.tool_options('compiler_cc')
	opt.tool_options('misc')

def configure(conf):
  conf.check_tool('compiler_cxx')
  if not conf.env.CXX: conf.fatal('c++ compiler not found')
  conf.check_tool('compiler_cc')
  if not conf.env.CC: conf.fatal('c compiler not found')
  conf.check_tool('node_addon')
  # o = Options.options
  conf.check_cfg(package='ImageMagick', uselib_store='LIBIMAGEMAGICK', args='--cflags --libs', mandatory=True)
  # check libs
  conf.check_cc( lib='MagickWand', uselib_store='LIBIMAGEMAGICK', mandatory=True )

def build(bld):
	# print 'build'
	t = bld.new_task_gen('cxx', 'shlib', 'node_addon')
	t.target = 'NodeMagick'
	t.source = './src/NodeMagick.cc'
	t.includes = ['.']
	t.uselib = ['LIBIMAGEMAGICK']
	t.lib = ['MagickWand']

def shutdown(ctx):
	pass
